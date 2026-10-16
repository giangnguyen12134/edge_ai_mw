// Copyright 2026 Genio Middleware Authors.
//
// Abstract microphone delivering interleaved S16LE PCM. The RT1061 USB audio
// device shows up as an ALSA capture card, so ALSA is the production backend.

#ifndef MW_MEDIA_AUDIO_SOURCE_H_
#define MW_MEDIA_AUDIO_SOURCE_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>

#include "mw/common/status.h"
#include "mw/media/media_config.h"

namespace mw {

class AudioSource {
 public:
  virtual ~AudioSource() {}

  virtual Status Open() = 0;
  virtual void Close() = 0;

  // Blocks until exactly |frames| frames are read into |out| (interleaved).
  //   kUnavailable  the device went away; Close() and Open() again.
  //   kTimeout      nothing arrived for a long while.
  virtual Status Read(int16_t* out, size_t frames) = 0;

  // Number of xruns (overruns) recovered so far.
  virtual uint64_t xrun_count() const = 0;

  virtual std::string Describe() const = 0;
};

Status CreateAudioSource(const AudioStreamConfig& config,
                         std::unique_ptr<AudioSource>* out);

}  // namespace mw

#endif  // MW_MEDIA_AUDIO_SOURCE_H_
