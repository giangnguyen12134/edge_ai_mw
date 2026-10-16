// Copyright 2026 VMX Middleware Team.

#include "mw/media/audio_source.h"

#include "mw/media/alsa_audio_source.h"
#include "mw/media/sine_audio_source.h"

namespace mw {

Status CreateAudioSource(const AudioStreamConfig& config,
                         std::unique_ptr<AudioSource>* out) {
  if (config.source == "alsa") {
    out->reset(new AlsaAudioSource(config));
    return Status::Ok();
  }
  if (config.source == "sine") {
    out->reset(new SineAudioSource(config));
    return Status::Ok();
  }
  return InvalidArgumentError("unknown audio source '" + config.source + "'");
}

}  // namespace mw
