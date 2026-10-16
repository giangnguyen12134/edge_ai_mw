// Copyright 2026 VMX Middleware Team.

#include "mw/media/media_service.h"

#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include "mw/common/frame_types.h"
#include "mw/common/logging.h"
#include "mw/common/time_util.h"
#include "mw/ipc/dbus_names.h"
#include "mw/ipc/introspection.h"

namespace mw {
namespace {

const uint64_t kWatchdogStaleNs = 5ull * 1000000000ull;
const int kInitialBackoffMs = 200;
const int kMaxBackoffMs = 5000;
const int kCaptureTimeoutMs = 200;
const size_t kMaxSubscribers = 32;

uint64_t NowNs() { return MonotonicNs(); }

}  // namespace

MediaService::Counters::Counters()
    : frames_captured(0),
      frames_published(0),
      frames_dropped_no_slot(0),
      ingest_errors(0),
      capture_errors(0),
      source_reopens(0),
      ingest_us_avg(0),
      audio_frames(0),
      audio_reopens(0) {}

MediaService::MediaService(const MediaConfig& config)
    : config_(config),
      native_(config.video.pool_format == "native"),
      stop_(false),
      started_(false),
      state_(kStarting),
      capture_heartbeat_ns_(0),
      audio_heartbeat_ns_(0),
      sequence_(0),
      start_ns_(0) {}

MediaService::~MediaService() { Stop(); }

const char* MediaService::StateName(int state) {
  switch (state) {
    case kStarting:
      return "starting";
    case kStreaming:
      return "streaming";
    case kRecovering:
      return "recovering";
  }
  return "unknown";
}

Status MediaService::Start() {
  if (started_) {
    return FailedPreconditionError("MediaService already started");
  }
  SetMinLogSeverityFromString(config_.log_level);
  MW_RETURN_IF_ERROR(ValidateMediaConfig(config_));
  BusType bus_type;
  MW_RETURN_IF_ERROR(ParseBusType(config_.bus, &bus_type));

  const VideoStreamConfig& video = config_.video;
  MW_RETURN_IF_ERROR(CreateBufferAllocator(video.allocator, &allocator_));
  if (native_) {
    MW_RETURN_IF_ERROR(CreateDmaBufSource(video, &dma_source_));
    wake_fd_.Reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!wake_fd_.valid()) {
      return ErrnoStatus("eventfd", errno);
    }
    dma_source_->SetWakeFd(wake_fd_.get());
    MW_LOG(Info) << "camera: native-format zero-copy capture, "
                 << dma_source_->Describe();
  } else {
    std::shared_ptr<Epoch> epoch = NewEpoch();
    MW_RETURN_IF_ERROR(
        epoch->pool.Init(allocator_.get(), video.pool_slots,
                         FrameFormat::Nv12(video.width, video.height)));
    SetEpoch(epoch);
    MW_RETURN_IF_ERROR(CreateFrameSource(video, &source_));
    MW_RETURN_IF_ERROR(CreateFrameIngest(video.pixel_format, "auto", &ingest_));
    MW_LOG(Info) << "camera: " << source_->Describe()
                 << "; ingest: " << ingest_->Describe();
  }

  const AudioStreamConfig& audio = config_.audio;
  if (audio.enabled) {
    MW_RETURN_IF_ERROR(CreateAudioSource(audio, &audio_));
    MW_RETURN_IF_ERROR(
        ring_.Create(audio.sample_rate, audio.channels, audio.ring_ms));
    MW_LOG(Info) << "microphone: " << audio_->Describe();
  }

  MW_RETURN_IF_ERROR(bus_.Connect(bus_type, dbus::kMediaBusName));
  MW_RETURN_IF_ERROR(
      bus_.AddObject(dbus::kMediaObjectPath, dbus::kMediaInterface,
                     dbus::kMediaIntrospectionXml,
                     [this](const char* member, sd_bus_message* call) {
                       OnMethodCall(member, call);
                     }));
  MW_RETURN_IF_ERROR(bus_.WatchPeerExits([this](const std::string& peer) {
    const std::shared_ptr<Epoch> epoch = CurrentEpoch();
    const size_t removed = epoch != nullptr ? epoch->hub->RemovePeer(peer) : 0;
    if (removed > 0) {
      MW_LOG(Info) << "peer " << peer << " left; reclaimed " << removed
                   << " subscription(s)";
    }
  }));

