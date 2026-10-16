// Copyright 2026 VMX Middleware Team.

#include "mw/media/synthetic_v4l2_device.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>

#include "mw/common/frame_types.h"
#include "mw/common/time_util.h"

namespace mw {
namespace {

const uint32_t kMaxBuffers = 32;
const uint32_t kPage = 4096;

int Fail(int error_number) {
  errno = error_number;
  return -1;
}

}  // namespace

SyntheticV4l2Device::SyntheticV4l2Device(uint32_t default_fps)
    : default_fps_(default_fps == 0 ? 30 : default_fps),
      fps_(default_fps_),
      width_(0),
      height_(0),
      fourcc_(0),
      bytes_per_line_(0),
      size_image_(0),
      buffer_length_(0),
      memory_(0),
      streaming_(false),
      sequence_(0) {}

SyntheticV4l2Device::~SyntheticV4l2Device() { Close(); }

Status SyntheticV4l2Device::Open(const std::string& /*path*/) {
  Close();
  timer_.Reset(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
  if (!timer_.valid()) {
    return ErrnoStatus("timerfd_create", errno);
  }
  fps_ = default_fps_;
  sequence_ = 0;
  return Status::Ok();
}

void SyntheticV4l2Device::Close() {
  FreeBuffers();
  streaming_ = false;
  timer_.Reset();
}

void SyntheticV4l2Device::FreeBuffers() {
  for (size_t i = 0; i < buffers_.size(); ++i) {
    if (buffers_[i].mmap_base != nullptr) {
      munmap(buffers_[i].mmap_base, buffer_length_);
    }
    if (buffers_[i].map_base != nullptr) {
      munmap(buffers_[i].map_base, buffer_length_);
    }
  }
  buffers_.clear();
  queue_.clear();
  memory_ = 0;
}

void SyntheticV4l2Device::SetTimer(bool on) {
  struct itimerspec spec;
  memset(&spec, 0, sizeof(spec));
  if (on) {
    const long period_ns = static_cast<long>(1000000000ull / fps_);
    spec.it_interval.tv_nsec = period_ns % 1000000000L;
    spec.it_interval.tv_sec = period_ns / 1000000000L;
    spec.it_value = spec.it_interval;
  }
  timerfd_settime(timer_.get(), 0, &spec, nullptr);
}

void SyntheticV4l2Device::FillPattern(uint32_t fourcc, uint32_t sequence,
                                      uint8_t* data, size_t size,
                                      size_t* bytes_used) {
  if (fourcc == kFourccMjpeg) {
    const size_t used = std::min<size_t>(
        size, size / 8 + 16 + static_cast<size_t>(sequence % 4) * 97);
    for (size_t i = 0; i < used; ++i) {
      data[i] = static_cast<uint8_t>(i + sequence);
    }
    if (used >= 4) {
      data[0] = 0xFF;
      data[1] = 0xD8;
      data[used - 2] = 0xFF;
      data[used - 1] = 0xD9;
    }
    *bytes_used = used;
    return;
  }
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>(i + sequence);
  }
  *bytes_used = size;
}

int SyntheticV4l2Device::Ioctl(unsigned long request, void* arg) {
  if (!timer_.valid()) {
    return Fail(EBADF);
  }
  switch (request) {
    case VIDIOC_QUERYCAP: {
      struct v4l2_capability* cap = static_cast<struct v4l2_capability*>(arg);
      memset(cap, 0, sizeof(*cap));
      strncpy(reinterpret_cast<char*>(cap->driver), "mw-synthetic",
              sizeof(cap->driver) - 1);
      cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
      cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
      return 0;
    }
    case VIDIOC_S_FMT: {
      struct v4l2_format* fmt = static_cast<struct v4l2_format*>(arg);
      if (fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        return Fail(EINVAL);
      }
      if (memory_ != 0) {
        return Fail(EBUSY);
      }
      uint32_t w = fmt->fmt.pix.width;
      uint32_t h = fmt->fmt.pix.height;
      uint32_t f = fmt->fmt.pix.pixelformat;
      if (f != kFourccNv12 && f != kFourccYuyv && f != kFourccMjpeg) {
        f = kFourccYuyv;  // Like a real driver: offer what it supports.
        w = 640;
        h = 480;
      }
      width_ = w;
      height_ = h;
      fourcc_ = f;
      bytes_per_line_ = f == kFourccYuyv ? w * 2 : (f == kFourccNv12 ? w : 0);
      size_image_ = f == kFourccNv12 ? w * h * 3 / 2
                                     : (f == kFourccYuyv ? w * h * 2 : w * h);
      fmt->fmt.pix.width = width_;
      fmt->fmt.pix.height = height_;
      fmt->fmt.pix.pixelformat = fourcc_;
      fmt->fmt.pix.bytesperline = bytes_per_line_;
      fmt->fmt.pix.sizeimage = size_image_;
      fmt->fmt.pix.field = V4L2_FIELD_NONE;
      return 0;
    }
    case VIDIOC_S_PARM: {
      struct v4l2_streamparm* parm = static_cast<struct v4l2_streamparm*>(arg);
      const uint32_t num = parm->parm.capture.timeperframe.numerator;
      const uint32_t den = parm->parm.capture.timeperframe.denominator;
      if (num != 0 && den != 0 && den / num >= 1 && den / num <= 240) {
        fps_ = den / num;
      }
      parm->parm.capture.timeperframe.numerator = 1;
      parm->parm.capture.timeperframe.denominator = fps_;
      return 0;
    }
    case VIDIOC_REQBUFS:
      return DoReqbufs(arg);
    case VIDIOC_QUERYBUF: {
      struct v4l2_buffer* buf = static_cast<struct v4l2_buffer*>(arg);
      if (memory_ != V4L2_MEMORY_MMAP || buf->index >= buffers_.size()) {
        return Fail(EINVAL);
      }
      buf->length = buffer_length_;
      buf->m.offset = buf->index * buffer_length_;
      return 0;
    }
    case VIDIOC_EXPBUF:
      return DoExpbuf(arg);
    case VIDIOC_QBUF:
      return DoQbuf(arg);
    case VIDIOC_DQBUF:
      return DoDqbuf(arg);
    case VIDIOC_STREAMON:
      if (memory_ == 0) {
        return Fail(EINVAL);
      }
      streaming_ = true;
      SetTimer(true);
      return 0;
    case VIDIOC_STREAMOFF: {
      streaming_ = false;
      SetTimer(false);
      for (size_t i = 0; i < buffers_.size(); ++i) {
        buffers_[i].queued = false;
        if (buffers_[i].map_base != nullptr) {
          munmap(buffers_[i].map_base, buffer_length_);
          buffers_[i].map_base = nullptr;
        }
        buffers_[i].imported.Reset();
      }
      queue_.clear();
      return 0;
    }
    default:
      return Fail(ENOTTY);
  }
}

int SyntheticV4l2Device::DoReqbufs(void* arg) {
  struct v4l2_requestbuffers* req =
      static_cast<struct v4l2_requestbuffers*>(arg);
  if (req->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || size_image_ == 0) {
    return Fail(EINVAL);
  }
  if (streaming_) {
    return Fail(EBUSY);
  }
  FreeBuffers();
  if (req->count == 0) {
    return 0;
  }
  if (req->memory == V4L2_MEMORY_MMAP) {
    if (faults_.fail_mmap_reqbufs) {
      return Fail(EINVAL);
    }
  } else if (req->memory == V4L2_MEMORY_DMABUF) {
    if (faults_.fail_dmabuf_reqbufs) {
      return Fail(EINVAL);
    }
  } else {
    return Fail(EINVAL);
  }
  const uint32_t count = std::min(req->count, kMaxBuffers);
  buffer_length_ = AlignUp(size_image_, kPage);
  buffers_.resize(count);
  memory_ = req->memory;
  if (memory_ == V4L2_MEMORY_MMAP) {
    for (uint32_t i = 0; i < count; ++i) {
      const int fd = static_cast<int>(
          syscall(SYS_memfd_create, "mw-synthetic-buf", 1u /*CLOEXEC*/));
      if (fd < 0) {
        const int saved = errno;
        FreeBuffers();
        return Fail(saved);
      }
      buffers_[i].memfd.Reset(fd);
      void* p = MAP_FAILED;
      if (ftruncate(fd, static_cast<off_t>(buffer_length_)) == 0) {
        p = mmap(nullptr, buffer_length_, PROT_READ | PROT_WRITE, MAP_SHARED,
                 fd, 0);
      }
      if (p == MAP_FAILED) {
        const int saved = errno;
        FreeBuffers();
        return Fail(saved);
      }
      buffers_[i].mmap_base = static_cast<uint8_t*>(p);
    }
  }
  req->count = count;
  return 0;
}

int SyntheticV4l2Device::DoExpbuf(void* arg) {
  struct v4l2_exportbuffer* exp = static_cast<struct v4l2_exportbuffer*>(arg);
  if (faults_.fail_expbuf) {
    return Fail(ENOTTY);
  }
  if (memory_ != V4L2_MEMORY_MMAP || exp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
      exp->index >= buffers_.size()) {
    return Fail(EINVAL);
  }
  const int fd = fcntl(buffers_[exp->index].memfd.get(), F_DUPFD_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  exp->fd = fd;
  return 0;
}

int SyntheticV4l2Device::DoQbuf(void* arg) {
  struct v4l2_buffer* buf = static_cast<struct v4l2_buffer*>(arg);
  if (buf->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || memory_ == 0 ||
      buf->memory != memory_ || buf->index >= buffers_.size()) {
    return Fail(EINVAL);
  }
  Buffer& b = buffers_[buf->index];
  if (b.queued) {
    return Fail(EINVAL);
  }
  if (memory_ == V4L2_MEMORY_DMABUF) {
    if (faults_.fail_dmabuf_qbuf || buf->m.fd < 0 ||
        buf->length < size_image_) {
      return Fail(EINVAL);
    }
    ScopedFd dup = ScopedFd::DuplicateFrom(buf->m.fd);
    if (!dup.valid()) {
      return Fail(EBADF);
    }
    void* p = mmap(nullptr, buffer_length_, PROT_READ | PROT_WRITE, MAP_SHARED,
                   dup.get(), 0);
    if (p == MAP_FAILED) {
      return Fail(EINVAL);  // Not a mappable buffer.
    }
    b.imported = std::move(dup);
    b.map_base = static_cast<uint8_t*>(p);
  }
  b.queued = true;
  queue_.push_back(buf->index);
  return 0;
}

int SyntheticV4l2Device::DoDqbuf(void* arg) {
  struct v4l2_buffer* buf = static_cast<struct v4l2_buffer*>(arg);
  if (!streaming_) {
    return Fail(EINVAL);
  }
  if (faults_.vanish_after_frames != 0 &&
      sequence_ >= faults_.vanish_after_frames) {
    return Fail(ENODEV);
  }
  uint64_t expirations = 0;
  if (read(timer_.get(), &expirations, sizeof(expirations)) !=
      static_cast<ssize_t>(sizeof(expirations))) {
    return Fail(EAGAIN);
  }
  if (queue_.empty()) {
    return Fail(EAGAIN);  // No buffer: this frame period is lost.
  }
  const uint32_t index = queue_.front();
  queue_.erase(queue_.begin());
  Buffer& b = buffers_[index];
  uint8_t* data = memory_ == V4L2_MEMORY_MMAP ? b.mmap_base : b.map_base;
  size_t used = 0;
  FillPattern(fourcc_, sequence_, data, size_image_, &used);
  if (memory_ == V4L2_MEMORY_DMABUF) {
    munmap(b.map_base, buffer_length_);
    b.map_base = nullptr;
    b.imported.Reset();
  }
  b.queued = false;

  memset(buf, 0, sizeof(*buf));
  buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf->memory = memory_;
  buf->index = index;
  buf->bytesused = static_cast<uint32_t>(used);
  buf->length = buffer_length_;
  buf->sequence = sequence_;
  buf->flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
  const uint64_t now = MonotonicNs();
  buf->timestamp.tv_sec = static_cast<time_t>(now / 1000000000ull);
  buf->timestamp.tv_usec = static_cast<suseconds_t>(now % 1000000000ull / 1000);
  ++sequence_;
  if (faults_.stall_after_frames != 0 &&
      sequence_ >= faults_.stall_after_frames) {
    SetTimer(false);
  }
  return 0;
}

}  // namespace mw
