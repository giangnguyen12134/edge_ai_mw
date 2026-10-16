// Copyright 2026 Genio Middleware Authors.

#include "mw/common/dma_buf_mapping.h"

#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

namespace mw {
namespace {

// Mirrors <linux/dma-buf.h>.
struct DmaBufSyncArg {
  uint64_t flags;
};
const uint64_t kSyncRead = 1ull << 0;
const uint64_t kSyncWrite = 2ull << 0;
const uint64_t kSyncStart = 0ull << 2;
const uint64_t kSyncEnd = 1ull << 2;
const unsigned long kDmaBufIoctlSync = _IOW('b', 0, struct DmaBufSyncArg);

}  // namespace

DmaBufMapping::DmaBufMapping() : fd_(-1), data_(nullptr), size_(0) {}

DmaBufMapping::~DmaBufMapping() { Unmap(); }

Status DmaBufMapping::Map(int fd, size_t size, bool writable) {
  Unmap();
  const int prot = PROT_READ | (writable ? PROT_WRITE : 0);
  void* p = mmap(nullptr, size, prot, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    return ErrnoStatus("mmap dma-buf", errno);
  }
  fd_ = fd;
  data_ = static_cast<uint8_t*>(p);
  size_ = size;
  return Status::Ok();
}

void DmaBufMapping::Unmap() {
  if (data_ != nullptr) {
    munmap(data_, size_);
  }
  data_ = nullptr;
  size_ = 0;
  fd_ = -1;
}

void DmaBufMapping::Sync(bool start, bool write) {
  if (fd_ < 0) {
    return;
  }
  DmaBufSyncArg arg;
  arg.flags = (start ? kSyncStart : kSyncEnd) |
              (write ? (kSyncRead | kSyncWrite) : kSyncRead);
  // ENOTTY/EINVAL means "not a dma-buf" (memfd fallback): nothing to do.
  ioctl(fd_, kDmaBufIoctlSync, &arg);
}

void DmaBufMapping::BeginCpuAccess(bool write) { Sync(true, write); }
void DmaBufMapping::EndCpuAccess(bool write) { Sync(false, write); }

}  // namespace mw
