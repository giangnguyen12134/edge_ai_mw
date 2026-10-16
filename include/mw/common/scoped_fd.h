// Copyright 2026 Genio Middleware Authors.
//
// Move-only RAII wrapper for a POSIX file descriptor.

#ifndef MW_COMMON_SCOPED_FD_H_
#define MW_COMMON_SCOPED_FD_H_

#include "mw/common/macros.h"

namespace mw {

class ScopedFd {
 public:
  ScopedFd() : fd_(-1) {}
  explicit ScopedFd(int fd) : fd_(fd) {}
  ScopedFd(ScopedFd&& other) : fd_(other.Release()) {}
  ScopedFd& operator=(ScopedFd&& other) {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }
  ~ScopedFd() { Reset(); }

  MW_DISALLOW_COPY_AND_ASSIGN(ScopedFd);

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }

  // Gives up ownership without closing.
  int Release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  // Closes the current descriptor (if any) and takes ownership of |fd|.
  void Reset(int fd = -1);

  // Returns a new close-on-exec duplicate, or an invalid ScopedFd on failure.
  ScopedFd Duplicate() const;

  // Duplicates |fd| with close-on-exec set. Invalid result on failure.
  static ScopedFd DuplicateFrom(int fd);

 private:
  int fd_;
};

}  // namespace mw

#endif  // MW_COMMON_SCOPED_FD_H_
