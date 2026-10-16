// Copyright 2026 Genio Middleware Authors.
//
// NV12 to JPEG encoding for the optional video link to the Jetson. Frames
// cannot stay zero copy across the device boundary, so they are compressed.
//
//   "libjpeg"    software encoder (built only when libjpeg is available).
//                720p15 costs a fraction of one A78 core; fine for bring-up.
//   "v4l2_m2m"   SoC hardware JPEG/H.264 encoder fed straight from the pool
//                dma-buf. NOT implemented in this drop, see docs/PORTING.md.

#ifndef MW_ROBOTIC_JPEG_ENCODER_H_
#define MW_ROBOTIC_JPEG_ENCODER_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "mw/common/frame_types.h"
#include "mw/common/status.h"

namespace mw {

class JpegEncoder {
 public:
  virtual ~JpegEncoder() {}

  // Compresses the NV12 image at |nv12| (laid out as |format|) into |out|.
  virtual Status EncodeNv12(const uint8_t* nv12, const FrameFormat& format,
                            int quality, std::vector<uint8_t>* out) = 0;

  virtual std::string Describe() const = 0;
};

Status CreateJpegEncoder(const std::string& name,
                         std::unique_ptr<JpegEncoder>* out);

}  // namespace mw

#endif  // MW_ROBOTIC_JPEG_ENCODER_H_
