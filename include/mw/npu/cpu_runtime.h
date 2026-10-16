// Copyright 2026 Genio Middleware Authors.
//
// CPU inference backend: runs ONNX models on the Cortex-A cores through a
// shared ONNX Runtime library that is loaded at run time with dlopen(). There
// is no build-time dependency on ONNX Runtime (only the vendored MIT-licensed
// C header in third_party/onnxruntime), so the same binary runs with whatever
// libonnxruntime.so (>= 1.14) the target provides.
//
// Model contract (see npu_config.h for the output formats):
//   * exactly one input tensor, float32 or uint8 (uint8 sources are cast to
//     float when the graph expects float32 — normalisation is then up to the
//     graph; use input_type "float32" with input_mean/input_scale to have the
//     image scaler normalise instead);
//   * image models: NHWC or NCHW with 3 channels (input_layout "auto"
//     detects it from the shape), static or dynamic height and width;
//   * speaker models: float32 PCM, rank 2 [1, N] or rank 3 [1, 1, N];
//   * outputs: float32, float16, float64, int32 or int64; they are converted
//     to float for the post-processors.
//
// This is a CPU backend. It does NOT use the NPU.

#ifndef MW_NPU_CPU_RUNTIME_H_
#define MW_NPU_CPU_RUNTIME_H_

#include <memory>

#include "mw/common/status.h"
#include "mw/npu/npu_config.h"
#include "mw/npu/npu_runtime.h"

namespace mw {

// Loads |config.onnx_library| and prepares an environment. Fails with a clear
// message when the library or its C API is missing or too old.
Status CreateCpuRuntime(const NpuConfig& config,
                        std::unique_ptr<NpuRuntime>* out);

}  // namespace mw

#endif  // MW_NPU_CPU_RUNTIME_H_
