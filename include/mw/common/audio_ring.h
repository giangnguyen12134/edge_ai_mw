// Copyright 2026 Genio Middleware Authors.
//
// Single-writer / multi-reader lock-free ring buffer of interleaved S16LE PCM
// frames placed in a shared memfd. Audio is only ~32 KB/s at 16 kHz mono so a
// plain shared ring is enough; no per-chunk D-Bus traffic is needed. Each
// reader keeps its own cursor and is never able to stall the writer.

#ifndef MW_COMMON_AUDIO_RING_H_
#define MW_COMMON_AUDIO_RING_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "mw/common/macros.h"
#include "mw/common/scoped_fd.h"
#include "mw/common/status.h"

namespace mw {

// Layout of the shared memory header. Public so readers in other processes
// can interpret the mapping; treat as read-only outside this module.
struct AudioRingHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t capacity_frames;
  uint32_t reserved[3];
  std::atomic<uint64_t> write_frames;  // Total frames ever written.
};

const uint32_t kAudioRingMagic = 0x5241574d;  // "MWAR"
const uint32_t kAudioRingVersion = 1;
const size_t kAudioRingDataOffset = 64;

class AudioRingWriter {
 public:
  AudioRingWriter();
  ~AudioRingWriter();

  MW_DISALLOW_COPY_AND_ASSIGN(AudioRingWriter);

  // Creates a ring holding |capacity_ms| of audio.
  Status Create(uint32_t sample_rate, uint32_t channels, uint32_t capacity_ms);

  // Descriptor to hand to readers. Owned by the writer.
  int fd() const { return fd_.get(); }
  uint32_t sample_rate() const { return sample_rate_; }
  uint32_t channels() const { return channels_; }
  uint32_t capacity_frames() const { return capacity_frames_; }

  // Appends |frames| interleaved frames, overwriting the oldest data.
  void Write(const int16_t* samples, size_t frames);

 private:
  ScopedFd fd_;
  void* base_;
  size_t map_size_;
  AudioRingHeader* header_;
  int16_t* data_;
  uint32_t sample_rate_;
  uint32_t channels_;
  uint32_t capacity_frames_;
};

class AudioRingReader {
 public:
  AudioRingReader();
  ~AudioRingReader();

  MW_DISALLOW_COPY_AND_ASSIGN(AudioRingReader);

  // Maps the ring behind |fd| read-only. |fd| is duplicated.
  Status Attach(int fd);

  uint32_t sample_rate() const { return sample_rate_; }
  uint32_t channels() const { return channels_; }
  uint32_t capacity_frames() const { return capacity_frames_; }

  // Total frames written so far.
  uint64_t WritePosition() const;

  // Copies up to |max_frames| frames starting at |*cursor| and advances the
  // cursor. If the writer lapped the reader, |*cursor| skips forward and
  // |*lost_frames| (if non-null) is incremented. Returns frames copied.
  size_t Read(uint64_t* cursor, int16_t* out, size_t max_frames,
              uint64_t* lost_frames) const;

 private:
  ScopedFd fd_;
  void* base_;
  size_t map_size_;
  const AudioRingHeader* header_;
  const int16_t* data_;
  uint32_t sample_rate_;
  uint32_t channels_;
  uint32_t capacity_frames_;
};

}  // namespace mw

#endif  // MW_COMMON_AUDIO_RING_H_
