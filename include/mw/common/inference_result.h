// Copyright 2026 Genio Middleware Authors.
//
// Model output as published by NpuManager and consumed by applications.

#ifndef MW_COMMON_INFERENCE_RESULT_H_
#define MW_COMMON_INFERENCE_RESULT_H_

#include <stdint.h>

#include <string>
#include <vector>

namespace mw {

struct ResultItem {
  ResultItem() : score(0), x(0), y(0), w(0), h(0) {}
  ResultItem(const std::string& l, double s, double bx, double by, double bw,
             double bh)
      : label(l), score(s), x(bx), y(by), w(bw), h(bh) {}

  std::string label;
  double score;
  // Box normalised to the source frame (0..1). All zero for non-spatial
  // results (gesture classification, speaker identification).
  double x, y, w, h;
};

struct InferenceResult {
  InferenceResult() : sequence(0), timestamp_ns(0) {}

  std::string model;      // Configured model name, e.g. "face".
  std::string kind;       // "detection", "classification" or "speaker".
  uint64_t sequence;      // Video frame sequence (0 for audio).
  uint64_t timestamp_ns;  // Video frame capture time (0 for audio).
  std::vector<ResultItem> items;
};

}  // namespace mw

#endif  // MW_COMMON_INFERENCE_RESULT_H_
