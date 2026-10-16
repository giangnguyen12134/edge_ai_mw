// Copyright 2026 Genio Middleware Authors.

#include "mw/robotic/robotic_bridge.h"

#include <string.h>

#include <chrono>
#include <vector>

#include "mw/common/logging.h"
#include "mw/common/time_util.h"
#include "mw/robotic/result_json.h"

namespace mw {
namespace {

const size_t kMaxQueuedResults = 256;
const uint64_t kHealthyWithinNs = 10ull * 1000000000ull;
const char kVideoMagic[4] = {'M', 'W', 'V', '1'};
const size_t kVideoHeaderSize = 28;

MediaClient::Options MakeMediaOptions(const RoboticConfig& config) {
  MediaClient::Options options;
  options.bus = config.bus;
  options.media_bus_name = config.media_bus_name;
  options.client_name = "robotic-bridge";
  options.queue_depth = 1;  // Newest frame only; never hoard pool slots.
  options.map_frames = true;
  return options;
}

NpuClient::Options MakeNpuOptions(const RoboticConfig& config) {
  NpuClient::Options options;
  options.bus = config.bus;
  options.npu_bus_name = config.npu_bus_name;
  return options;
}

void PutU16(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void PutU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
  }
}

void PutU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
  }
}

}  // namespace

RoboticBridge::RoboticBridge(const RoboticConfig& config)
    : config_(config),
      npu_(MakeNpuOptions(config)),
      media_(MakeMediaOptions(config)),
      stop_(false),
      started_(false),
      result_heartbeat_ns_(0),
      video_heartbeat_ns_(0),
      results_sent_(0),
      results_dropped_(0),
      frames_sent_(0),
      frames_dropped_(0) {}

RoboticBridge::~RoboticBridge() { Stop(); }

Status RoboticBridge::Start() {
  if (started_) {
    return FailedPreconditionError("RoboticBridge already started");
  }
  MW_RETURN_IF_ERROR(ValidateRoboticConfig(config_));
  if (config_.send_video) {
    MW_RETURN_IF_ERROR(CreateJpegEncoder(config_.encoder, &encoder_));
    MW_LOG(Info) << "video to " << config_.jetson_host << ":"
                 << config_.video_port << " via " << encoder_->Describe();
  }
  stop_ = false;
  started_ = true;
  const uint64_t now = MonotonicNs();
  result_heartbeat_ns_ = now;
  video_heartbeat_ns_ = now;

  if (config_.send_results) {
    result_thread_ = std::thread(&RoboticBridge::ResultLoop, this);
    const Status status =
        npu_.Start([this](const InferenceResult& result) { OnResult(result); });
    if (!status.ok()) {
      Stop();
      return status;
    }
    MW_LOG(Info) << "results to " << config_.jetson_host << ":"
                 << config_.results_port;
  }
  if (config_.send_video) {
    video_thread_ = std::thread(&RoboticBridge::VideoLoop, this);
    const Status status = media_.Start(
        [this](MediaClient::Frame frame) { OnFrame(std::move(frame)); },
        nullptr);
    if (!status.ok()) {
      Stop();
      return status;
    }
  }
  return Status::Ok();
}

void RoboticBridge::Stop() {
  if (!started_) {
    return;
  }
  stop_ = true;
  result_cv_.notify_all();
  frame_cv_.notify_all();
  npu_.Stop();
  media_.Stop();
  if (result_thread_.joinable()) {
    result_thread_.join();
  }
  if (video_thread_.joinable()) {
    video_thread_.join();
  }
  {
    std::shared_ptr<MediaClient::Frame> dropped;
    std::lock_guard<std::mutex> lock(frame_mutex_);
    dropped.swap(latest_frame_);
  }
  started_ = false;
}

bool RoboticBridge::Healthy() const {
  if (!started_) {
    return false;
  }
  const uint64_t now = MonotonicNs();
  const uint64_t result_beat = result_heartbeat_ns_.load();
  const uint64_t video_beat = video_heartbeat_ns_.load();
  const bool results_ok = !config_.send_results || now < result_beat ||
                          now - result_beat < kHealthyWithinNs;
  const bool video_ok = !config_.send_video || now < video_beat ||
                        now - video_beat < kHealthyWithinNs;
  return results_ok && video_ok;
}

