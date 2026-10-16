// Copyright 2026 Genio Middleware Authors.

#include "mw/viewer/viewer_core.h"

#include <chrono>

#include "mw/common/logging.h"
#include "mw/common/time_util.h"

namespace mw {
namespace {

const uint64_t kAudioResultMaxAgeNs = 3ull * 1000000000ull;
const std::chrono::milliseconds kControlPeriod(1000);

}  // namespace

ViewerCore::Options::Options()
    : bus("system"),
      client_name("mw-viewer"),
      use_npu(true),
      queue_depth(2),
      result_max_age_ms(500) {}

ViewerCore::ViewerCore(const Options& options)
    : options_(options),
      results_(static_cast<uint64_t>(options.result_max_age_ms) * 1000000ull,
               kAudioResultMaxAgeNs),
      fps_window_start_ns_(0),
      fps_window_frames_(0),
      stop_(false),
      started_(false) {}

ViewerCore::~ViewerCore() { Stop(); }

Status ViewerCore::Start() {
  if (started_) {
    return FailedPreconditionError("ViewerCore already started");
  }
  MediaClient::Options media_options;
  media_options.bus = options_.bus;
  media_options.client_name = options_.client_name;
  media_options.queue_depth = options_.queue_depth;
  media_options.map_frames = true;  // We convert on the CPU for display.
  media_.reset(new MediaClient(media_options));
  MW_RETURN_IF_ERROR(media_->Start(
      [this](MediaClient::Frame frame) { OnFrame(std::move(frame)); },
      [this](bool connected, const MediaClient::VideoInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.video_connected = connected;
        if (connected) {
          stats_.stream_id = info.stream_id;
          stats_.width = info.format.width;
          stats_.height = info.format.height;
          stats_.fourcc = info.format.fourcc;
        } else {
          latest_.reset();  // Do not show a frozen picture of a dead camera.
        }
      }));

  if (options_.use_npu) {
    NpuClient::Options npu_options;
    npu_options.bus = options_.bus;
    npu_.reset(new NpuClient(npu_options));
    MW_RETURN_IF_ERROR(npu_->Start([this](const InferenceResult& result) {
      results_.Update(result, MonotonicNs());
    }));
  }
  stop_ = false;
  started_ = true;
  if (npu_ != nullptr) {
    control_ = std::thread(&ViewerCore::ControlLoop, this);
  }
  return Status::Ok();
}

void ViewerCore::Stop() {
  if (!started_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (control_.joinable()) {
    control_.join();
  }
  if (media_ != nullptr) {
    media_->Stop();
  }
  if (npu_ != nullptr) {
    npu_->Stop();
  }
  started_ = false;
}

void ViewerCore::OnFrame(MediaClient::Frame frame) {
  std::shared_ptr<BgraImage> image(new BgraImage());
  frame.BeginCpuRead();
  const Status status = ConvertFrameToBgra(frame.data(), frame.size(),
                                           frame.format(), image.get());
  frame.EndCpuRead();
  frame.Release();  // The camera may reuse the buffer from here on.

  const uint64_t now = MonotonicNs();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status.ok()) {
      if (stats_.convert_errors++ == 0) {
        MW_LOG(Warning) << "cannot display frame: " << status.ToString();
      }
      return;
    }
    latest_ = image;
    ++stats_.frames;
    if (fps_window_start_ns_ == 0) {
      fps_window_start_ns_ = now;
    }
    ++fps_window_frames_;
    if (now - fps_window_start_ns_ >= 1000000000ull) {
      stats_.fps = static_cast<double>(fps_window_frames_) * 1e9 /
                   static_cast<double>(now - fps_window_start_ns_);
      fps_window_start_ns_ = now;
      fps_window_frames_ = 0;
    }
  }
  if (frame_notify_) {
    frame_notify_();
  }
}

std::shared_ptr<const BgraImage> ViewerCore::latest_image() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

OverlaySnapshot ViewerCore::overlay() const {
  return results_.Snapshot(MonotonicNs());
}

ViewerCore::Stats ViewerCore::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Stats copy = stats_;
  copy.results = results_.updates();
  return copy;
}

std::vector<NpuClient::ModelInfo> ViewerCore::models() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return models_;
}

void ViewerCore::SetModelEnabled(const std::string& name, bool enabled) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_toggles_.push_back(std::make_pair(name, enabled));
  }
  cv_.notify_all();
}

// Bus calls can take seconds when the NpuManager is down, so they never run
// on the UI thread: this thread applies toggles and refreshes the model list.
void ViewerCore::ControlLoop() {
  while (true) {
    std::vector<std::pair<std::string, bool>> toggles;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, kControlPeriod,
                   [this] { return stop_ || !pending_toggles_.empty(); });
      if (stop_) {
        return;
      }
      toggles.swap(pending_toggles_);
    }
    for (size_t i = 0; i < toggles.size(); ++i) {
      const Status status =
          npu_->SetModelEnabled(toggles[i].first, toggles[i].second);
      if (!status.ok()) {
        MW_LOG(Warning) << "model '" << toggles[i].first
                        << "': " << status.ToString();
      }
    }
    std::vector<NpuClient::ModelInfo> models;
    const Status listed = npu_->ListModels(&models);
    if (listed.ok()) {
      std::vector<std::string> names;
      for (size_t i = 0; i < models.size(); ++i) {
        names.push_back(models[i].name);
      }
      results_.SetOrder(names);  // Colours follow the config order.
    }
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.npu_connected = listed.ok();
    models_ = listed.ok() ? models : std::vector<NpuClient::ModelInfo>();
  }
}

}  // namespace mw
