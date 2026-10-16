// Copyright 2026 VMX Middleware Team.

#include "mw/media/test_pattern_source.h"

#include <string.h>
#include <time.h>

#include <algorithm>

#include "mw/common/frame_types.h"
#include "mw/common/time_util.h"

namespace mw {
namespace {

void SleepNs(uint64_t ns) {
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ull);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ull);
  nanosleep(&ts, nullptr);
}

}  // namespace

TestPatternSource::TestPatternSource(const VideoStreamConfig& config)
    : config_(config),
      opened_(false),
      streaming_(false),
      stride_(0),
      uv_offset_(0),
      frame_index_(0),
      next_deadline_ns_(0) {}

TestPatternSource::~TestPatternSource() {}

Status TestPatternSource::Open() {
  const FrameFormat format = FrameFormat::Nv12(config_.width, config_.height);
  stride_ = format.stride;
  uv_offset_ = format.uv_offset;
  buffer_.assign(format.buffer_size, 0);
  opened_ = true;
  streaming_ = false;
  return Status::Ok();
}

Status TestPatternSource::Start() {
  if (!opened_) {
    return FailedPreconditionError("test pattern source is not open");
  }
  frame_index_ = 0;
  next_deadline_ns_ = MonotonicNs();
  streaming_ = true;
  return Status::Ok();
}

void TestPatternSource::Stop() { streaming_ = false; }

std::string TestPatternSource::Describe() const {
  return "test_pattern " + std::to_string(config_.width) + "x" +
         std::to_string(config_.height) + "@" + std::to_string(config_.fps);
}

void TestPatternSource::Render(uint64_t frame_index) {
  const uint32_t w = config_.width;
  const uint32_t h = config_.height;
  uint8_t* y_plane = buffer_.data();
  uint8_t* uv_plane = buffer_.data() + uv_offset_;
  const uint32_t square = std::max<uint32_t>(16, h / 8);
  const uint32_t travel = w > square ? w - square : 1;
  const uint32_t sx = static_cast<uint32_t>((frame_index * 4) % travel);
  const uint32_t sy = h / 2 - square / 2;
  for (uint32_t row = 0; row < h; ++row) {
    uint8_t* line = y_plane + static_cast<size_t>(row) * stride_;
    for (uint32_t col = 0; col < w; ++col) {
      line[col] = static_cast<uint8_t>((col * 255) / w);
    }
  }
  for (uint32_t row = sy; row < sy + square && row < h; ++row) {
    memset(y_plane + static_cast<size_t>(row) * stride_ + sx, 235, square);
  }
  for (uint32_t row = 0; row < (h + 1) / 2; ++row) {
    memset(uv_plane + static_cast<size_t>(row) * stride_, 128, w);
  }
}

Status TestPatternSource::Capture(int timeout_ms, CapturedFrame* frame) {
  *frame = CapturedFrame();
  if (!streaming_) {
    return FailedPreconditionError("test pattern source is not streaming");
  }
  const uint64_t period_ns = 1000000000ull / config_.fps;
  const uint64_t now = MonotonicNs();
  if (now < next_deadline_ns_) {
    const uint64_t wait_ns = next_deadline_ns_ - now;
    const uint64_t limit_ns = static_cast<uint64_t>(timeout_ms) * 1000000ull;
    if (wait_ns > limit_ns) {
      SleepNs(limit_ns);
      return TimeoutError("no frame yet");
    }
    SleepNs(wait_ns);
  }
  next_deadline_ns_ += period_ns;
  Render(frame_index_);
  frame->data = buffer_.data();
  frame->bytes_used =
      uv_offset_ + static_cast<size_t>(stride_) * ((config_.height + 1) / 2);
  frame->fourcc = kFourccNv12;
  frame->width = config_.width;
  frame->height = config_.height;
  frame->stride = stride_;
  frame->sequence = static_cast<uint32_t>(frame_index_);
  frame->timestamp_ns = MonotonicNs();
  frame->buffer_index = 0;
  ++frame_index_;
  return Status::Ok();
}

void TestPatternSource::Release(const CapturedFrame& /*frame*/) {}

}  // namespace mw
