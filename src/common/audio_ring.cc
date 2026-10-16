// Copyright 2026 Genio Middleware Authors.

#include "mw/common/audio_ring.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <algorithm>
#include <memory>
#include <new>

#include "mw/common/buffer_allocator.h"

namespace mw {

static_assert(sizeof(AudioRingHeader) <= kAudioRingDataOffset,
              "ring header must fit before the data region");
static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
              "64-bit atomics must be lock-free for shared memory use");

AudioRingWriter::AudioRingWriter()
    : base_(nullptr),
      map_size_(0),
      header_(nullptr),
      data_(nullptr),
      sample_rate_(0),
      channels_(0),
      capacity_frames_(0) {}

AudioRingWriter::~AudioRingWriter() {
  if (base_ != nullptr) {
    munmap(base_, map_size_);
  }
}

Status AudioRingWriter::Create(uint32_t sample_rate, uint32_t channels,
                               uint32_t capacity_ms) {
  if (sample_rate == 0 || channels == 0 || capacity_ms == 0) {
    return InvalidArgumentError("audio ring parameters must be non-zero");
  }
  const uint32_t capacity_frames = static_cast<uint32_t>(
      static_cast<uint64_t>(sample_rate) * capacity_ms / 1000);
  const size_t bytes_per_frame = sizeof(int16_t) * channels;
  const size_t map_size =
      kAudioRingDataOffset +
      static_cast<size_t>(capacity_frames) * bytes_per_frame;

  std::unique_ptr<BufferAllocator> allocator;
  MW_RETURN_IF_ERROR(CreateBufferAllocator("memfd", &allocator));
  MW_RETURN_IF_ERROR(allocator->Allocate(map_size, &fd_));

  void* base =
      mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_.get(), 0);
  if (base == MAP_FAILED) {
    return ErrnoStatus("mmap audio ring", errno);
  }
  base_ = base;
  map_size_ = map_size;
  memset(base_, 0, kAudioRingDataOffset);
  header_ = new (base_) AudioRingHeader();
  header_->magic = kAudioRingMagic;
  header_->version = kAudioRingVersion;
  header_->sample_rate = sample_rate;
  header_->channels = channels;
  header_->capacity_frames = capacity_frames;
  header_->write_frames.store(0, std::memory_order_release);
  data_ = reinterpret_cast<int16_t*>(static_cast<uint8_t*>(base_) +
                                     kAudioRingDataOffset);
  sample_rate_ = sample_rate;
  channels_ = channels;
  capacity_frames_ = capacity_frames;
  return Status::Ok();
}

void AudioRingWriter::Write(const int16_t* samples, size_t frames) {
  if (header_ == nullptr || frames == 0) {
    return;
  }
  uint64_t pos = header_->write_frames.load(std::memory_order_relaxed);
  if (frames > capacity_frames_) {
    samples += (frames - capacity_frames_) * channels_;
    pos += frames - capacity_frames_;
    frames = capacity_frames_;
  }
  size_t start = static_cast<size_t>(pos % capacity_frames_);
  const size_t first = std::min(frames, capacity_frames_ - start);
  memcpy(data_ + start * channels_, samples,
         first * channels_ * sizeof(int16_t));
  if (frames > first) {
    memcpy(data_, samples + first * channels_,
           (frames - first) * channels_ * sizeof(int16_t));
  }
  header_->write_frames.store(pos + frames, std::memory_order_release);
}

AudioRingReader::AudioRingReader()
    : base_(nullptr),
      map_size_(0),
      header_(nullptr),
      data_(nullptr),
      sample_rate_(0),
      channels_(0),
      capacity_frames_(0) {}

AudioRingReader::~AudioRingReader() {
  if (base_ != nullptr) {
    munmap(base_, map_size_);
  }
}

Status AudioRingReader::Attach(int fd) {
  fd_ = ScopedFd::DuplicateFrom(fd);
  if (!fd_.valid()) {
    return ErrnoStatus("dup audio ring fd", errno);
  }
  struct stat st;
  if (fstat(fd_.get(), &st) < 0) {
    return ErrnoStatus("fstat audio ring", errno);
  }
  if (static_cast<size_t>(st.st_size) < kAudioRingDataOffset) {
    return InvalidArgumentError("audio ring too small");
  }
  const size_t map_size = static_cast<size_t>(st.st_size);
  void* base = mmap(nullptr, map_size, PROT_READ, MAP_SHARED, fd_.get(), 0);
  if (base == MAP_FAILED) {
    return ErrnoStatus("mmap audio ring", errno);
  }
  const AudioRingHeader* header = static_cast<const AudioRingHeader*>(base);
  if (header->magic != kAudioRingMagic ||
      header->version != kAudioRingVersion || header->channels == 0 ||
      header->capacity_frames == 0 ||
      kAudioRingDataOffset + static_cast<size_t>(header->capacity_frames) *
                                 header->channels * sizeof(int16_t) >
          map_size) {
    munmap(base, map_size);
    return InvalidArgumentError("bad audio ring header");
  }
  base_ = base;
  map_size_ = map_size;
  header_ = header;
  data_ = reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(base) +
                                           kAudioRingDataOffset);
  sample_rate_ = header->sample_rate;
  channels_ = header->channels;
  capacity_frames_ = header->capacity_frames;
  return Status::Ok();
}

uint64_t AudioRingReader::WritePosition() const {
  return header_ == nullptr
             ? 0
             : header_->write_frames.load(std::memory_order_acquire);
}

size_t AudioRingReader::Read(uint64_t* cursor, int16_t* out, size_t max_frames,
                             uint64_t* lost_frames) const {
  if (header_ == nullptr) {
    return 0;
  }
  const uint64_t capacity = capacity_frames_;
  const uint64_t write_pos =
      header_->write_frames.load(std::memory_order_acquire);
  if (*cursor > write_pos) {
    *cursor = write_pos;
  }
  if (write_pos - *cursor > capacity) {
    if (lost_frames != nullptr) {
      *lost_frames += write_pos - *cursor - capacity;
    }
    *cursor = write_pos - capacity;
  }
  const size_t count =
      static_cast<size_t>(std::min<uint64_t>(write_pos - *cursor, max_frames));
  if (count == 0) {
    return 0;
  }
  const size_t start = static_cast<size_t>(*cursor % capacity);
  const size_t first = std::min<size_t>(count, capacity - start);
  memcpy(out, data_ + start * channels_, first * channels_ * sizeof(int16_t));
  if (count > first) {
    memcpy(out + first * channels_, data_,
           (count - first) * channels_ * sizeof(int16_t));
  }
  // Detect that the writer overwrote the region while we were copying.
  const uint64_t write_pos_after =
      header_->write_frames.load(std::memory_order_acquire);
  if (write_pos_after - *cursor > capacity) {
    if (lost_frames != nullptr) {
      *lost_frames += count;
    }
    *cursor = write_pos_after - capacity;
    return 0;
  }
  *cursor += count;
  return count;
}

}  // namespace mw
