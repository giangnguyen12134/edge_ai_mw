// Copyright 2026 VMX Middleware Team.

#include "mw/media/v4l2_frame_source.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <sstream>

#include "mw/common/frame_types.h"
#include "mw/common/logging.h"
#include "mw/common/time_util.h"

namespace mw {
namespace {

int Xioctl(int fd, unsigned long request, void* arg) {
  int r;
  do {
    r = ioctl(fd, request, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

}  // namespace

V4l2FrameSource::V4l2FrameSource(const VideoStreamConfig& config)
    : config_(config), stride_(0), image_size_(0), streaming_(false) {}

V4l2FrameSource::~V4l2FrameSource() { CloseDevice(); }

void V4l2FrameSource::CloseDevice() {
  Stop();
  for (size_t i = 0; i < buffers_.size(); ++i) {
    if (buffers_[i].start != nullptr) {
      munmap(buffers_[i].start, buffers_[i].length);
    }
  }
  buffers_.clear();
  fd_.Reset();
}

Status V4l2FrameSource::Open() {
  CloseDevice();
  fd_.Reset(open(config_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
  if (!fd_.valid()) {
    return ErrnoStatus("open " + config_.device, errno);
  }

  struct v4l2_capability cap;
  memset(&cap, 0, sizeof(cap));
  if (Xioctl(fd_.get(), VIDIOC_QUERYCAP, &cap) < 0) {
    return ErrnoStatus("VIDIOC_QUERYCAP", errno);
  }
  const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                            ? cap.device_caps
                            : cap.capabilities;
  if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
    return FailedPreconditionError(config_.device +
                                   " is not a streaming capture device");
  }

  struct v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = config_.width;
  fmt.fmt.pix.height = config_.height;
  fmt.fmt.pix.pixelformat = config_.pixel_format;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  if (Xioctl(fd_.get(), VIDIOC_S_FMT, &fmt) < 0) {
    return ErrnoStatus("VIDIOC_S_FMT", errno);
  }
  if (fmt.fmt.pix.width != config_.width ||
      fmt.fmt.pix.height != config_.height ||
      fmt.fmt.pix.pixelformat != config_.pixel_format) {
    std::ostringstream os;
    os << config_.device << " cannot deliver " << config_.width << "x"
       << config_.height << " " << FourccToString(config_.pixel_format)
       << "; driver offers " << fmt.fmt.pix.width << "x" << fmt.fmt.pix.height
       << " " << FourccToString(fmt.fmt.pix.pixelformat);
    return FailedPreconditionError(os.str());
  }
  stride_ = fmt.fmt.pix.bytesperline;
  image_size_ = fmt.fmt.pix.sizeimage;

  struct v4l2_streamparm parm;
  memset(&parm, 0, sizeof(parm));
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = config_.fps;
  if (Xioctl(fd_.get(), VIDIOC_S_PARM, &parm) < 0) {
    MW_LOG(Warning) << "VIDIOC_S_PARM failed (" << strerror(errno)
                    << "); using the driver's default frame rate";
  } else if (parm.parm.capture.timeperframe.numerator != 0) {
    const double actual =
        static_cast<double>(parm.parm.capture.timeperframe.denominator) /
        parm.parm.capture.timeperframe.numerator;
    if (actual + 0.5 < config_.fps) {
      MW_LOG(Warning) << "camera runs at " << actual << " fps, requested "
                      << config_.fps;
    }
  }

  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = config_.v4l2_buffers;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (Xioctl(fd_.get(), VIDIOC_REQBUFS, &req) < 0) {
    return ErrnoStatus("VIDIOC_REQBUFS", errno);
  }
  if (req.count < 2) {
    return ResourceExhaustedError("driver granted fewer than 2 buffers");
  }
  buffers_.resize(req.count);
  for (uint32_t i = 0; i < req.count; ++i) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (Xioctl(fd_.get(), VIDIOC_QUERYBUF, &buf) < 0) {
      return ErrnoStatus("VIDIOC_QUERYBUF", errno);
    }
    void* start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd_.get(), buf.m.offset);
    if (start == MAP_FAILED) {
      return ErrnoStatus("mmap V4L2 buffer", errno);
    }
    buffers_[i].start = start;
    buffers_[i].length = buf.length;
  }
  return Status::Ok();
}

Status V4l2FrameSource::Start() {
  for (uint32_t i = 0; i < buffers_.size(); ++i) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (Xioctl(fd_.get(), VIDIOC_QBUF, &buf) < 0) {
      return ErrnoStatus("VIDIOC_QBUF", errno);
    }
  }
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (Xioctl(fd_.get(), VIDIOC_STREAMON, &type) < 0) {
    return ErrnoStatus("VIDIOC_STREAMON", errno);
  }
  streaming_ = true;
  return Status::Ok();
}

void V4l2FrameSource::Stop() {
  if (streaming_ && fd_.valid()) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    Xioctl(fd_.get(), VIDIOC_STREAMOFF, &type);
  }
  streaming_ = false;
}

Status V4l2FrameSource::Capture(int timeout_ms, CapturedFrame* frame) {
  const uint64_t deadline_ns =
      MonotonicNs() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
  while (true) {
    const uint64_t now_ns = MonotonicNs();
    const int remaining_ms =
        now_ns >= deadline_ns
            ? 0
            : static_cast<int>((deadline_ns - now_ns + 999999ull) / 1000000ull);
    struct pollfd pfd;
    pfd.fd = fd_.get();
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int pr = poll(&pfd, 1, remaining_ms);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      return ErrnoStatus("poll V4L2", errno);
    }
    if (pr == 0) {
      return TimeoutError("no frame within timeout");
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      return UnavailableError(config_.device + " vanished (hang-up)");
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (Xioctl(fd_.get(), VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EAGAIN) {
        continue;
      }
      return ErrnoStatus("VIDIOC_DQBUF", errno);
    }
    const bool corrupt = (buf.flags & V4L2_BUF_FLAG_ERROR) != 0;
    const bool short_frame =
        config_.pixel_format != kFourccMjpeg && buf.bytesused < image_size_;
    if (corrupt || short_frame) {
      MW_LOG(Debug) << "dropping bad V4L2 frame (seq " << buf.sequence << ")";
      Xioctl(fd_.get(), VIDIOC_QBUF, &buf);
      continue;
    }
    frame->data = static_cast<const uint8_t*>(buffers_[buf.index].start);
    frame->bytes_used = buf.bytesused;
    frame->fourcc = config_.pixel_format;
    frame->width = config_.width;
    frame->height = config_.height;
    frame->stride = config_.pixel_format == kFourccMjpeg ? 0 : stride_;
    frame->sequence = buf.sequence;
    frame->buffer_index = static_cast<int>(buf.index);
    if ((buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
        V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
      frame->timestamp_ns =
          static_cast<uint64_t>(buf.timestamp.tv_sec) * 1000000000ull +
          static_cast<uint64_t>(buf.timestamp.tv_usec) * 1000ull;
    } else {
      frame->timestamp_ns = MonotonicNs();
    }
    return Status::Ok();
  }
}

void V4l2FrameSource::Release(const CapturedFrame& frame) {
  if (frame.buffer_index < 0) {
    return;
  }
  struct v4l2_buffer buf;
  memset(&buf, 0, sizeof(buf));
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = static_cast<uint32_t>(frame.buffer_index);
  Xioctl(fd_.get(), VIDIOC_QBUF, &buf);
}

std::string V4l2FrameSource::Describe() const {
  std::ostringstream os;
  os << "v4l2:" << config_.device << " " << config_.width << "x"
     << config_.height << "@" << config_.fps << " "
     << FourccToString(config_.pixel_format);
  return os.str();
}

}  // namespace mw