  start_ns_ = NowNs();
  capture_heartbeat_ns_ = start_ns_;
  audio_heartbeat_ns_ = start_ns_;
  stop_ = false;
  started_ = true;
  MW_RETURN_IF_ERROR(bus_.Start());
  capture_thread_ = std::thread(
      native_ ? &MediaService::NativeCaptureLoop : &MediaService::CaptureLoop,
      this);
  if (audio_ != nullptr) {
    audio_thread_ = std::thread(&MediaService::AudioLoop, this);
  }
  MW_LOG(Info) << "MediaManager up on " << config_.bus << " bus as "
               << dbus::kMediaBusName;
  return Status::Ok();
}

void MediaService::Stop() {
  if (!started_) {
    return;
  }
  stop_ = true;
  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }
  if (audio_thread_.joinable()) {
    audio_thread_.join();
  }
  bus_.Stop();
  started_ = false;
}

bool MediaService::Healthy() const {
  if (!started_ || bus_.connection_lost()) {
    return false;
  }
  // A worker may refresh its heartbeat between reading |now| and the
  // heartbeat itself; unsigned subtraction would then wrap and report a
  // healthy thread as stalled.
  const uint64_t now = NowNs();
  const uint64_t capture = capture_heartbeat_ns_.load();
  if (now > capture && now - capture > kWatchdogStaleNs) {
    return false;
  }
  const uint64_t audio = audio_heartbeat_ns_.load();
  if (audio_ != nullptr && now > audio && now - audio > kWatchdogStaleNs) {
    return false;
  }
  return true;
}

std::shared_ptr<MediaService::Epoch> MediaService::CurrentEpoch() const {
  std::lock_guard<std::mutex> lock(epoch_mutex_);
  return epoch_;
}

void MediaService::SetEpoch(const std::shared_ptr<Epoch>& epoch) {
  std::shared_ptr<Epoch> old;
  {
    std::lock_guard<std::mutex> lock(epoch_mutex_);
    old = epoch_;
    epoch_ = epoch;
  }
  // |old| is released here, outside the lock, once handlers let go of it.
}

std::shared_ptr<MediaService::Epoch> MediaService::NewEpoch() {
  std::shared_ptr<Epoch> epoch(new Epoch());
  epoch->hub.reset(new VideoHub(
      &epoch->pool,
      [this](const VideoSubscriber& subscriber, const FrameInfo& frame) {
        NotifyFrame(subscriber, frame);
      }));
  return epoch;
}

void MediaService::SetState(StreamState state) {
  const int previous = state_.exchange(state);
  if (previous == state) {
    return;
  }
  MW_LOG(Info) << "video stream '" << config_.video.id << "' is "
               << StateName(state);
  const std::string stream = config_.video.id;
  const std::string name = StateName(state);
  bus_.EmitSignal("", dbus::kMediaObjectPath, dbus::kMediaInterface,
                  dbus::kMediaStreamState, [stream, name](sd_bus_message* m) {
                    return sd_bus_message_append(m, "ss", stream.c_str(),
                                                 name.c_str());
                  });
}

void MediaService::SleepInterruptibly(int milliseconds,
                                      std::atomic<uint64_t>* heartbeat) {
  int remaining = milliseconds;
  while (remaining > 0 && !stop_) {
    const int slice = std::min(remaining, 100);
    poll(nullptr, 0, slice);
    remaining -= slice;
    heartbeat->store(NowNs());
  }
}

// ---------------------------------------------------------------------------
// Camera loop
// ---------------------------------------------------------------------------

