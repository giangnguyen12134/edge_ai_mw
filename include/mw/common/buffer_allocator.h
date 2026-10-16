// Copyright 2026 Genio Middleware Authors.
//
// Allocates shareable buffers (dma-buf file descriptors). Production uses a
// Linux dma-heap (physically contiguous CMA or system heap) so the NPU, the
// scaler and the encoder can import the buffer. A memfd fallback exists for
// development machines and CI; memfd buffers can NOT be imported by hardware.

#ifndef MW_COMMON_BUFFER_ALLOCATOR_H_
#define MW_COMMON_BUFFER_ALLOCATOR_H_

#include <stddef.h>

#include <memory>
#include <string>

#include "mw/common/scoped_fd.h"
#include "mw/common/status.h"

namespace mw {

class BufferAllocator {
 public:
  virtual ~BufferAllocator() {}

  // Allocates |size| bytes. The returned fd supports mmap(MAP_SHARED).
  virtual Status Allocate(size_t size, ScopedFd* out_fd) = 0;

  // True if buffers can be imported by hardware (real dma-buf).
  virtual bool IsDmaBuf() const = 0;

  virtual std::string Describe() const = 0;
};

// |spec| is one of:
//   "dma_heap:/dev/dma_heap/system"   explicit heap device
//   "dma_heap:system"                 shorthand for /dev/dma_heap/system
//   "dma_heap:linux,cma"              shorthand for /dev/dma_heap/linux,cma
//   "memfd"                           CPU-only fallback
Status CreateBufferAllocator(const std::string& spec,
                             std::unique_ptr<BufferAllocator>* out);

}  // namespace mw

#endif  // MW_COMMON_BUFFER_ALLOCATOR_H_