bool RoboticBridge::EnsureConnected(TcpSender* sender, uint32_t port,
                                    uint64_t* next_attempt_ns,
                                    const char* what) {
  if (sender->connected()) {
    return true;
  }
  const uint64_t now = MonotonicNs();
  if (now < *next_attempt_ns) {
    return false;
  }
  const Status status = sender->Connect(
      config_.jetson_host, port, static_cast<int>(config_.connect_timeout_ms));
  if (status.ok()) {
    MW_LOG(Info) << what << " link to Jetson up (" << config_.jetson_host << ":"
                 << port << ")";
    return true;
  }
  MW_LOG(Debug) << what << " link to Jetson down: " << status.ToString();
  *next_attempt_ns =
      MonotonicNs() + static_cast<uint64_t>(config_.reconnect_ms) * 1000000ull;
  return false;
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

void RoboticBridge::OnResult(const InferenceResult& result) {
  std::string line = InferenceResultToJson(result);
  line.push_back('\n');
  {
    std::lock_guard<std::mutex> lock(result_mutex_);
    if (result_lines_.size() >= kMaxQueuedResults) {
      result_lines_.pop_front();  // Newest data matters most to a robot.
      ++results_dropped_;
    }
    result_lines_.push_back(line);
  }
  result_cv_.notify_one();
}

void RoboticBridge::ResultLoop() {
  SetCurrentThreadName("mw-bridge-res");
  TcpSender sender;
  uint64_t next_attempt_ns = 0;
  bool was_connected = false;
  while (!stop_) {
    result_heartbeat_ns_ = MonotonicNs();
    std::string line;
    {
      std::unique_lock<std::mutex> lock(result_mutex_);
      result_cv_.wait_for(lock, std::chrono::milliseconds(500),
                          [this] { return stop_ || !result_lines_.empty(); });
      if (stop_) {
        break;
      }
      if (result_lines_.empty()) {
        continue;
      }
      line = result_lines_.front();
      result_lines_.pop_front();
    }
    if (!EnsureConnected(&sender, config_.results_port, &next_attempt_ns,
                         "results")) {
      ++results_dropped_;  // Stale results are useless once the link is back.
      std::lock_guard<std::mutex> lock(result_mutex_);
      results_dropped_ += result_lines_.size();
      result_lines_.clear();
      was_connected = false;
      continue;
    }
    was_connected = true;
    const Status status =
        sender.SendAll(reinterpret_cast<const uint8_t*>(line.data()),
                       line.size(), static_cast<int>(config_.send_timeout_ms));
    if (status.ok()) {
      ++results_sent_;
    } else {
      ++results_dropped_;
      MW_LOG(Warning) << "results link lost: " << status.ToString();
      next_attempt_ns =
          MonotonicNs() +
          static_cast<uint64_t>(config_.reconnect_ms) * 1000000ull;
    }
  }
  (void)was_connected;
}

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------

void RoboticBridge::OnFrame(MediaClient::Frame frame) {
  std::shared_ptr<MediaClient::Frame> fresh(
      new MediaClient::Frame(std::move(frame)));
  std::shared_ptr<MediaClient::Frame> old;
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    old.swap(latest_frame_);
    latest_frame_ = fresh;
  }
  if (old != nullptr) {
    ++frames_dropped_;
  }
  old.reset();
  frame_cv_.notify_one();
}

void RoboticBridge::VideoLoop() {
  SetCurrentThreadName("mw-bridge-vid");
  TcpSender sender;
  uint64_t next_attempt_ns = 0;
  uint64_t last_sent_ns = 0;
  const uint64_t min_interval_ns = 1000000000ull / config_.video_fps;
  std::vector<uint8_t> packet;
  std::vector<uint8_t> jpeg;
  while (!stop_) {
    video_heartbeat_ns_ = MonotonicNs();
    std::shared_ptr<MediaClient::Frame> frame;
    {
      std::unique_lock<std::mutex> lock(frame_mutex_);
      frame_cv_.wait_for(lock, std::chrono::milliseconds(500),
                         [this] { return stop_ || latest_frame_ != nullptr; });
      if (stop_) {
        break;
      }
      frame.swap(latest_frame_);
    }
    if (frame == nullptr || !frame->valid()) {
      continue;
    }
    const uint64_t now = MonotonicNs();
    if (last_sent_ns != 0 && now - last_sent_ns < min_interval_ns) {
      ++frames_dropped_;  // Over the configured frame rate.
      continue;
    }
    if (!EnsureConnected(&sender, config_.video_port, &next_attempt_ns,
                         "video")) {
      ++frames_dropped_;
      continue;
    }
    Status encoded;
    const uint32_t fourcc = frame->format().fourcc;
    if (fourcc == kFourccMjpeg) {
      // The camera already produced JPEG: forward it without re-encoding.
      if (frame->data() == nullptr || frame->size() < 4) {
        encoded = InvalidArgumentError("empty MJPEG frame");
      } else {
        frame->BeginCpuRead();
        jpeg.assign(frame->data(), frame->data() + frame->size());
        frame->EndCpuRead();
      }
    } else if (fourcc == kFourccNv12) {
      frame->BeginCpuRead();
      encoded =
          encoder_->EncodeNv12(frame->data(), frame->format(),
                               static_cast<int>(config_.jpeg_quality), &jpeg);
      frame->EndCpuRead();
    } else {
      encoded = UnimplementedError("cannot encode " + FourccToString(fourcc) +
                                   " frames yet (needs a preprocessing stage)");
    }
    if (!encoded.ok()) {
      MW_LOG(Error) << "encode failed: " << encoded.ToString();
      ++frames_dropped_;
      continue;
    }
    packet.assign(kVideoHeaderSize + jpeg.size(), 0);
    memcpy(&packet[0], kVideoMagic, 4);
    PutU32(&packet[4], static_cast<uint32_t>(jpeg.size()));
    PutU64(&packet[8], frame->info().sequence);
    PutU64(&packet[16], frame->info().timestamp_ns);
    PutU16(&packet[24], frame->format().width);
    PutU16(&packet[26], frame->format().height);
    memcpy(&packet[kVideoHeaderSize], jpeg.data(), jpeg.size());
    frame.reset();  // Give the slot back before the (slow) network send.
    const Status sent =
        sender.SendAll(packet.data(), packet.size(),
                       static_cast<int>(config_.send_timeout_ms));
    if (sent.ok()) {
      ++frames_sent_;
      last_sent_ns = now;
    } else {
      ++frames_dropped_;
      MW_LOG(Warning) << "video link lost: " << sent.ToString();
      next_attempt_ns =
          MonotonicNs() +
          static_cast<uint64_t>(config_.reconnect_ms) * 1000000ull;
    }
  }
}

}  // namespace mw
