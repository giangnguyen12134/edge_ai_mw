// Copyright 2026 Genio Middleware Authors.
//
// MediaManager configuration. Loaded from a JSON file; individual values can
// be overridden on the command line with --set key=value (for example
// --set video.width=1920 --set video.pixel_format=YUYV).

#ifndef MW_MEDIA_MEDIA_CONFIG_H_
#define MW_MEDIA_MEDIA_CONFIG_H_

#include <stdint.h>

#include <string>

#include "mw/common/status.h"

namespace mw {

struct VideoStreamConfig {
  VideoStreamConfig();

  std::string id;      // Stream name seen by clients.
  std::string source;  // "v4l2" or "test_pattern".
  std::string device;  // V4L2 device node.
  uint32_t width;
  uint32_t height;
  uint32_t fps;
  uint32_t pixel_format;  // Camera output fourcc (NV12, YUYV or MJPG).
  uint32_t v4l2_buffers;  // Driver-side buffer count.
  uint32_t pool_slots;    // Shared DMA-BUF pool size (max 16).
  // "native": the camera fills the shared dma-bufs directly and frames keep
  // the camera's pixel format (zero copy). "nv12": legacy mode, every frame
  // is converted by the CPU into an NV12 pool (one copy, models can use it).
  std::string pool_format;
  // Native mode only: "auto", "expbuf" (VIDIOC_EXPBUF of driver buffers) or
  // "dmabuf" (V4L2_MEMORY_DMABUF import of dma-heap buffers).
  std::string capture_io;
  std::string allocator;      // See CreateBufferAllocator().
  uint32_t stall_timeout_ms;  // No frame for this long => reopen the device.
  // Test hook for source "test_pattern" in native mode: the synthetic camera
  // "unplugs" itself after this many frames of every session (0 = never).
  uint32_t test_unplug_after_frames;
};

struct AudioStreamConfig {
  AudioStreamConfig();

  bool enabled;
  std::string id;
  std::string source;  // "alsa" or "sine".
  std::string device;  // ALSA device, e.g. "hw:1,0".
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t ring_ms;    // Shared ring capacity.
  uint32_t period_ms;  // Capture chunk size.
};

struct MediaConfig {
  MediaConfig();

  std::string bus;  // "system" or "user".
  std::string log_level;
  VideoStreamConfig video;
  AudioStreamConfig audio;
};

// Loads |path| on top of the defaults.
Status LoadMediaConfig(const std::string& path, MediaConfig* config);

// Applies one "section.key=value" override.
Status ApplyMediaConfigOverride(const std::string& assignment,
                                MediaConfig* config);

Status ValidateMediaConfig(const MediaConfig& config);

}  // namespace mw

#endif  // MW_MEDIA_MEDIA_CONFIG_H_
