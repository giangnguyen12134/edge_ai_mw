// Copyright 2026 Genio Middleware Authors.
//
// Video frame description shared by every module. Frames in the DMA-BUF pool
// use the camera's native pixel format (NV12, YUYV or MJPEG); conversion to
// a model's input format is the job of a later preprocessing stage.

#ifndef MW_COMMON_FRAME_TYPES_H_
#define MW_COMMON_FRAME_TYPES_H_

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "mw/common/status.h"

namespace mw {

constexpr uint32_t MakeFourcc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

// Values match V4L2_PIX_FMT_*.
constexpr uint32_t kFourccNv12 = MakeFourcc('N', 'V', '1', '2');
constexpr uint32_t kFourccYuyv = MakeFourcc('Y', 'U', 'Y', 'V');
constexpr uint32_t kFourccMjpeg = MakeFourcc('M', 'J', 'P', 'G');

// Parses "NV12", "YUYV" or "MJPEG"/"MJPG" (case-insensitive).
bool ParsePixelFormat(const std::string& name, uint32_t* fourcc);
std::string FourccToString(uint32_t fourcc);

inline uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// Layout of one NV12 buffer inside the shared pool.
struct FrameFormat {
  FrameFormat()
      : width(0),
        height(0),
        fourcc(0),
        stride(0),
        uv_offset(0),
        buffer_size(0) {}

  // Builds the NV12 layout for |width| x |height|: stride aligned to 16
  // bytes, UV plane directly after the Y plane, buffer size page aligned.
  static FrameFormat Nv12(uint32_t width, uint32_t height);

  // Builds the layout the camera driver negotiated: |bytes_per_line| and
  // |size_image| come straight from VIDIOC_S_FMT. NV12 gets its UV plane
  // right after the Y plane; packed formats have uv_offset 0. The buffer
  // size is |size_image| rounded up to whole pages.
  static FrameFormat FromCamera(uint32_t width, uint32_t height,
                                uint32_t fourcc, uint32_t bytes_per_line,
                                uint32_t size_image);

  uint32_t width;
  uint32_t height;
  uint32_t fourcc;
  uint32_t stride;       // Bytes per row, both planes.
  uint32_t uv_offset;    // Byte offset of the UV plane.
  uint32_t buffer_size;  // Total bytes of the dma-buf.
};

// Checks that |format| is self-consistent and that a frame fits in
// |buffer_size|. Used by the service before it publishes a pool and by every
// client before it maps one received from the bus.
Status ValidateFrameFormat(const FrameFormat& format);

// Metadata that accompanies every frame notification.
struct FrameInfo {
  FrameInfo() : slot(0), bytes_used(0), sequence(0), timestamp_ns(0) {}
  uint32_t slot;
  uint32_t bytes_used;  // Valid bytes in the buffer (JPEG size for MJPEG).
  uint64_t sequence;
  uint64_t timestamp_ns;
};

}  // namespace mw

#endif  // MW_COMMON_FRAME_TYPES_H_
