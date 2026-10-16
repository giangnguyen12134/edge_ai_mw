// Copyright 2026 Genio Middleware Authors.
//
// Synthetic NV12 camera for development machines and CI. Produces a moving
// gradient with a bright square so frame flow and latency are easy to verify.

#ifndef MW_MEDIA_TEST_PATTERN_SOURCE_H_
#define MW_MEDIA_TEST_PATTERN_SOURCE_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "mw/common/macros.h"
#include "mw/media/frame_source.h"

namespace mw {

class TestPatternSource : public FrameSource {
 public:
  explicit TestPatternSource(const VideoStreamConfig& config);
  ~TestPatternSource() override;

  MW_DISALLOW_COPY_AND_ASSIGN(TestPatternSource);

  Status Open() override;
  Status Start() override;
  void Stop() override;
  Status Capture(int timeout_ms, CapturedFrame* frame) override;
  void Release(const CapturedFrame& frame) override;
  std::string Describe() const override;

 private:
  void Render(uint64_t frame_index);

  VideoStreamConfig config_;
  bool opened_;
  bool streaming_;
  uint32_t stride_;
  uint32_t uv_offset_;
  uint64_t frame_index_;
  uint64_t next_deadline_ns_;
  std::vector<uint8_t> buffer_;
};

}  // namespace mw

#endif  // MW_MEDIA_TEST_PATTERN_SOURCE_H_
