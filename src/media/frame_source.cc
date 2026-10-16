// Copyright 2026 VMX Middleware Team.

#include "mw/media/frame_source.h"

#include "mw/media/test_pattern_source.h"
#include "mw/media/v4l2_frame_source.h"

namespace mw {

Status CreateFrameSource(const VideoStreamConfig& config,
                         std::unique_ptr<FrameSource>* out) {
  if (config.source == "v4l2") {
    out->reset(new V4l2FrameSource(config));
    return Status::Ok();
  }
  if (config.source == "test_pattern") {
    out->reset(new TestPatternSource(config));
    return Status::Ok();
  }
  return InvalidArgumentError("unknown video source '" + config.source + "'");
}

}  // namespace mw
