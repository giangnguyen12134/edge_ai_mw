// Copyright 2026 Genio Middleware Authors.
//
// Converts a camera frame in its native format (NV12, YUYV or MJPG) into a
// packed 32-bit image for on-screen display. Display code only; inference
// uses the NPU manager's own scaler.

#ifndef MW_VIEWER_FRAME_CONVERT_H_
#define MW_VIEWER_FRAME_CONVERT_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "mw/common/frame_types.h"
#include "mw/common/status.h"

namespace mw {

// Pixels in memory order B, G, R, X with 4 bytes per pixel and no row
// padding. This is exactly cairo's CAIRO_FORMAT_RGB24 on little-endian CPUs.
struct BgraImage {
  BgraImage() : width(0), height(0) {}
  uint32_t width;
  uint32_t height;
  std::vector<uint8_t> pixels;  // width * height * 4 bytes.

  uint32_t stride() const { return width * 4; }
};

// |data| holds |bytes_used| valid bytes laid out as |format| describes.
// NV12/YUYV use BT.601 limited range (what UVC cameras produce); MJPG is
// decoded with gdk-pixbuf (the result size is the JPEG's, not |format|'s).
Status ConvertFrameToBgra(const uint8_t* data, size_t bytes_used,
                          const FrameFormat& format, BgraImage* out);

}  // namespace mw

#endif  // MW_VIEWER_FRAME_CONVERT_H_