void MediaService::CaptureLoop() {
  SetCurrentThreadName("mw-capture");
  const std::shared_ptr<Epoch> epoch = CurrentEpoch();
  const uint64_t stall_ns =
      static_cast<uint64_t>(config_.video.stall_timeout_ms) * 1000000ull;
  int backoff_ms = kInitialBackoffMs;
  while (!stop_) {
    Status status = source_->Open();
    if (status.ok()) {
      status = source_->Start();
    }
    if (!status.ok()) {
      MW_LOG(Warning) << "camera not ready (" << status.ToString()
                      << "); retrying in " << backoff_ms << " ms";
      source_->Stop();
      SetState(kRecovering);
      SleepInterruptibly(backoff_ms, &capture_heartbeat_ns_);
      backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
      continue;
    }
    backoff_ms = kInitialBackoffMs;
    SetState(kStreaming);
    uint64_t last_frame_ns = NowNs();
    while (!stop_) {
      capture_heartbeat_ns_.store(NowNs());
      CapturedFrame raw;
      const Status captured = source_->Capture(kCaptureTimeoutMs, &raw);
      if (captured.ok()) {
        last_frame_ns = NowNs();
        ProcessFrame(epoch.get(), raw);
        source_->Release(raw);
        continue;
      }
      if (captured.code() == Status::kTimeout) {
        if (NowNs() - last_frame_ns > stall_ns) {
          MW_LOG(Warning) << "no frame for " << config_.video.stall_timeout_ms
                          << " ms; reopening camera";
          break;
        }
        continue;
      }
      if (captured.code() == Status::kUnavailable) {
        MW_LOG(Warning) << "camera lost: " << captured.ToString();
        break;
      }
      counters_.capture_errors.fetch_add(1);
      MW_LOG(Debug) << "capture error: " << captured.ToString();
    }
    source_->Stop();
    if (!stop_) {
      counters_.source_reopens.fetch_add(1);
      SetState(kRecovering);
    }
  }
  source_->Stop();
}

void MediaService::ProcessFrame(Epoch* epoch, const CapturedFrame& raw) {
  FramePool& pool = epoch->pool;
  counters_.frames_captured.fetch_add(1);
  const int acquired = pool.Acquire();
  if (acquired < 0) {
    counters_.frames_dropped_no_slot.fetch_add(1);
    return;
  }
  const uint32_t slot = static_cast<uint32_t>(acquired);
  const uint64_t t0 = NowNs();
  pool.BeginCpuWrite(slot);
  const Status status = ingest_->Convert(raw, pool.format(), pool.data(slot));
  pool.EndCpuWrite(slot);
  if (!status.ok()) {
    counters_.ingest_errors.fetch_add(1);
    MW_LOG(Debug) << "ingest failed: " << status.ToString();
    pool.Unref(slot);
    return;
  }
  const uint64_t elapsed_us = (NowNs() - t0) / 1000;
  const uint64_t previous = counters_.ingest_us_avg.load();
  counters_.ingest_us_avg.store(
      previous == 0 ? elapsed_us : (previous * 7 + elapsed_us) / 8);

  FrameInfo info;
  info.slot = slot;
  info.sequence = sequence_.fetch_add(1);
  info.timestamp_ns = raw.timestamp_ns;
  info.bytes_used = static_cast<uint32_t>(raw.bytes_used);
  epoch->hub->Publish(info);
  counters_.frames_published.fetch_add(1);
  pool.Unref(slot);  // Drop the producer reference; subscribers hold theirs.
}

// Native mode: the driver fills the pool's dma-bufs; we only juggle indices.
Status MediaService::RequeueFree(Epoch* epoch) {
  while (true) {
    const int slot = epoch->pool.Acquire();  // Reference = "owned by driver".
    if (slot < 0) {
      return Status::Ok();
    }
    MW_RETURN_IF_ERROR(dma_source_->Queue(static_cast<uint32_t>(slot)));
  }
}

