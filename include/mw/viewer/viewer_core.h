// Copyright 2026 Genio Middleware Authors.
//
// Everything the viewer does except the window: receives frames from the
// MediaManager (MediaClient), converts the newest one for display, collects
// NPU results (NpuClient) and keeps model state for the UI. The GTK front end
// and the headless --snapshot mode both sit on top of it.

#ifndef MW_VIEWER_VIEWER_CORE_H_
#define MW_VIEWER_VIEWER_CORE_H_

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mw/common/macros.h"
#include "mw/common/status.h"
#include "mw/sdk/media_client.h"
#include "mw/sdk/npu_client.h"
#include "mw/viewer/frame_convert.h"
#include "mw/viewer/overlay.h"

namespace mw {

class ViewerCore {
 public:
  struct Options {
    Options();
    std::string bus;          // "system" or "user".
    std::string client_name;  // Shown in the MediaManager log.
    bool use_npu;             // Subscribe to results and show model controls.
    uint32_t queue_depth;     // Frames the viewer may hold at once.
    uint32_t result_max_age_ms;
  };

  struct Stats {
    Stats()
        : video_connected(false),
          npu_connected(false),
          frames(0),
          convert_errors(0),
          results(0),
          fps(0),
          width(0),
          height(0),
          fourcc(0) {}
    bool video_connected;
    bool npu_connected;
    uint64_t frames;
    uint64_t convert_errors;
    uint64_t results;
    double fps;
    uint32_t width;  // Of the camera format.
    uint32_t height;
    uint32_t fourcc;  // Camera pixel format.
    std::string stream_id;
  };

  explicit ViewerCore(const Options& options);
  ~ViewerCore();

  MW_DISALLOW_COPY_AND_ASSIGN(ViewerCore);

  // Called on an SDK thread for every converted frame; must be quick and
  // thread safe (the GTK front end posts an idle callback). Set before Start.
  void SetFrameNotify(const std::function<void()>& notify) {
    frame_notify_ = notify;
  }

  // Returns once the bus connections are up; the services need not run yet.
  Status Start();
  void Stop();

  // Newest converted frame (null before the first one). Immutable; keep it
  // as long as you like.
  std::shared_ptr<const BgraImage> latest_image() const;

  OverlaySnapshot overlay() const;
  Stats stats() const;
  std::vector<NpuClient::ModelInfo> models() const;

  // Asks the NPU manager to switch a model; runs on the control thread.
  void SetModelEnabled(const std::string& name, bool enabled);

 private:
  void OnFrame(MediaClient::Frame frame);
  void ControlLoop();

  Options options_;
  std::function<void()> frame_notify_;
  std::unique_ptr<MediaClient> media_;
  std::unique_ptr<NpuClient> npu_;
  ResultStore results_;

  mutable std::mutex mutex_;
  std::shared_ptr<const BgraImage> latest_;
  Stats stats_;
  uint64_t fps_window_start_ns_;
  uint64_t fps_window_frames_;
  std::vector<NpuClient::ModelInfo> models_;
  std::vector<std::pair<std::string, bool>> pending_toggles_;

  std::thread control_;
  std::condition_variable cv_;
  bool stop_;
  bool started_;
};

}  // namespace mw

#endif  // MW_VIEWER_VIEWER_CORE_H_
