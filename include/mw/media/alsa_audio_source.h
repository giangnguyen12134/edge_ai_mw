// Copyright 2026 Genio Middleware Authors.
//
// ALSA capture (S16_LE, interleaved). Built only when ALSA is available
// (MW_HAVE_ALSA); otherwise Open() reports Unimplemented.

#ifndef MW_MEDIA_ALSA_AUDIO_SOURCE_H_
#define MW_MEDIA_ALSA_AUDIO_SOURCE_H_

#include "mw/common/macros.h"
#include "mw/media/audio_source.h"

namespace mw {

class AlsaAudioSource : public AudioSource {
 public:
  explicit AlsaAudioSource(const AudioStreamConfig& config);
  ~AlsaAudioSource() override;

  MW_DISALLOW_COPY_AND_ASSIGN(AlsaAudioSource);

  Status Open() override;
  void Close() override;
  Status Read(int16_t* out, size_t frames) override;
  uint64_t xrun_count() const override { return xruns_; }
  std::string Describe() const override;

 private:
  AudioStreamConfig config_;
  void* pcm_;  // snd_pcm_t*, opaque so this header needs no ALSA include.
  uint64_t xruns_;
};

}  // namespace mw

#endif  // MW_MEDIA_ALSA_AUDIO_SOURCE_H_
