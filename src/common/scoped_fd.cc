// Copyright 2026 Genio Middleware Authors.

#include "mw/common/scoped_fd.h"

#include <fcntl.h>
#include <unistd.h>

namespace mw {

void ScopedFd::Reset(int fd) {
  if (fd_ >= 0) {
    close(fd_);
  }
  fd_ = fd;
}

ScopedFd ScopedFd::Duplicate() const { return DuplicateFrom(fd_); }

ScopedFd ScopedFd::DuplicateFrom(int fd) {
  if (fd < 0) {
    return ScopedFd();
  }
  return ScopedFd(fcntl(fd, F_DUPFD_CLOEXEC, 3));
}

}  // namespace mw
