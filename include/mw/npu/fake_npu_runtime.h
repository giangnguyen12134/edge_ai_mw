// Copyright 2026 Genio Middleware Authors.
//
// Synthetic NPU backend: produces plausible, deterministic outputs in the
// layout the post-processors expect, and reads the input so the dma-buf
// really gets touched. No hardware needed.

#ifndef MW_NPU_FAKE_NPU_RUNTIME_H_
#define MW_NPU_FAKE_NPU_RUNTIME_H_

#include "mw/npu/npu_runtime.h"

namespace mw {

class FakeNpuRuntime : public NpuRuntime {
 public:
  Status Load(const ModelConfig& config,
              std::unique_ptr<NpuModel>* model) override;
  std::string Describe() const override { return "fake (no NPU)"; }
};

}  // namespace mw

#endif  // MW_NPU_FAKE_NPU_RUNTIME_H_
