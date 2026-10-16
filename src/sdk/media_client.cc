// Copyright 2026 Genio Middleware Authors.

#include "mw/sdk/media_client.h"

#include <sys/stat.h>
#include <systemd/sd-bus.h>

#include <chrono>

#include "mw/common/dma_buf_mapping.h"
#include "mw/common/logging.h"
#include "mw/ipc/dbus_names.h"

namespace mw {
namespace {

const uint64_t kCallTimeoutUs = 5ull * 1000000ull;
const size_t kMaxPoolSlots = 64;
const std::chrono::milliseconds kRetryDelay(500);
const std::chrono::milliseconds kPollInterval(1000);

}  // namespace

// Everything that must outlive the MediaClient while Frames are still held.
struct MediaClient::Session {
  Session() : id(0), bus(nullptr), alive(true) {}

  uint32_t id;
  std::string stream_id;
  FrameFormat format;
  std::string service_peer;  // Unique name of the MediaManager instance.
  std::vector<ScopedFd> fds;
  std::vector<std::unique_ptr<DmaBufMapping>> maps;

  std::mutex mutex;  // Guards |alive| and use of |bus|.
  Bus* bus;
  bool alive;
};

MediaClient::Options::Options()
    : bus("system"),
      media_bus_name(dbus::kMediaBusName),
      client_name("client"),
      queue_depth(2),
      map_frames(true),
      want_audio(false) {}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

MediaClient::Frame::Frame() {}

MediaClient::Frame::Frame(Frame&& other)
    : session_(std::move(other.session_)), info_(other.info_) {}

MediaClient::Frame& MediaClient::Frame::operator=(Frame&& other) {
  if (this != &other) {
    Release();
    session_ = std::move(other.session_);
    info_ = other.info_;
  }
  return *this;
}

MediaClient::Frame::~Frame() { Release(); }

const FrameFormat& MediaClient::Frame::format() const {
  static const FrameFormat kEmpty;
  return session_ != nullptr ? session_->format : kEmpty;
}

int MediaClient::Frame::dma_buf_fd() const {
  return session_ != nullptr ? session_->fds[info_.slot].get() : -1;
}

const uint8_t* MediaClient::Frame::data() const {
  if (session_ == nullptr || session_->maps.empty()) {
    return nullptr;
  }
  return session_->maps[info_.slot]->data();
}

void MediaClient::Frame::BeginCpuRead() const {
  if (session_ != nullptr && !session_->maps.empty()) {
    session_->maps[info_.slot]->BeginCpuAccess(false);
  }
}

void MediaClient::Frame::EndCpuRead() const {
  if (session_ != nullptr && !session_->maps.empty()) {
    session_->maps[info_.slot]->EndCpuAccess(false);
  }
}

void MediaClient::Frame::Release() {
  if (session_ == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(session_->mutex);
    if (session_->alive && session_->bus != nullptr) {
      const uint32_t id = session_->id;
      const uint32_t slot = info_.slot;
      // Addressed to the unique name so a restarted service never sees it.
      session_->bus->SendNoReply(
          session_->service_peer, dbus::kMediaObjectPath, dbus::kMediaInterface,
          dbus::kMediaReleaseFrame, [id, slot](sd_bus_message* m) {
            return sd_bus_message_append(m, "uu", id, slot);
          });
    }
  }
  session_.reset();
}

// ---------------------------------------------------------------------------
// MediaClient
// ---------------------------------------------------------------------------

MediaClient::MediaClient(const Options& options)
    : options_(options),
      stop_(false),
      needs_subscribe_(true),
      started_(false) {}

MediaClient::~MediaClient() { Stop(); }

static void KillSession(const std::shared_ptr<MediaClient::Session>& session) {
  if (session != nullptr) {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->alive = false;
    session->bus = nullptr;
  }
}

Status MediaClient::Start(const FrameCallback& on_frame,
                          const ConnectionCallback& on_connection) {
  if (started_) {
    return FailedPreconditionError("MediaClient already started");
  }
  if (options_.queue_depth == 0) {
    return InvalidArgumentError("queue_depth must be >= 1");
  }
  on_frame_ = on_frame;
  on_connection_ = on_connection;
  BusType type;
  MW_RETURN_IF_ERROR(ParseBusType(options_.bus, &type));
  MW_RETURN_IF_ERROR(bus_.Connect(type, ""));
  MW_RETURN_IF_ERROR(bus_.AddMatch(
      std::string("type='signal',interface='") + dbus::kMediaInterface +
          "',member='" + dbus::kMediaFrameReady + "'",
      [this](sd_bus_message* m) { OnFrameReady(m); }));
  MW_RETURN_IF_ERROR(bus_.AddMatch(
      std::string("type='signal',interface='") + dbus::kMediaInterface +
          "',member='" + dbus::kMediaStreamState + "'",
      [this](sd_bus_message* m) { OnStreamState(m); }));
  MW_RETURN_IF_ERROR(
      bus_.WatchName(options_.media_bus_name, [this](bool present) {
        if (present) {
          RequestResubscribe();
          return;
        }
        std::shared_ptr<Session> old;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          old = session_;
          session_.reset();
          audio_ring_.reset();
          needs_subscribe_ = true;
        }
        KillSession(old);
        if (old != nullptr) {
          MW_LOG(Warning) << "MediaManager went away; will resubscribe";
          if (on_connection_) {
            on_connection_(false, VideoInfo());
          }
        }
        cv_.notify_all();
      }));
  MW_RETURN_IF_ERROR(bus_.Start());
  started_ = true;
  manager_ = std::thread(&MediaClient::ManagerLoop, this);
  return Status::Ok();
}

