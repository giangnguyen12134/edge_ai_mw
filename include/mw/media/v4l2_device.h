// Copyright 2026 Genio Middleware Authors.
//
// Thin seam over a V4L2 device node: open, ioctl, and a pollable descriptor.
// The production implementation talks to the kernel; SyntheticV4l2Device
// (synthetic_v4l2_device.h) emulates a capture driver so the capture code can
// be tested on machines without a camera, dma-heap or vivid.

#ifndef MW_MEDIA_V4L2_DEVICE_H_
#define MW_MEDIA_V4L2_DEVICE_H_

#include <memory>
#include <string>

#include "mw/common/status.h"

namespace mw {

class V4l2Device {
 public:
  virtual ~V4l2Device() {}

  virtual Status Open(const std::string& path) = 0;
  virtual void Close() = 0;

  // Same contract as ioctl(2): returns -1 and sets errno on failure.
  // EINTR is retried internally.
  virtual int Ioctl(unsigned long request, void* arg) = 0;

  // Descriptor to poll() for POLLIN ("a frame can be dequeued"). -1 if the
  // device is not open.
  virtual int fd() const = 0;

  virtual std::string Describe() const = 0;
};

// Opens /dev/videoN with O_RDWR | O_NONBLOCK | O_CLOEXEC.
std::unique_ptr<V4l2Device> CreateRealV4l2Device();

}  // namespace mw

#endif  // MW_MEDIA_V4L2_DEVICE_H_