Status MediaService::StartNativeSession(std::shared_ptr<Epoch>* out) {
  const VideoStreamConfig& video = config_.video;
  CameraFormat camera;
  MW_RETURN_IF_ERROR(dma_source_->Open(&camera));
  std::vector<ScopedFd> fds;
  uint32_t buffer_length = 0;
  MW_RETURN_IF_ERROR(dma_source_->Prepare(video.pool_slots, allocator_.get(),
                                          &fds, &buffer_length));
  // The bus message carries at most video.pool_slots descriptors; buffers the
  // driver granted beyond that are simply never queued.
  if (fds.size() > video.pool_slots) {
    fds.erase(fds.begin() + video.pool_slots, fds.end());
  }
  FrameFormat format =
      FrameFormat::FromCamera(camera.width, camera.height, camera.fourcc,
                              camera.bytes_per_line, camera.size_image);
  format.buffer_size = buffer_length;
  MW_RETURN_IF_ERROR(ValidateFrameFormat(format));

  std::shared_ptr<Epoch> epoch = NewEpoch();
  MW_RETURN_IF_ERROR(epoch->pool.InitFromFds(std::move(fds), format));
  const int wake = wake_fd_.get();
  epoch->pool.SetFreeNotifier([wake]() {
    const uint64_t one = 1;
    if (write(wake, &one, sizeof(one)) < 0) {
      // EAGAIN: the counter is already non-zero, which is wake-up enough.
    }
  });
  MW_RETURN_IF_ERROR(RequeueFree(epoch.get()));
  MW_RETURN_IF_ERROR(dma_source_->Start());
  MW_LOG(Info) << "camera session: " << dma_source_->Describe() << ", "
               << epoch->pool.slot_count() << " buffers of " << buffer_length
               << " bytes, sizeimage " << camera.size_image;
  *out = epoch;
  return Status::Ok();
}

void MediaService::PublishNative(Epoch* epoch, const DequeuedBuffer& buffer) {
  counters_.frames_captured.fetch_add(1);
  FrameInfo info;
  info.slot = buffer.index;
  info.bytes_used = buffer.bytes_used;
  info.sequence = sequence_.fetch_add(1);
  info.timestamp_ns = buffer.timestamp_ns;
  epoch->hub->Publish(info);
  counters_.frames_published.fetch_add(1);
  // Drop the driver's reference; subscribers hold theirs. With no subscriber
  // the slot is free again at once and the capture loop requeues it.
  epoch->pool.Unref(buffer.index);
}

void MediaService::NativeCaptureLoop() {
  SetCurrentThreadName("mw-capture");
  const uint64_t stall_ns =
      static_cast<uint64_t>(config_.video.stall_timeout_ms) * 1000000ull;
  int backoff_ms = kInitialBackoffMs;
  while (!stop_) {
    std::shared_ptr<Epoch> epoch;
    const Status started = StartNativeSession(&epoch);
    if (!started.ok()) {
      MW_LOG(Warning) << "camera not ready (" << started.ToString()
                      << "); retrying in " << backoff_ms << " ms";
      dma_source_->Stop();
      SetState(kRecovering);
      SleepInterruptibly(backoff_ms, &capture_heartbeat_ns_);
      backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
      continue;
    }
    backoff_ms = kInitialBackoffMs;
    SetEpoch(epoch);  // Before announcing "streaming": clients subscribe then.
    SetState(kStreaming);
    uint64_t last_frame_ns = NowNs();
    while (!stop_) {
      capture_heartbeat_ns_.store(NowNs());
      const Status requeued = RequeueFree(epoch.get());
      if (!requeued.ok()) {
        MW_LOG(Warning) << "requeue failed: " << requeued.ToString();
        break;
      }
      DequeuedBuffer buffer;
      const Status dequeued = dma_source_->Dequeue(kCaptureTimeoutMs, &buffer);
      if (dequeued.ok()) {
        last_frame_ns = NowNs();
        PublishNative(epoch.get(), buffer);
        continue;
      }
      if (dequeued.code() == Status::kTimeout) {
        // No stall while every buffer sits with slow clients: the camera
        // cannot deliver without a queued buffer.
        if (dma_source_->queued_count() == 0) {
          last_frame_ns = NowNs();
        } else if (NowNs() - last_frame_ns > stall_ns) {
          MW_LOG(Warning) << "no frame for " << config_.video.stall_timeout_ms
                          << " ms; reopening camera";
          break;
        }
        continue;
      }
      if (dequeued.code() == Status::kUnavailable) {
        MW_LOG(Warning) << "camera lost: " << dequeued.ToString();
        break;
      }
      counters_.capture_errors.fetch_add(1);
      MW_LOG(Warning) << "capture error: " << dequeued.ToString();
      break;  // Anything else is not recoverable in place.
    }
    dma_source_->Stop();
    if (!stop_) {
      counters_.source_reopens.fetch_add(1);
      SetEpoch(nullptr);
      SetState(kRecovering);
    }
  }
  dma_source_->Stop();
}

