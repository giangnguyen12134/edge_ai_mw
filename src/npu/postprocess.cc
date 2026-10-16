// Copyright 2026 Genio Middleware Authors.

#include "mw/npu/postprocess.h"

#include <math.h>

#include <algorithm>

#include "mw/common/logging.h"

namespace mw {
namespace {

const size_t kMaxDetections = 64;

std::string LabelFor(const ModelConfig& config, int id) {
  if (id >= 0 && static_cast<size_t>(id) < config.labels.size()) {
    return config.labels[static_cast<size_t>(id)];
  }
  return "class_" + std::to_string(id);
}

constexpr float kNmsIouThreshold = 0.45f;

struct Candidate {
  float x0;
  float y0;
  float x1;
  float y1;
  float score;
  int cls;
};

float Iou(const Candidate& a, const Candidate& b) {
  const float ix0 = std::max(a.x0, b.x0);
  const float iy0 = std::max(a.y0, b.y0);
  const float ix1 = std::min(a.x1, b.x1);
  const float iy1 = std::min(a.y1, b.y1);
  const float iw = std::max(0.0f, ix1 - ix0);
  const float ih = std::max(0.0f, iy1 - iy0);
  const float inter = iw * ih;
  const float area_a = (a.x1 - a.x0) * (a.y1 - a.y0);
  const float area_b = (b.x1 - b.x0) * (b.y1 - b.y0);
  const float uni = area_a + area_b - inter;
  return uni > 0.0f ? inter / uni : 0.0f;
}

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

bool ByScoreDescending(const ResultItem& a, const ResultItem& b) {
  return a.score > b.score;
}

class DetectionPostprocessor : public Postprocessor {
 public:
  explicit DetectionPostprocessor(const ModelConfig& config)
      : config_(config) {}

  Status Process(const std::vector<OutputTensor>& outputs,
                 const LetterboxInfo& g,
                 std::vector<ResultItem>* items) override {
    items->clear();

    const int in_w = static_cast<int>(g.input_w);
    const int in_h = static_cast<int>(g.input_h);
    size_t anchors = 0;
    for (int stride : {8, 16, 32}) {
      anchors += static_cast<size_t>(in_w / stride) * (in_h / stride);
    }

    const OutputTensor* tensor = nullptr;
    size_t channels = 0;
    for (size_t i = 0; i < outputs.size(); ++i) {
      const size_t n = outputs[i].data.size();
      if (anchors > 0 && n % anchors == 0 && n / anchors > 4) {
        tensor = &outputs[i];
        channels = n / anchors;
        break;
      }
    }
    if (tensor == nullptr) {
      return InvalidArgumentError(
          "detection model produced no [4 + classes, anchors] output");
    }
    const size_t num_classes = channels - 4;
    const float* d = tensor->data.data();

    std::vector<Candidate> cands;
    for (size_t i = 0; i < anchors; ++i) {
      int best_cls = 0;
      float best = d[4 * anchors + i];
      for (size_t k = 1; k < num_classes; ++k) {
        const float s = d[(4 + k) * anchors + i];
        if (s > best) {
          best = s;
          best_cls = static_cast<int>(k);
        }
      }
      if (!(best >= config_.score_threshold)) {
        continue;
      }
      const float cx = d[0 * anchors + i];
      const float cy = d[1 * anchors + i];
      const float w = d[2 * anchors + i];
      const float h = d[3 * anchors + i];
      cands.push_back(Candidate{cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2,
                                best, best_cls});
    }

    std::sort(cands.begin(), cands.end(),
              [](const Candidate& a, const Candidate& b) {
                return a.score > b.score;
              });
    std::vector<Candidate> kept;
    for (const Candidate& c : cands) {
      bool suppressed = false;
      for (const Candidate& k : kept) {
        if (k.cls == c.cls && Iou(k, c) > kNmsIouThreshold) {
          suppressed = true;
          break;
        }
      }
      if (!suppressed) {
        kept.push_back(c);
      }
    }

    for (const Candidate& c : kept) {
      const float nx0 = Clamp01((c.x0 - g.pad_x) / g.content_w);
      const float ny0 = Clamp01((c.y0 - g.pad_y) / g.content_h);
      const float nx1 = Clamp01((c.x1 - g.pad_x) / g.content_w);
      const float ny1 = Clamp01((c.y1 - g.pad_y) / g.content_h);
      if (nx1 <= nx0 || ny1 <= ny0) {
        continue;
      }
      items->push_back(ResultItem(LabelFor(config_, c.cls), c.score, nx0, ny0,
                                  nx1 - nx0, ny1 - ny0));
    }
    std::sort(items->begin(), items->end(), ByScoreDescending);
    if (items->size() > kMaxDetections) {
      items->resize(kMaxDetections);
    }
    return Status::Ok();
  }

 private:
  ModelConfig config_;
};

class ClassificationPostprocessor : public Postprocessor {
 public:
  explicit ClassificationPostprocessor(const ModelConfig& config)
      : config_(config) {}

  Status Process(const std::vector<OutputTensor>& outputs,
                 const LetterboxInfo& /*geometry*/,
                 std::vector<ResultItem>* items) override {
    items->clear();
    if (outputs.empty() || outputs[0].data.empty()) {
      return InvalidArgumentError("classification model produced no output");
    }
    std::vector<float> scores = outputs[0].data;
    if (config_.apply_softmax) {
      const float peak = *std::max_element(scores.begin(), scores.end());
      float sum = 0;
      for (size_t i = 0; i < scores.size(); ++i) {
        scores[i] = expf(scores[i] - peak);
        sum += scores[i];
      }
      for (size_t i = 0; i < scores.size(); ++i) {
        scores[i] /= sum;
      }
    }
    const size_t best = static_cast<size_t>(
        std::max_element(scores.begin(), scores.end()) - scores.begin());
    if (scores[best] >= config_.score_threshold) {
      items->push_back(ResultItem(LabelFor(config_, static_cast<int>(best)),
                                  scores[best], 0, 0, 0, 0));
    }
    return Status::Ok();
  }

 private:
  ModelConfig config_;
};
}  // namespace

Status CreatePostprocessor(const ModelConfig& config,
                           std::unique_ptr<Postprocessor>* out) {
  if (config.kind == "detection") {
    out->reset(new DetectionPostprocessor(config));
    return Status::Ok();
  }
  if (config.kind == "classification") {
    out->reset(new ClassificationPostprocessor(config));
    return Status::Ok();
  }
  return InvalidArgumentError("no image post-processor for kind '" +
                              config.kind + "'");
}

bool L2Normalize(std::vector<float>* v) {
  double sum = 0;
  for (size_t i = 0; i < v->size(); ++i) {
    sum += static_cast<double>((*v)[i]) * (*v)[i];
  }
  if (sum <= 1e-12) {
    return false;
  }
  const float inv = static_cast<float>(1.0 / sqrt(sum));
  for (size_t i = 0; i < v->size(); ++i) {
    (*v)[i] *= inv;
  }
  return true;
}

float CosineSimilarity(const std::vector<float>& a,
                       const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) {
    return 0.0f;
  }
  double dot = 0, na = 0, nb = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * b[i];
    na += static_cast<double>(a[i]) * a[i];
    nb += static_cast<double>(b[i]) * b[i];
  }
  if (na <= 1e-12 || nb <= 1e-12) {
    return 0.0f;
  }
  return static_cast<float>(dot / sqrt(na * nb));
}

}  // namespace mw
