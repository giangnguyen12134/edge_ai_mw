// Copyright 2026 Genio Middleware Authors.

#include "mw/npu/fake_npu_runtime.h"

#include <math.h>
#include <string.h>

#include <algorithm>

namespace mw {
namespace {

const int kEmbeddingSize = 16;

class FakeModel : public NpuModel {
 public:
  explicit FakeModel(const ModelConfig& config)
      : config_(config), spec_(InputSpecFromConfig(config)), counter_(0) {}

  const InputSpec& input_spec() const override { return spec_; }

  Status Run(const TensorBuffer& input,
             std::vector<OutputTensor>* outputs) override {
    if (input.size() < spec_.bytes()) {
      return InvalidArgumentError("input tensor too small");
    }
    // Touch the input so a broken buffer shows up as a crash in tests.
    uint32_t checksum = 0;
    const uint8_t* p = input.data();
    for (size_t i = 0; i < spec_.bytes(); i += 509) {
      checksum += p[i];
    }
    ++counter_;
    outputs->clear();
    outputs->push_back(OutputTensor());
    OutputTensor& out = outputs->back();
    if (config_.kind == "detection") {
      MakeDetection(&out);
    } else if (config_.kind == "classification") {
      MakeClassification(&out);
    } else {
      MakeEmbedding(checksum, &out);
    }
    return Status::Ok();
  }

 private:
  void MakeDetection(OutputTensor* out) {
    // One box sweeping left to right, in model-input pixels or 0..1.
    const float phase = static_cast<float>(counter_ % 40) / 40.0f;
    const float x0 = 0.1f + 0.5f * phase;
    const float y0 = 0.25f, x1 = x0 + 0.25f, y1 = 0.75f;
    const float sx =
        config_.coords_normalized ? 1.0f : static_cast<float>(spec_.width);
    const float sy =
        config_.coords_normalized ? 1.0f : static_cast<float>(spec_.height);
    const float cls = static_cast<float>(
        config_.labels.empty() ? 0 : counter_ % config_.labels.size());
    float row[6];
    if (config_.box_format == "xywh") {
      row[0] = (x0 + x1) / 2 * sx;
      row[1] = (y0 + y1) / 2 * sy;
      row[2] = (x1 - x0) * sx;
      row[3] = (y1 - y0) * sy;
    } else {
      row[0] = x0 * sx;
      row[1] = y0 * sy;
      row[2] = x1 * sx;
      row[3] = y1 * sy;
    }
    row[4] = 0.9f;
    row[5] = cls;
    out->dims.assign(2, 0);
    out->dims[0] = 2;
    out->dims[1] = 6;
    out->data.assign(row, row + 6);
    // Second row below any sensible threshold, to exercise filtering.
    row[4] = 0.05f;
    out->data.insert(out->data.end(), row, row + 6);
  }

  void MakeClassification(OutputTensor* out) {
    const size_t classes = std::max<size_t>(config_.labels.size(), 3);
    out->dims.assign(1, static_cast<int>(classes));
    out->data.assign(classes, 0.02f);
    out->data[counter_ % classes] = 0.9f;
  }

  void MakeEmbedding(uint32_t checksum, OutputTensor* out) {
    out->dims.assign(1, kEmbeddingSize);
    out->data.resize(kEmbeddingSize);
    // Stable per-"voice": depends only on coarse input energy.
    const uint32_t voice = checksum % 4;
    for (int i = 0; i < kEmbeddingSize; ++i) {
      out->data[static_cast<size_t>(i)] =
          sinf(static_cast<float>((voice + 1) * (i + 1)));
    }
  }

  ModelConfig config_;
  InputSpec spec_;
  uint64_t counter_;
};

}  // namespace

Status FakeNpuRuntime::Load(const ModelConfig& config,
                            std::unique_ptr<NpuModel>* model) {
  model->reset(new FakeModel(config));
  return Status::Ok();
}

}  // namespace mw