void MediaService::NotifyFrame(const VideoSubscriber& subscriber,
                               const FrameInfo& frame) {
  const uint32_t id = subscriber.id;
  const uint32_t slot = frame.slot;
  const uint32_t bytes_used = frame.bytes_used;
  const uint64_t sequence = frame.sequence;
  const uint64_t timestamp_ns = frame.timestamp_ns;
  bus_.EmitSignal(
      subscriber.peer, dbus::kMediaObjectPath, dbus::kMediaInterface,
      dbus::kMediaFrameReady,
      [id, slot, bytes_used, sequence, timestamp_ns](sd_bus_message* m) {
        return sd_bus_message_append(m, "uuutt", id, slot, bytes_used, sequence,
                                     timestamp_ns);
      });
}

// ---------------------------------------------------------------------------
// Audio loop
// ---------------------------------------------------------------------------

void MediaService::AudioLoop() {
  SetCurrentThreadName("mw-audio");
  const AudioStreamConfig& cfg = config_.audio;
  const size_t period_frames =
      static_cast<size_t>(cfg.sample_rate) * cfg.period_ms / 1000;
  std::vector<int16_t> chunk(period_frames * cfg.channels);
  int backoff_ms = kInitialBackoffMs;
  while (!stop_) {
    Status status = audio_->Open();
    if (!status.ok()) {
      MW_LOG(Warning) << "microphone not ready (" << status.ToString()
                      << "); retrying in " << backoff_ms << " ms";
      SleepInterruptibly(backoff_ms, &audio_heartbeat_ns_);
      backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
      continue;
    }
    backoff_ms = kInitialBackoffMs;
    MW_LOG(Info) << "microphone streaming";
    while (!stop_) {
      audio_heartbeat_ns_.store(NowNs());
      status = audio_->Read(chunk.data(), period_frames);
      if (!status.ok()) {
        MW_LOG(Warning) << "microphone: " << status.ToString();
        break;
      }
      ring_.Write(chunk.data(), period_frames);
      counters_.audio_frames.fetch_add(period_frames);
    }
    audio_->Close();
    if (!stop_) {
      counters_.audio_reopens.fetch_add(1);
    }
  }
  audio_->Close();
}

// ---------------------------------------------------------------------------
// D-Bus interface (runs on the bus thread)
// ---------------------------------------------------------------------------

void MediaService::OnMethodCall(const char* member, sd_bus_message* call) {
  const std::string name = member != nullptr ? member : "";
  if (name == dbus::kMediaSubscribeVideo) {
    HandleSubscribeVideo(call);
  } else if (name == dbus::kMediaSubscribeAudio) {
    HandleSubscribeAudio(call);
  } else if (name == dbus::kMediaUnsubscribe) {
    HandleUnsubscribe(call);
  } else if (name == dbus::kMediaReleaseFrame) {
    HandleReleaseFrame(call);
  } else if (name == dbus::kMediaGetStats) {
    HandleGetStats(call);
  } else {
    Bus::ReplyStatus(call, UnimplementedError("unknown method " + name));
  }
}

