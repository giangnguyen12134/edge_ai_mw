// Copyright 2026 Genio Middleware Authors.
//
// MJPEG to NV12 decoding. This is the platform integration point for the
// SoC's hardware JPEG decoder:
//
//   "libjpeg"    software fallback (built only when libjpeg is available).
//   "v4l2_m2m"   hardware decoder (MediaTek mtk-jpeg) writing straight into
//                the pool dma-buf. NOT implemented in this drop: it needs the
//                exact BSP node and plane layout, see docs/PORTING.md.

#ifndef MW_MEDIA_JPEG_DECODER_H_
#define MW_MEDIA_JPEG_DECODER_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>

#include "mw/common/frame_types.h"
#include "mw/common/status.h"

namespace mw {

class JpegDecoder {
 public:
  virtual ~JpegDecoder() {}

  // Decodes one baseline JPEG into |dst| laid out as |format| (NV12). The JPEG
  // dimensions must equal format.width x format.height.
  virtual Status DecodeToNv12(const uint8_t* jpeg, size_t size, uint8_t* dst,
                              const FrameFormat& format) = 0;

  virtual std::string Describe() const = 0;
};

Status CreateJpegDecoder(const std::string& name,
                         std::unique_ptr<JpegDecoder>* out);

}  // namespace mw

#endif  // MW_MEDIA_JPEG_DECODER_H_
