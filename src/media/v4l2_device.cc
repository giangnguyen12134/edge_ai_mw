// Copyright 2026 Genio Middleware Authors.

#include "mw/media/v4l2_device.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "mw/common/scoped_fd.h"

namespace mw {
namespace {

class RealV4l2Device : public V4l2Device {
 public:
  RealV4l2Device() {}

  Status Open(const std::string& path) override {
    Close();
    fd_.Reset(open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
    if (!fd_.valid()) {
      return ErrnoStatus("open " + path, errno);
    }
    path_ = path;
    return Status::Ok();
  }

  void Close() override { fd_.Reset(); }

  int Ioctl(unsigned long request, void* arg) override {
    int r;
    do {
      r = ioctl(fd_.get(), request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
  }

  int fd() const override { return fd_.get(); }
  std::string Describe() const override { return path_; }

 private:
  ScopedFd fd_;
  std::string path_;
};

}  // namespace

std::unique_ptr<V4l2Device> CreateRealV4l2Device() {
  return std::unique_ptr<V4l2Device>(new RealV4l2Device());
}

}  // namespace mw