void MediaService::HandleSubscribeVideo(sd_bus_message* call) {
  const char* client_name = nullptr;
  uint32_t queue_depth = 0;
  if (sd_bus_message_read(call, "su", &client_name, &queue_depth) < 0) {
    Bus::ReplyStatus(call, InvalidArgumentError("expected (s client_name, u "
                                                "queue_depth)"));
    return;
  }
  const std::shared_ptr<Epoch> epoch = CurrentEpoch();
  if (epoch == nullptr) {
    Bus::ReplyStatus(call, UnavailableError("camera is not streaming"));
    return;
  }
  FramePool& pool = epoch->pool;
  if (epoch->hub->subscriber_count() >= kMaxSubscribers) {
    Bus::ReplyStatus(call,
                     ResourceExhaustedError("too many video subscribers"));
    return;
  }
  const uint32_t max_depth = std::max<uint32_t>(1, pool.slot_count() / 2);
  queue_depth = std::max<uint32_t>(1, std::min(queue_depth, max_depth));
  const std::string peer = SenderOf(call);
  const uint32_t id = epoch->hub->Add(
      peer, client_name != nullptr ? client_name : "", queue_depth);

  sd_bus_message* reply = nullptr;
  int r = sd_bus_message_new_method_return(call, &reply);
  const FrameFormat& f = pool.format();
  if (r >= 0) {
    r = sd_bus_message_append(reply, "usuuuuuu", id, config_.video.id.c_str(),
                              f.width, f.height, f.fourcc, f.stride,
                              f.uv_offset, f.buffer_size);
  }
  if (r >= 0) {
    r = sd_bus_message_open_container(reply, 'a', "h");
  }
  for (uint32_t slot = 0; r >= 0 && slot < pool.slot_count(); ++slot) {
    r = AppendFd(reply, pool.fd(slot));
  }
  if (r >= 0) {
    r = sd_bus_message_close_container(reply);
  }
  if (r >= 0) {
    r = sd_bus_send(sd_bus_message_get_bus(call), reply, nullptr);
  }
  if (reply != nullptr) {
    sd_bus_message_unref(reply);
  }
  if (r < 0) {
    epoch->hub->Remove(id, peer);
    Bus::ReplyStatus(call, ErrnoStatus("build SubscribeVideo reply", -r));
    return;
  }
  MW_LOG(Info) << "video subscriber '" << (client_name ? client_name : "")
               << "' " << peer << " id=" << id << " depth=" << queue_depth;
}

void MediaService::HandleSubscribeAudio(sd_bus_message* call) {
  if (audio_ == nullptr) {
    Bus::ReplyStatus(call, UnavailableError("audio is disabled"));
    return;
  }
  const char* client_name = nullptr;
  if (sd_bus_message_read(call, "s", &client_name) < 0) {
    Bus::ReplyStatus(call, InvalidArgumentError("expected (s client_name)"));
    return;
  }
  sd_bus_message* reply = nullptr;
  int r = sd_bus_message_new_method_return(call, &reply);
  if (r >= 0) {
    r = sd_bus_message_append(reply, "suuu", config_.audio.id.c_str(),
                              ring_.sample_rate(), ring_.channels(),
                              ring_.capacity_frames());
  }
  if (r >= 0) {
    r = AppendFd(reply, ring_.fd());
  }
  if (r >= 0) {
    r = sd_bus_send(sd_bus_message_get_bus(call), reply, nullptr);
  }
  if (reply != nullptr) {
    sd_bus_message_unref(reply);
  }
  if (r < 0) {
    Bus::ReplyStatus(call, ErrnoStatus("build SubscribeAudio reply", -r));
    return;
  }
  MW_LOG(Info) << "audio subscriber '" << (client_name ? client_name : "")
               << "' " << SenderOf(call);
}

void MediaService::HandleUnsubscribe(sd_bus_message* call) {
  uint32_t id = 0;
  if (sd_bus_message_read(call, "u", &id) < 0) {
    Bus::ReplyStatus(call, InvalidArgumentError("expected (u id)"));
    return;
  }
  const std::shared_ptr<Epoch> epoch = CurrentEpoch();
  Bus::ReplyStatus(call, epoch != nullptr
                             ? epoch->hub->Remove(id, SenderOf(call))
                             : NotFoundError("no active subscription"));
}

