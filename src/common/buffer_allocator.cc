// Copyright 2026 Genio Middleware Authors.

#include "mw/common/buffer_allocator.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace mw {
namespace {

// Mirrors <linux/dma-heap.h>. Defined locally so the project builds against
// older kernel headers in a Yocto sysroot. The ABI is stable.
struct DmaHeapAllocationData {
  uint64_t len;
  uint32_t fd;
  uint32_t fd_flags;
  uint64_t heap_flags;
};
const unsigned long kDmaHeapIoctlAlloc =
    _IOWR('H', 0x0, struct DmaHeapAllocationData);

class DmaHeapAllocator : public BufferAllocator {
 public:
  explicit DmaHeapAllocator(const std::string& path) : path_(path) {}

  Status Open() {
    heap_fd_.Reset(open(path_.c_str(), O_RDWR | O_CLOEXEC));
    if (!heap_fd_.valid()) {
      return ErrnoStatus("open " + path_, errno);
    }
    return Status::Ok();
  }

  Status Allocate(size_t size, ScopedFd* out_fd) override {
    DmaHeapAllocationData data;
    data.len = size;
    data.fd = 0;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    data.heap_flags = 0;
    if (ioctl(heap_fd_.get(), kDmaHeapIoctlAlloc, &data) < 0) {
      return ErrnoStatus("DMA_HEAP_IOCTL_ALLOC " + path_, errno);
    }
    out_fd->Reset(static_cast<int>(data.fd));
    return Status::Ok();
  }

  bool IsDmaBuf() const override { return true; }
  std::string Describe() const override { return "dma_heap:" + path_; }

 private:
  std::string path_;
  ScopedFd heap_fd_;
};

class MemfdAllocator : public BufferAllocator {
 public:
  Status Allocate(size_t size, ScopedFd* out_fd) override {
    const int fd = static_cast<int>(
        syscall(SYS_memfd_create, "mw-buffer", 1u /*CLOEXEC*/));
    if (fd < 0) {
      return ErrnoStatus("memfd_create", errno);
    }
    ScopedFd owned(fd);
    if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
      return ErrnoStatus("ftruncate", errno);
    }
    *out_fd = std::move(owned);
    return Status::Ok();
  }

  bool IsDmaBuf() const override { return false; }
  std::string Describe() const override { return "memfd"; }
};

}  // namespace

Status CreateBufferAllocator(const std::string& spec,
                             std::unique_ptr<BufferAllocator>* out) {
  if (spec == "memfd") {
    out->reset(new MemfdAllocator());
    return Status::Ok();
  }
  const std::string prefix = "dma_heap:";
  if (spec.compare(0, prefix.size(), prefix) == 0) {
    std::string path = spec.substr(prefix.size());
    if (path.empty()) {
      return InvalidArgumentError("empty dma_heap name in '" + spec + "'");
    }
    if (path[0] != '/') {
      path = "/dev/dma_heap/" + path;
    }
    std::unique_ptr<DmaHeapAllocator> allocator(new DmaHeapAllocator(path));
    MW_RETURN_IF_ERROR(allocator->Open());
    *out = std::move(allocator);
    return Status::Ok();
  }
  return InvalidArgumentError("unknown allocator spec '" + spec + "'");
}

}  // namespace mw
