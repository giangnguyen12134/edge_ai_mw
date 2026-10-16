// Copyright 2026 Genio Middleware Authors.
//
// The single ingestion step: turns a captured camera frame into NV12 inside a
// pool slot. Implementations are selected from the camera pixel format
// (configuration). Hardware-accelerated variants (V4L2 mem2mem JPEG decoder or
// scaler/colour converter writing directly into the dma-buf) implement the
// same interface and replace the CPU versions without any other change.

#ifndef MW_MEDIA_FRAME_INGEST_H_
#define MW_MEDIA_FRAME_INGEST_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "mw/common/frame_types.h"
#include "mw/common/status.h"
#include "mw/media/frame_source.h"

namespace mw {

class FrameIngest {
 public:
  virtual ~FrameIngest() {}

  // Writes |src| as NV12 into |dst| using the layout of |dst_format|.
  virtual Status Convert(const CapturedFrame& src,
                         const FrameFormat& dst_format, uint8_t* dst) = 0;

  virtual std::string Describe() const = 0;
};

// Chooses an implementation for |source_fourcc|. |mode| is "cpu" or "auto"
// (currently equivalent; reserved for hardware backends).
Status CreateFrameIngest(uint32_t source_fourcc, const std::string& mode,
                         std::unique_ptr<FrameIngest>* out);

}  // namespace mw

#endif  // MW_MEDIA_FRAME_INGEST_H_
