// Copyright 2026 VMX Middleware Team.

#include "mw/media/v4l2_dmabuf_source.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <sstream>

#include "mw/common/frame_types.h"
#include "mw/common/logging.h"
#include "mw/common/time_util.h"
#include "mw/media/synthetic_v4l2_device.h"

namespace mw {
namespace {

const uint32_t kPage = 4096;

}  // namespace

V4l2DmaBufSource::V4l2DmaBufSource(const VideoStreamConfig& config,
                                   std::unique_ptr<V4l2Device> device)
    : config_(config),
      device_(std::move(device)),
      mode_(kIoNone),
      buffer_length_(0),
      buffer_count_(0),
      queued_(0),
      bad_frames_(0),
      streaming_(false),
      wake_fd_(-1) {}

V4l2DmaBufSource::~V4l2DmaBufSource() {
  Stop();
  ReleaseBuffers();
  device_->Close();
}

std::string V4l2DmaBufSource::io_mode() const {
  switch (mode_) {
    case kIoExpbuf:
      return "expbuf";
    case kIoDmabuf:
      return "dmabuf";
    case kIoNone:
      break;
  }
  return "none";
}

void V4l2DmaBufSource::ReleaseBuffers() {
  if (mode_ != kIoNone && device_->fd() >= 0) {
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = mode_ == kIoExpbuf ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF;
    req.count = 0;
    device_->Ioctl(VIDIOC_REQBUFS, &req);
  }
  import_fds_.clear();
  mode_ = kIoNone;
  buffer_count_ = 0;
  queued_ = 0;
}

Status V4l2DmaBufSource::Open(CameraFormat* out) {
  Stop();
  ReleaseBuffers();
  device_->Close();
  MW_RETURN_IF_ERROR(device_->Open(config_.device));

  struct v4l2_capability cap;
  memset(&cap, 0, sizeof(cap));
  if (device_->Ioctl(VIDIOC_QUERYCAP, &cap) < 0) {
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
  if (device_->Ioctl(VIDIOC_S_FMT, &fmt) < 0) {
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
  format_.width = fmt.fmt.pix.width;
  format_.height = fmt.fmt.pix.height;
  format_.fourcc = fmt.fmt.pix.pixelformat;
  format_.bytes_per_line = fmt.fmt.pix.bytesperline;
  format_.size_image = fmt.fmt.pix.sizeimage;
  if (format_.size_image == 0) {
    return FailedPreconditionError("driver reported sizeimage 0");
  }

  struct v4l2_streamparm parm;
  memset(&parm, 0, sizeof(parm));
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = config_.fps;
  if (device_->Ioctl(VIDIOC_S_PARM, &parm) < 0) {
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
  *out = format_;
  return Status::Ok();
}

Status V4l2DmaBufSource::Prepare(uint32_t count, BufferAllocator* allocator,
                                 std::vector<ScopedFd>* fds,
                                 uint32_t* buffer_length) {
  if (format_.size_image == 0) {
    return FailedPreconditionError("Prepare() before Open()");
  }
  ReleaseBuffers();
  const std::string& io = config_.capture_io;
  bool order_dmabuf_first = false;
  bool try_expbuf = true;
  bool try_dmabuf = true;
  if (io == "expbuf") {
    try_dmabuf = false;
  } else if (io == "dmabuf") {
    try_expbuf = false;
  } else {
    order_dmabuf_first = allocator != nullptr && allocator->IsDmaBuf();
  }

  Status last = FailedPreconditionError("no capture I/O method enabled");
  for (int pass = 0; pass < 2; ++pass) {
    const bool dmabuf = (pass == 0) == order_dmabuf_first;
    if (dmabuf ? !try_dmabuf : !try_expbuf) {
      continue;
    }
    fds->clear();
    last = dmabuf ? PrepareDmabuf(count, allocator, fds, buffer_length)
                  : PrepareExpbuf(count, fds, buffer_length);
    if (last.ok()) {
      return last;
    }
    MW_LOG(Warning) << (dmabuf ? "dmabuf" : "expbuf")
                    << " capture setup failed: " << last.ToString();
    fds->clear();
    ReleaseBuffers();
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    device_->Ioctl(VIDIOC_STREAMOFF, &type);
  }
  return last;
}

Status V4l2DmaBufSource::PrepareExpbuf(uint32_t count,
                                       std::vector<ScopedFd>* fds,
                                       uint32_t* buffer_length) {
  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = count;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (device_->Ioctl(VIDIOC_REQBUFS, &req) < 0) {
    return ErrnoStatus("VIDIOC_REQBUFS(MMAP)", errno);
  }
  mode_ = kIoExpbuf;
  if (req.count < 2) {
    return ResourceExhaustedError("driver granted fewer than 2 buffers");
  }
  uint32_t length = 0;
  for (uint32_t i = 0; i < req.count; ++i) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (device_->Ioctl(VIDIOC_QUERYBUF, &buf) < 0) {
      return ErrnoStatus("VIDIOC_QUERYBUF", errno);
    }
    if (length != 0 && buf.length != length) {
      return FailedPreconditionError("driver buffers differ in size");
    }
    length = buf.length;

    struct v4l2_exportbuffer exp;
    memset(&exp, 0, sizeof(exp));
    exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    exp.index = i;
    exp.flags = O_RDWR | O_CLOEXEC;
    if (device_->Ioctl(VIDIOC_EXPBUF, &exp) < 0) {
      return ErrnoStatus("VIDIOC_EXPBUF", errno);
    }
    fds->push_back(ScopedFd(exp.fd));
  }
  if (length < format_.size_image) {
    return FailedPreconditionError("driver buffers smaller than sizeimage");
  }
  buffer_count_ = req.count;
  buffer_length_ = length;
  *buffer_length = length;
  return Status::Ok();
}

Status V4l2DmaBufSource::PrepareDmabuf(uint32_t count,
                                       BufferAllocator* allocator,
                                       std::vector<ScopedFd>* fds,
                                       uint32_t* buffer_length) {
  if (allocator == nullptr) {
    return FailedPreconditionError("dmabuf capture needs an allocator");
  }
  const uint32_t length = AlignUp(format_.size_image, kPage);
  std::vector<ScopedFd> own;
  for (uint32_t i = 0; i < count; ++i) {
    ScopedFd fd;
    MW_RETURN_IF_ERROR(allocator->Allocate(length, &fd));
    own.push_back(std::move(fd));
  }
  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = count;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_DMABUF;
  if (device_->Ioctl(VIDIOC_REQBUFS, &req) < 0) {
    return ErrnoStatus("VIDIOC_REQBUFS(DMABUF)", errno);
  }
  mode_ = kIoDmabuf;
  if (req.count < 2) {
    return ResourceExhaustedError("driver granted fewer than 2 buffers");
  }
  if (req.count < own.size()) {
    own.resize(req.count);
  }
  buffer_length_ = length;
  buffer_count_ = static_cast<uint32_t>(own.size());
  for (size_t i = 0; i < own.size(); ++i) {
    ScopedFd copy = own[i].Duplicate();
    if (!copy.valid()) {
      return ErrnoStatus("dup dma-buf", errno);
    }
    import_fds_.push_back(std::move(copy));
  }

  // Some drivers only reject a foreign dma-buf at QBUF time (for example
  // memfd buffers, or heaps the device cannot address). Find out now so that
  // "auto" can still fall back to EXPBUF.
  MW_RETURN_IF_ERROR(QueueIndex(0));
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  device_->Ioctl(VIDIOC_STREAMOFF, &type);
  queued_ = 0;

  for (size_t i = 0; i < own.size(); ++i) {
    fds->push_back(std::move(own[i]));
  }
  *buffer_length = length;
  return Status::Ok();
}

Status V4l2DmaBufSource::QueueIndex(uint32_t index) {
  struct v4l2_buffer buf;
  memset(&buf, 0, sizeof(buf));
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.index = index;
  if (mode_ == kIoDmabuf) {
    if (index >= import_fds_.size()) {
      return InvalidArgumentError("buffer index out of range");
    }
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.m.fd = import_fds_[index].get();
    buf.length = buffer_length_;
  } else {
    buf.memory = V4L2_MEMORY_MMAP;
  }
  if (device_->Ioctl(VIDIOC_QBUF, &buf) < 0) {
    return ErrnoStatus("VIDIOC_QBUF", errno);
  }
  ++queued_;
  return Status::Ok();
}

Status V4l2DmaBufSource::Queue(uint32_t index) {
  if (mode_ == kIoNone || index >= buffer_count_) {
    return InvalidArgumentError("Queue() of an unknown buffer");
  }
  return QueueIndex(index);
}

Status V4l2DmaBufSource::Start() {
  if (mode_ == kIoNone) {
    return FailedPreconditionError("Start() before Prepare()");
  }
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (device_->Ioctl(VIDIOC_STREAMON, &type) < 0) {
    return ErrnoStatus("VIDIOC_STREAMON", errno);
  }
  streaming_ = true;
  return Status::Ok();
}

void V4l2DmaBufSource::Stop() {
  if (streaming_ && device_->fd() >= 0) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    device_->Ioctl(VIDIOC_STREAMOFF, &type);
  }
  streaming_ = false;
  queued_ = 0;
}

Status V4l2DmaBufSource::Dequeue(int timeout_ms, DequeuedBuffer* out) {
  const uint64_t deadline_ns =
      MonotonicNs() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
  while (true) {
    const uint64_t now_ns = MonotonicNs();
    const int remaining_ms =
        now_ns >= deadline_ns
            ? 0
            : static_cast<int>((deadline_ns - now_ns + 999999ull) / 1000000ull);
    struct pollfd pfds[2];
    pfds[0].fd = device_->fd();
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = wake_fd_;  // -1 is ignored by poll().
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;
    const int pr = poll(pfds, 2, remaining_ms);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      return ErrnoStatus("poll V4L2", errno);
    }
    if (pfds[1].revents & POLLIN) {
      uint64_t counter = 0;
      if (read(wake_fd_, &counter, sizeof(counter)) < 0) {
        // EAGAIN: someone else drained it; nothing to do.
      }
      if (!(pfds[0].revents & POLLIN)) {
        return TimeoutError("woken up");
      }
    }
    if (pr == 0) {
      return TimeoutError("no frame within timeout");
    }
    if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      return UnavailableError(config_.device + " vanished (hang-up)");
    }
    if (!(pfds[0].revents & POLLIN)) {
      continue;
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = mode_ == kIoExpbuf ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF;
    if (device_->Ioctl(VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EAGAIN) {
        continue;
      }
      if (errno == ENODEV || errno == EIO || errno == EPIPE) {
        return UnavailableError("VIDIOC_DQBUF: " +
                                std::string(strerror(errno)));
      }
      return ErrnoStatus("VIDIOC_DQBUF", errno);
    }
    if (queued_ > 0) {
      --queued_;
    }
    const bool corrupt = (buf.flags & V4L2_BUF_FLAG_ERROR) != 0;
    const bool short_frame = format_.fourcc == kFourccMjpeg
                                 ? buf.bytesused == 0
                                 : buf.bytesused < format_.size_image;
    if (corrupt || short_frame) {
      ++bad_frames_;
      MW_LOG(Debug) << "dropping bad V4L2 frame (seq " << buf.sequence << ")";
      const Status requeued = QueueIndex(buf.index);
      if (!requeued.ok()) {
        return requeued;
      }
      continue;
    }
    out->index = buf.index;
    out->bytes_used = buf.bytesused;
    out->sequence = buf.sequence;
    if ((buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
        V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
      out->timestamp_ns =
          static_cast<uint64_t>(buf.timestamp.tv_sec) * 1000000000ull +
          static_cast<uint64_t>(buf.timestamp.tv_usec) * 1000ull;
    } else {
      out->timestamp_ns = MonotonicNs();
    }
    return Status::Ok();
  }
}

std::string V4l2DmaBufSource::Describe() const {
  std::ostringstream os;
  os << "v4l2-dmabuf:" << device_->Describe() << " " << config_.width << "x"
     << config_.height << "@" << config_.fps << " "
     << FourccToString(config_.pixel_format) << " io=" << io_mode();
  return os.str();
}

Status CreateDmaBufSource(const VideoStreamConfig& config,
                          std::unique_ptr<DmaBufSource>* out) {
  std::unique_ptr<V4l2Device> device;
  if (config.source == "v4l2") {
    device = CreateRealV4l2Device();
  } else if (config.source == "test_pattern") {
    SyntheticV4l2Device* synthetic = new SyntheticV4l2Device(config.fps);
    SyntheticFaults faults;
    faults.vanish_after_frames = config.test_unplug_after_frames;
    synthetic->set_faults(faults);
    device.reset(synthetic);
  } else {
    return InvalidArgumentError("unknown video source '" + config.source + "'");
  }
  out->reset(new V4l2DmaBufSource(config, std::move(device)));
  return Status::Ok();
}

}  // namespace mw
