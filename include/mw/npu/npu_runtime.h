// Copyright 2026 Genio Middleware Authors.
//
// Hardware abstraction of the NPU. Everything above this interface is
// portable; everything below is a backend:
//
//   "neuron"  MediaTek NeuroPilot (Neuron Adapter), compiled models (.dla).
//             Built only when the SDK is available (MW_HAVE_NEURON).
//   "fake"    Deterministic synthetic outputs, no hardware. For CI and for
//             developing applications on a PC.
//
// All models run on the NPU; there is deliberately no CPU inference backend.

#ifndef MW_NPU_NPU_RUNTIME_H_
#define MW_NPU_NPU_RUNTIME_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "mw/common/status.h"
#include "mw/npu/npu_config.h"
#include "mw/npu/tensor_buffer.h"

namespace mw {

enum TensorType { kTensorUint8, kTensorFloat32 };

struct InputSpec {
  InputSpec() : width(0), height(0), channels(0), type(kTensorUint8) {}
  uint32_t width;     // Image width, or samples per window for audio.
  uint32_t height;    // Image height, 1 for audio.
  uint32_t channels;  // 3 for images, 1 for audio.
  TensorType type;
  size_t bytes() const {
    return static_cast<size_t>(width) * height * channels *
           (type == kTensorFloat32 ? 4 : 1);
  }
};

// One output tensor, already de-quantised to float.
struct OutputTensor {
  std::vector<int> dims;
  std::vector<float> data;
};

class NpuModel {
 public:
  virtual ~NpuModel() {}

  virtual const InputSpec& input_spec() const = 0;

  // Runs one inference. |input| holds input_spec().bytes() bytes; backends
  // that support it import input.fd() instead of copying.
  virtual Status Run(const TensorBuffer& input,
                     std::vector<OutputTensor>* outputs) = 0;
};

class NpuRuntime {
 public:
  virtual ~NpuRuntime() {}

  virtual Status Load(const ModelConfig& config,
                      std::unique_ptr<NpuModel>* model) = 0;
  virtual std::string Describe() const = 0;
};

// Creates the backend named by config.runtime: "cpu" (ONNX Runtime loaded at
// run time), "neuron" (NPU, not implemented yet) or "fake" (test double).
Status CreateNpuRuntime(const NpuConfig& config,
                        std::unique_ptr<NpuRuntime>* out);

// Derives the input tensor layout a model of |config| expects.
InputSpec InputSpecFromConfig(const ModelConfig& config);

}  // namespace mw

#endif  // MW_NPU_NPU_RUNTIME_H_
