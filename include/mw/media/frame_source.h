// Copyright 2026 Genio Middleware Authors.
//
// Abstract camera. Implementations: V4L2 (UVC cameras) and a synthetic test
// pattern for development machines and CI.
//
// Life cycle: Open() -> Start() -> { Capture() / Release() }* -> Stop().
// Open() may be called again after a failure or after Stop() to recover from
// a USB unplug.

#ifndef MW_MEDIA_FRAME_SOURCE_H_
#define MW_MEDIA_FRAME_SOURCE_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>

#include "mw/common/status.h"
#include "mw/media/media_config.h"

namespace mw {

// One captured buffer. |data| points into driver-owned memory and stays valid
// until Release() is called for this frame.
struct CapturedFrame {
  CapturedFrame()
      : data(nullptr),
        bytes_used(0),
        fourcc(0),
        width(0),
        height(0),
        stride(0),
        sequence(0),
        timestamp_ns(0),
        buffer_index(-1) {}
  const uint8_t* data;
  size_t bytes_used;  // Valid bytes (the JPEG size for MJPEG).
  uint32_t fourcc;
  uint32_t width;
  uint32_t height;
  uint32_t stride;        // Bytes per row of the first plane; 0 if compressed.
  uint32_t sequence;      // Driver frame counter.
  uint64_t timestamp_ns;  // CLOCK_MONOTONIC.
  int buffer_index;       // Opaque to everyone but the source.
};

class FrameSource {
 public:
  virtual ~FrameSource() {}

  // Opens the device and negotiates the format. Does not stream yet.
  virtual Status Open() = 0;
  virtual Status Start() = 0;
  virtual void Stop() = 0;

  // Waits up to |timeout_ms| for the next good frame.
  //   kOk           frame returned (give it back with Release()).
  //   kTimeout      no frame yet.
  //   kUnavailable  the device went away; Stop() and Open() again.
  virtual Status Capture(int timeout_ms, CapturedFrame* frame) = 0;
  virtual void Release(const CapturedFrame& frame) = 0;

  virtual std::string Describe() const = 0;
};

Status CreateFrameSource(const VideoStreamConfig& config,
                         std::unique_ptr<FrameSource>* out);

}  // namespace mw

#endif  // MW_MEDIA_FRAME_SOURCE_H_