void MediaClient::Stop() {
  if (!started_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (manager_.joinable()) {
    manager_.join();
  }
  std::shared_ptr<Session> old;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    old = session_;
    session_.reset();
    audio_ring_.reset();
  }
  if (old != nullptr) {
    const uint32_t id = old->id;
    bus_.SendNoReply(
        old->service_peer, dbus::kMediaObjectPath, dbus::kMediaInterface,
        dbus::kMediaUnsubscribe,
        [id](sd_bus_message* m) { return sd_bus_message_append(m, "u", id); });
  }
  KillSession(old);
  bus_.Stop();
  started_ = false;
}

bool MediaClient::connected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_ != nullptr;
}

std::shared_ptr<AudioRingReader> MediaClient::audio_ring() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return audio_ring_;
}

void MediaClient::RequestResubscribe() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    needs_subscribe_ = true;
  }
  cv_.notify_all();
}

void MediaClient::ManagerLoop() {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, kPollInterval,
                   [this] { return stop_ || needs_subscribe_; });
      if (stop_) {
        return;
      }
      if (!needs_subscribe_) {
        continue;
      }
    }
    std::shared_ptr<Session> fresh;
    std::shared_ptr<AudioRingReader> ring;
    Status status = SubscribeVideo(&fresh);
    if (status.ok() && options_.want_audio) {
      status = SubscribeAudio(&ring);
      if (!status.ok()) {
        MW_LOG(Warning) << "audio unavailable: " << status.ToString();
        status = Status::Ok();  // Video still works.
      }
    }
    if (!status.ok()) {
      MW_LOG(Debug) << "subscribe failed: " << status.ToString();
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, kRetryDelay, [this] { return stop_; });
      continue;
    }
    VideoInfo info;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      audio_ring_ = ring;
      needs_subscribe_ = false;
      info.stream_id = fresh->stream_id;
      info.format = fresh->format;
      info.slot_count = static_cast<uint32_t>(fresh->fds.size());
    }
    MW_LOG(Info) << "subscribed to '" << info.stream_id << "' "
                 << info.format.width << "x" << info.format.height << " ("
                 << info.slot_count << " slots)";
    if (on_connection_) {
      on_connection_(true, info);
    }
  }
}

Status MediaClient::SubscribeVideo(std::shared_ptr<Session>* out) {
  std::shared_ptr<Session> session(new Session());
  const std::string name = options_.client_name;
  const uint32_t depth = options_.queue_depth;
  const bool map_frames = options_.map_frames;
  Bus* bus = &bus_;

  // The session is installed inside the reply reader (bus thread) so that the
  // first FrameReady, dispatched right after the reply, already finds it.
  Status status = bus_.CallMethod(
      options_.media_bus_name, dbus::kMediaObjectPath, dbus::kMediaInterface,
      dbus::kMediaSubscribeVideo,
      [&](sd_bus_message* m) {
        return sd_bus_message_append(m, "su", name.c_str(), depth);
      },
      [&](sd_bus_message* reply) -> Status {
        const char* stream = nullptr;
        FrameFormat f;
        uint32_t id = 0;
        int r = sd_bus_message_read(reply, "usuuuuuu", &id, &stream, &f.width,
                                    &f.height, &f.fourcc, &f.stride,
                                    &f.uv_offset, &f.buffer_size);
        if (r < 0) {
          return InvalidArgumentError("malformed SubscribeVideo reply");
        }
        r = sd_bus_message_enter_container(reply, 'a', "h");
        if (r < 0) {
          return InvalidArgumentError("malformed fd array");
        }
        while (session->fds.size() <= kMaxPoolSlots &&
               sd_bus_message_at_end(reply, 0) == 0) {
          ScopedFd fd;
          MW_RETURN_IF_ERROR(ReadFdDup(reply, &fd));
          session->fds.push_back(std::move(fd));
        }
        sd_bus_message_exit_container(reply);
        if (session->fds.empty() || session->fds.size() > kMaxPoolSlots ||
            !ValidateFrameFormat(f).ok()) {
          return InvalidArgumentError("inconsistent SubscribeVideo reply");
        }
        for (size_t i = 0; i < session->fds.size(); ++i) {
          struct stat st;
          if (fstat(session->fds[i].get(), &st) < 0 ||
              static_cast<uint64_t>(st.st_size) < f.buffer_size) {
            return InvalidArgumentError("pool buffer smaller than advertised");
          }
          if (map_frames) {
            std::unique_ptr<DmaBufMapping> mapping(new DmaBufMapping());
            MW_RETURN_IF_ERROR(
                mapping->Map(session->fds[i].get(), f.buffer_size, false));
            session->maps.push_back(std::move(mapping));
          }
        }
        session->id = id;
        session->stream_id = stream != nullptr ? stream : "";
        session->format = f;
        session->service_peer = SenderOf(reply);
        session->bus = bus;
        std::shared_ptr<Session> previous;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          previous = session_;
          session_ = session;
        }
        KillSession(previous);
        return Status::Ok();
      },
      kCallTimeoutUs);
  if (status.ok()) {
    *out = session;
  }
  return status;
}

