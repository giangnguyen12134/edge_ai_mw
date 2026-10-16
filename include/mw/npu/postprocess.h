// Copyright 2026 Genio Middleware Authors.
//
// Turns raw model outputs into ResultItems. See npu_config.h for the output
// layouts the decoders expect.

#ifndef MW_NPU_POSTPROCESS_H_
#define MW_NPU_POSTPROCESS_H_

#include <memory>
#include <vector>

#include "mw/common/inference_result.h"
#include "mw/common/status.h"
#include "mw/npu/image_scaler.h"
#include "mw/npu/npu_config.h"
#include "mw/npu/npu_runtime.h"

namespace mw {

class Postprocessor {
 public:
  virtual ~Postprocessor() {}

  // |geometry| maps model-input coordinates back to the source frame.
  virtual Status Process(const std::vector<OutputTensor>& outputs,
                         const LetterboxInfo& geometry,
                         std::vector<ResultItem>* items) = 0;
};

// Creates the decoder for "detection" and "classification" models.
Status CreatePostprocessor(const ModelConfig& config,
                           std::unique_ptr<Postprocessor>* out);

// L2-normalises |v| in place. Returns false for a zero vector.
bool L2Normalize(std::vector<float>* v);

// Cosine similarity of two equally sized vectors (0 if sizes differ).
float CosineSimilarity(const std::vector<float>& a,
                       const std::vector<float>& b);

}  // namespace mw

#endif  // MW_NPU_POSTPROCESS_H_
