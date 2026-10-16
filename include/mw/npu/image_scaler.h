// Copyright 2026 Genio Middleware Authors.
//
// Pre-processing: NV12, YUVY camera frame -> resized, colour-converted model
// input tensor. This is the second hardware integration point (after JPEG
// decode): production should implement ImageScaler on the SoC's
// scaler/colour-converter (V4L2 mem2mem or 2D engine) taking
// SourceImage::dma_buf_fd and writing TensorBuffer::fd(), so no pixel is
// touched by the CPU. The CPU implementation below is the portable reference
// and fallback.

#ifndef MW_NPU_IMAGE_SCALER_H_
#define MW_NPU_IMAGE_SCALER_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "mw/common/frame_types.h"
#include "mw/common/status.h"
#include "mw/npu/npu_config.h"
#include "mw/npu/npu_runtime.h"
#include "mw/npu/tensor_buffer.h"

namespace mw {

// A frame as seen by the scaler. |data| is a CPU view (may be null for
// hardware implementations that use only the descriptor).
struct SourceImage {
  SourceImage() : data(nullptr), dma_buf_fd(-1) {}
  const uint8_t* data;
  int dma_buf_fd;
  FrameFormat format;
};

// Where the source image sits inside the model input. For "stretch" the
// content fills the input and the padding is zero.
struct LetterboxInfo {
  LetterboxInfo()
      : pad_x(0),
        pad_y(0),
        content_w(1),
        content_h(1),
        input_w(1),
        input_h(1) {}
  float pad_x, pad_y;
  float content_w, content_h;
  float input_w, input_h;
};

class ImageScaler {
 public:
  virtual ~ImageScaler() {}

  // Fills |dst| (spec.bytes() bytes) and reports the geometry used.
  virtual Status Convert(const SourceImage& src, const ModelConfig& config,
                         const InputSpec& spec, TensorBuffer* dst,
                         LetterboxInfo* geometry) = 0;

  virtual std::string Describe() const = 0;
};

// |name| is "cpu" (the only implementation in this drop).
Status CreateImageScaler(const std::string& name,
                         std::unique_ptr<ImageScaler>* out);

}  // namespace mw

#endif  // MW_NPU_IMAGE_SCALER_H_
