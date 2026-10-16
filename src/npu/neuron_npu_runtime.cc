// Copyright 2026 Genio Middleware Authors.

#include "mw/npu/neuron_npu_runtime.h"

namespace mw {

Status CreateNeuronNpuRuntime(std::unique_ptr<NpuRuntime>* out) {
  out->reset();
  return UnimplementedError(
      "the 'neuron' NPU runtime backend is not implemented in this source "
      "drop; implement src/npu/neuron_npu_runtime.cc against the NeuroPilot "
      "SDK of your BSP (see docs/PORTING.md), or set runtime to 'fake' for "
      "development");
}

}  // namespace mw