void MediaService::HandleReleaseFrame(sd_bus_message* call) {
  uint32_t id = 0;
  uint32_t slot = 0;
  if (sd_bus_message_read(call, "uu", &id, &slot) < 0) {
    Bus::ReplyStatus(call, InvalidArgumentError("expected (u id, u slot)"));
    return;
  }
  const std::shared_ptr<Epoch> epoch = CurrentEpoch();
  Bus::ReplyStatus(call, epoch != nullptr
                             ? epoch->hub->Release(id, SenderOf(call), slot)
                             : NotFoundError("no active subscription"));
}

void MediaService::HandleGetStats(sd_bus_message* call) {
  std::vector<std::pair<std::string, uint64_t>> stats;
  stats.push_back(
      std::make_pair("state", static_cast<uint64_t>(state_.load())));
  stats.push_back(
      std::make_pair("uptime_s", (NowNs() - start_ns_) / 1000000000ull));
  stats.push_back(
      std::make_pair("frames_captured", counters_.frames_captured.load()));
  stats.push_back(
      std::make_pair("frames_published", counters_.frames_published.load()));
  stats.push_back(std::make_pair("frames_dropped_no_slot",
                                 counters_.frames_dropped_no_slot.load()));
  stats.push_back(
      std::make_pair("ingest_errors", counters_.ingest_errors.load()));
  stats.push_back(
      std::make_pair("capture_errors", counters_.capture_errors.load()));
  stats.push_back(
      std::make_pair("source_reopens", counters_.source_reopens.load()));
  stats.push_back(
      std::make_pair("ingest_us_avg", counters_.ingest_us_avg.load()));
  const std::shared_ptr<Epoch> epoch = CurrentEpoch();
  stats.push_back(std::make_pair(
      "subscribers", epoch != nullptr ? epoch->hub->subscriber_count() : 0));
  stats.push_back(
      std::make_pair("deliveries_skipped",
                     epoch != nullptr ? epoch->hub->skipped_deliveries() : 0));
  // Slots no client holds. In native mode the ones queued to the driver count
  // as free too: they are not in the pool's free list while the camera owns
  // them.
  uint64_t free_slots = epoch != nullptr ? epoch->pool.free_count() : 0;
  if (epoch != nullptr && dma_source_ != nullptr) {
    free_slots += dma_source_->queued_count();
  }
  stats.push_back(std::make_pair("pool_free_slots", free_slots));
  stats.push_back(std::make_pair(
      "pool_slots", epoch != nullptr ? epoch->pool.slot_count() : 0));
  stats.push_back(std::make_pair("native_capture", native_ ? 1 : 0));
  stats.push_back(std::make_pair(
      "bad_frames", dma_source_ != nullptr ? dma_source_->bad_frames() : 0));
  stats.push_back(
      std::make_pair("queued_buffers",
                     dma_source_ != nullptr ? dma_source_->queued_count() : 0));
  stats.push_back(
      std::make_pair("audio_frames", counters_.audio_frames.load()));
  stats.push_back(
      std::make_pair("audio_reopens", counters_.audio_reopens.load()));
  stats.push_back(std::make_pair("audio_xruns",
                                 audio_ != nullptr ? audio_->xrun_count() : 0));

  sd_bus_message* reply = nullptr;
  int r = sd_bus_message_new_method_return(call, &reply);
  if (r >= 0) {
    r = sd_bus_message_open_container(reply, 'a', "(st)");
  }
  for (size_t i = 0; r >= 0 && i < stats.size(); ++i) {
    r = sd_bus_message_append(reply, "(st)", stats[i].first.c_str(),
                              stats[i].second);
  }
  if (r >= 0) {
    r = sd_bus_message_close_container(reply);
  }
  if (r >= 0) {
    r = sd_bus_send(sd_bus_message_get_bus(call), reply, nullptr);
  }
  if (reply != nullptr) {
    sd_bus_message_unref(reply);
  }
  if (r < 0) {
    Bus::ReplyStatus(call, ErrnoStatus("build GetStats reply", -r));
  }
}

}  // namespace mw
