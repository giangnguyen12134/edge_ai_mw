// Copyright 2026 VMX Middleware Team.

#include "mw/media/sine_audio_source.h"

#include <math.h>
#include <poll.h>

#include "mw/common/time_util.h"

namespace mw {

SineAudioSource::SineAudioSource(const AudioStreamConfig& config)
    : config_(config), opened_(false), frames_generated_(0), start_ns_(0) {}

std::string SineAudioSource::Describe() const {
  return "sine " + std::to_string(config_.sample_rate) + " Hz x" +
         std::to_string(config_.channels);
}

Status SineAudioSource::Open() {
  opened_ = true;
  frames_generated_ = 0;
  start_ns_ = MonotonicNs();
  return Status::Ok();
}

void SineAudioSource::Close() { opened_ = false; }

Status SineAudioSource::Read(int16_t* out, size_t frames) {
  if (!opened_) {
    return UnavailableError("sine source is not open");
  }
  // Pace in real time: the last frame of this chunk is "due" at this moment.
  const uint64_t due_ns = start_ns_ + (frames_generated_ + frames) *
                                          1000000000ull / config_.sample_rate;
  const uint64_t now = MonotonicNs();
  if (due_ns > now) {
    poll(nullptr, 0, static_cast<int>((due_ns - now + 999999) / 1000000));
  }
  const double step = 2.0 * M_PI * 440.0 / config_.sample_rate;
  for (size_t i = 0; i < frames; ++i) {
    const int16_t sample = static_cast<int16_t>(
        8000.0 * sin(step * static_cast<double>(frames_generated_ + i)));
    for (uint32_t c = 0; c < config_.channels; ++c) {
      out[i * config_.channels + c] = sample;
    }
  }
  frames_generated_ += frames;
  return Status::Ok();
}

}  // namespace mw
