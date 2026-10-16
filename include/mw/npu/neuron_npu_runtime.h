// Copyright 2026 Genio Middleware Authors.
//
// MediaTek NeuroPilot backend (INTEGRATION POINT, NOT IMPLEMENTED YET).
//
// The factory fails fast with kUnimplemented so a production configuration
// that selects runtime "neuron" refuses to start instead of silently running
// on something else. To implement it against the NeuroPilot SDK that matches
// your BSP (the headers and libraries are version specific and are not part of
// this repository):
//
//   1. Load:  read the compiled .dla (ModelConfig::path) and restore it into a
//             compilation/execution handle with the Neuron Adapter (or the
//             Neuron Runtime API for direct DLA loading).
//   2. Query: read the input/output tensor shapes and quantisation parameters
//             and check them against InputSpecFromConfig().
//   3. Input: create an SDK memory object from TensorBuffer::fd() (dma-buf)
//             once per model and bind it as the input, so Run() copies
//             nothing. If your SDK version cannot import a dma-buf, copy from
//             TensorBuffer::data() into the SDK's own input buffer instead
//             (one small copy of the already resized tensor).
//   4. Run:   execute synchronously on the NPU, de-quantise each output to
//             float and return it in OutputTensor order.
//
// See docs/PORTING.md.

#ifndef MW_NPU_NEURON_NPU_RUNTIME_H_
#define MW_NPU_NEURON_NPU_RUNTIME_H_

#include <memory>

#include "mw/common/status.h"
#include "mw/npu/npu_runtime.h"

namespace mw {

Status CreateNeuronNpuRuntime(std::unique_ptr<NpuRuntime>* out);

}  // namespace mw

#endif  // MW_NPU_NEURON_NPU_RUNTIME_H_
