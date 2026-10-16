// Copyright 2026 Genio Middleware Authors.

#include "mw/npu/npu_runtime.h"

#include "mw/npu/cpu_runtime.h"
#include "mw/npu/fake_npu_runtime.h"
#include "mw/npu/neuron_npu_runtime.h"

namespace mw {

InputSpec InputSpecFromConfig(const ModelConfig& config) {
  InputSpec spec;
  if (config.is_video()) {
    spec.width = config.input_width;
    spec.height = config.input_height;
    spec.channels = 3;
    spec.type = config.input_type == "float32" ? kTensorFloat32 : kTensorUint8;
  } else {
    spec.width = static_cast<uint32_t>(
        static_cast<uint64_t>(config.sample_rate) * config.window_ms / 1000);
    spec.height = 1;
    spec.channels = 1;
    spec.type = kTensorFloat32;
  }
  return spec;
}

Status CreateNpuRuntime(const NpuConfig& config,
                        std::unique_ptr<NpuRuntime>* out) {
  const std::string& name = config.runtime;
  if (name == "cpu") {
    return CreateCpuRuntime(config, out);
  }
  if (name == "fake") {
    out->reset(new FakeNpuRuntime());
    return Status::Ok();
  }
  if (name == "neuron") {
    return CreateNeuronNpuRuntime(out);
  }
  return InvalidArgumentError("unknown NPU runtime '" + name + "'");
}

}  // namespace mw
