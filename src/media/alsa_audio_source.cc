// Copyright 2026 VMX Middleware Team.

#include "mw/media/alsa_audio_source.h"

#ifdef MW_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

namespace mw {

AlsaAudioSource::AlsaAudioSource(const AudioStreamConfig& config)
    : config_(config), pcm_(nullptr), xruns_(0) {}

AlsaAudioSource::~AlsaAudioSource() { Close(); }

std::string AlsaAudioSource::Describe() const {
  return "alsa:" + config_.device + " " + std::to_string(config_.sample_rate) +
         " Hz x" + std::to_string(config_.channels);
}

#ifdef MW_HAVE_ALSA

Status AlsaAudioSource::Open() {
  Close();
  snd_pcm_t* pcm = nullptr;
  int r = snd_pcm_open(&pcm, config_.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
  if (r < 0) {
    return UnavailableError("snd_pcm_open " + config_.device + ": " +
                            snd_strerror(r));
  }
  const snd_pcm_uframes_t period_frames =
      static_cast<snd_pcm_uframes_t>(config_.sample_rate) * config_.period_ms /
      1000;
  // 3 periods of headroom against scheduling jitter.
  const snd_pcm_uframes_t buffer_frames = period_frames * 4;
  r = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                         SND_PCM_ACCESS_RW_INTERLEAVED, config_.channels,
                         config_.sample_rate, 0 /*no soft resample*/,
                         static_cast<unsigned int>(buffer_frames * 1000000ull /
                                                   config_.sample_rate));
  if (r < 0) {
    const std::string message = snd_strerror(r);
    snd_pcm_close(pcm);
    return FailedPreconditionError("snd_pcm_set_params " + config_.device +
                                   ": " + message);
  }
  r = snd_pcm_prepare(pcm);
  if (r < 0) {
    const std::string message = snd_strerror(r);
    snd_pcm_close(pcm);
    return InternalError("snd_pcm_prepare: " + message);
  }
  pcm_ = pcm;
  return Status::Ok();
}

void AlsaAudioSource::Close() {
  if (pcm_ != nullptr) {
    snd_pcm_close(static_cast<snd_pcm_t*>(pcm_));
    pcm_ = nullptr;
  }
}

Status AlsaAudioSource::Read(int16_t* out, size_t frames) {
  if (pcm_ == nullptr) {
    return UnavailableError("alsa device is not open");
  }
  snd_pcm_t* pcm = static_cast<snd_pcm_t*>(pcm_);
  size_t done = 0;
  while (done < frames) {
    const snd_pcm_sframes_t n =
        snd_pcm_readi(pcm, out + done * config_.channels,
                      static_cast<snd_pcm_uframes_t>(frames - done));
    if (n > 0) {
      done += static_cast<size_t>(n);
      continue;
    }
    const int err = n < 0 ? static_cast<int>(n) : -EAGAIN;
    if (err == -ENODEV || err == -ENOTTY || err == -EIO) {
      return UnavailableError(std::string("alsa capture lost: ") +
                              snd_strerror(err));
    }
    if (err == -EPIPE || err == -ESTRPIPE) {
      ++xruns_;
    }
    const int rec = snd_pcm_recover(pcm, err, 1 /*silent*/);
    if (rec < 0) {
      return UnavailableError(std::string("alsa recover failed: ") +
                              snd_strerror(rec));
    }
  }
  return Status::Ok();
}

#else  // MW_HAVE_ALSA

Status AlsaAudioSource::Open() {
  return UnimplementedError(
      "built without ALSA; reconfigure with -DMW_ENABLE_ALSA=ON or use "
      "audio.source=sine");
}

void AlsaAudioSource::Close() {}

Status AlsaAudioSource::Read(int16_t* /*out*/, size_t /*frames*/) {
  return UnimplementedError("built without ALSA");
}

#endif  // MW_HAVE_ALSA

}  // namespace mw
