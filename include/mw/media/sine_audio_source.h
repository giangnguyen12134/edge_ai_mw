// Copyright 2026 Genio Middleware Authors.
//
// Real-time paced 440 Hz tone. For development machines and CI.

#ifndef MW_MEDIA_SINE_AUDIO_SOURCE_H_
#define MW_MEDIA_SINE_AUDIO_SOURCE_H_

#include "mw/common/macros.h"
#include "mw/media/audio_source.h"

namespace mw {

class SineAudioSource : public AudioSource {
 public:
  explicit SineAudioSource(const AudioStreamConfig& config);

  MW_DISALLOW_COPY_AND_ASSIGN(SineAudioSource);

  Status Open() override;
  void Close() override;
  Status Read(int16_t* out, size_t frames) override;
  uint64_t xrun_count() const override { return 0; }
  std::string Describe() const override;

 private:
  AudioStreamConfig config_;
  bool opened_;
  uint64_t frames_generated_;
  uint64_t start_ns_;
};

}  // namespace mw

#endif  // MW_MEDIA_SINE_AUDIO_SOURCE_H_
