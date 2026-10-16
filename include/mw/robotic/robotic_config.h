// Copyright 2026 Genio Middleware Authors.
//
// RoboticBridge configuration (JSON). The bridge is optional: it relays NPU
// results and/or camera video to the Jetson Orin Nano over the USB 3.0 network
// link (for example the Jetson's USB device-mode ethernet gadget).
//
//   {
//     "bus": "system",
//     "jetson": { "host": "192.168.55.1", "results_port": 9100,
//                 "video_port": 9101 },
//     "send_results": true,
//     "send_video": false,
//     "video": { "fps": 15, "jpeg_quality": 80, "encoder": "libjpeg" }
//   }

#ifndef MW_ROBOTIC_ROBOTIC_CONFIG_H_
#define MW_ROBOTIC_ROBOTIC_CONFIG_H_

#include <stdint.h>

#include <string>

#include "mw/common/status.h"

namespace mw {

struct RoboticConfig {
  RoboticConfig();

  std::string bus;  // "system" or "user".
  std::string log_level;
  std::string media_bus_name;
  std::string npu_bus_name;

  std::string jetson_host;  // IP address (preferred) or host name.
  uint32_t results_port;    // Newline-delimited JSON.
  uint32_t video_port;      // Length-prefixed JPEG frames.
  uint32_t connect_timeout_ms;
  uint32_t send_timeout_ms;
  uint32_t reconnect_ms;  // Pause between connection attempts.

  bool send_results;
  bool send_video;
  uint32_t video_fps;     // Upper bound of the frames sent.
  uint32_t jpeg_quality;  // 1..100.
  std::string encoder;    // "libjpeg" (software) for now.
};

Status LoadRoboticConfig(const std::string& path, RoboticConfig* config);
Status ApplyRoboticConfigOverride(const std::string& assignment,
                                  RoboticConfig* config);
Status ValidateRoboticConfig(const RoboticConfig& config);

}  // namespace mw

#endif  // MW_ROBOTIC_ROBOTIC_CONFIG_H_