Status MediaClient::SubscribeAudio(std::shared_ptr<AudioRingReader>* out) {
  const std::string name = options_.client_name;
  std::shared_ptr<AudioRingReader> ring(new AudioRingReader());
  Status status = bus_.CallMethod(
      options_.media_bus_name, dbus::kMediaObjectPath, dbus::kMediaInterface,
      dbus::kMediaSubscribeAudio,
      [&](sd_bus_message* m) {
        return sd_bus_message_append(m, "s", name.c_str());
      },
      [&](sd_bus_message* reply) -> Status {
        const char* stream = nullptr;
        uint32_t rate = 0, channels = 0, capacity = 0;
        if (sd_bus_message_read(reply, "suuu", &stream, &rate, &channels,
                                &capacity) < 0) {
          return InvalidArgumentError("malformed SubscribeAudio reply");
        }
        ScopedFd fd;
        MW_RETURN_IF_ERROR(ReadFdDup(reply, &fd));
        return ring->Attach(fd.get());
      },
      kCallTimeoutUs);
  if (status.ok()) {
    *out = ring;
  }
  return status;
}

void MediaClient::OnFrameReady(sd_bus_message* message) {
  uint32_t id = 0;
  uint32_t slot = 0;
  uint32_t bytes_used = 0;
  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
  if (sd_bus_message_read(message, "uuutt", &id, &slot, &bytes_used, &sequence,
                          &timestamp_ns) < 0) {
    return;
  }
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    session = session_;
  }
  // Drop signals from an old subscription or from anyone but the service.
  if (session == nullptr || session->id != id ||
      SenderOf(message) != session->service_peer ||
      slot >= session->fds.size() || bytes_used > session->format.buffer_size) {
    return;
  }
  Frame frame;
  frame.session_ = session;
  frame.info_.slot = slot;
  frame.info_.bytes_used = bytes_used;
  frame.info_.sequence = sequence;
  frame.info_.timestamp_ns = timestamp_ns;
  if (on_frame_) {
    on_frame_(std::move(frame));
  }  // Otherwise |frame| is destroyed and the slot goes straight back.
}

void MediaClient::OnStreamState(sd_bus_message* message) {
  const char* stream = nullptr;
  const char* state = nullptr;
  if (sd_bus_message_read(message, "ss", &stream, &state) < 0 ||
      state == nullptr) {
    return;
  }
  const std::string name = state;
  if (name == "streaming") {
    // A new camera session has a new pool: subscribe again, but only when we
    // do not hold a live subscription (we saw "recovering" first).
    bool need = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      need = session_ == nullptr;
    }
    if (need) {
      RequestResubscribe();
    }
    return;
  }
  if (name != "recovering") {
    return;
  }
  std::shared_ptr<Session> old;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_ == nullptr || SenderOf(message) != session_->service_peer) {
      return;
    }
    old = session_;
    session_.reset();
    needs_subscribe_ = true;
  }
  KillSession(old);
  MW_LOG(Info) << "camera session ended; will resubscribe when it is back";
  if (on_connection_) {
    on_connection_(false, VideoInfo());
  }
}

Status MediaClient::GetStats(
    std::vector<std::pair<std::string, uint64_t>>* stats) {
  stats->clear();
  return bus_.CallMethod(
      options_.media_bus_name, dbus::kMediaObjectPath, dbus::kMediaInterface,
      dbus::kMediaGetStats, nullptr,
      [&](sd_bus_message* reply) -> Status {
        if (sd_bus_message_enter_container(reply, 'a', "(st)") < 0) {
          return InvalidArgumentError("malformed GetStats reply");
        }
        while (true) {
          const char* key = nullptr;
          uint64_t value = 0;
          const int r = sd_bus_message_read(reply, "(st)", &key, &value);
          if (r < 0) {
            return InvalidArgumentError("malformed stat entry");
          }
          if (r == 0) {
            break;
          }
          stats->push_back(std::make_pair(std::string(key), value));
        }
        sd_bus_message_exit_container(reply);
        return Status::Ok();
      },
      kCallTimeoutUs);
}

}  // namespace mw
