// Copyright 2026 Genio Middleware Authors.
//
// RoboticBridge (optional service): relays what the Genio sees and hears to
// the Jetson Orin Nano that drives the JetArm.
//
//   results  NpuClient -> JSON lines -> TCP results_port   (small, preferred)
//   video    MediaClient -> JPEG     -> TCP video_port     (optional)
//
// Video frame record (all integers little endian):
//   offset size  field
//        0    4  magic "MWV1"
//        4    4  payload size N
//        8    8  frame sequence
//       16    8  capture timestamp, CLOCK_MONOTONIC ns of the Genio
//       24    2  width
//       26    2  height
//       28    N  JPEG image
//
// Neither path can ever stall the camera or the NPU: results use a bounded
// drop-oldest queue and video keeps only the newest frame. Both reconnect
// forever, so the Jetson may boot after (or reboot independently of) the
// Genio.
//
// SECURITY: the link is plain TCP without authentication, meant for a
// dedicated point-to-point USB network. Results can contain speaker names.

#ifndef MW_ROBOTIC_ROBOTIC_BRIDGE_H_
#define MW_ROBOTIC_ROBOTIC_BRIDGE_H_

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "mw/common/inference_result.h"
#include "mw/common/macros.h"
#include "mw/common/status.h"
#include "mw/robotic/jpeg_encoder.h"
#include "mw/robotic/robotic_config.h"
#include "mw/robotic/tcp_sender.h"
#include "mw/sdk/media_client.h"
#include "mw/sdk/npu_client.h"

namespace mw {

class RoboticBridge {
 public:
  explicit RoboticBridge(const RoboticConfig& config);
  ~RoboticBridge();

  MW_DISALLOW_COPY_AND_ASSIGN(RoboticBridge);

  Status Start();
  void Stop();

  bool Healthy() const;

 private:
  void OnResult(const InferenceResult& result);
  void OnFrame(MediaClient::Frame frame);
  void ResultLoop();
  void VideoLoop();
  // Connects |sender| unless a recent attempt failed. Returns true if usable.
  bool EnsureConnected(TcpSender* sender, uint32_t port,
                       uint64_t* next_attempt_ns, const char* what);

  RoboticConfig config_;
  NpuClient npu_;
  MediaClient media_;
  std::unique_ptr<JpegEncoder> encoder_;

  std::thread result_thread_;
  std::thread video_thread_;
  std::atomic<bool> stop_;
  std::atomic<bool> started_;
  std::atomic<uint64_t> result_heartbeat_ns_;
  std::atomic<uint64_t> video_heartbeat_ns_;

  std::mutex result_mutex_;
  std::condition_variable result_cv_;
  std::deque<std::string> result_lines_;

  std::mutex frame_mutex_;
  std::condition_variable frame_cv_;
  std::shared_ptr<MediaClient::Frame> latest_frame_;

  std::atomic<uint64_t> results_sent_;
  std::atomic<uint64_t> results_dropped_;
  std::atomic<uint64_t> frames_sent_;
  std::atomic<uint64_t> frames_dropped_;
};

}  // namespace mw

#endif  // MW_ROBOTIC_ROBOTIC_BRIDGE_H_
