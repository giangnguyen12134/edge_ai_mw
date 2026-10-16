// Copyright 2026 Genio Middleware Authors.
//
// A model input tensor living in a shareable buffer. The image scaler fills
// it (hardware via the fd, or the CPU via the mapping) and the NPU runtime
// imports the very same fd, so the pre-processed tensor is not copied either.

#ifndef MW_NPU_TENSOR_BUFFER_H_
#define MW_NPU_TENSOR_BUFFER_H_

#include <stddef.h>
#include <stdint.h>

#include "mw/common/buffer_allocator.h"
#include "mw/common/dma_buf_mapping.h"
#include "mw/common/macros.h"
#include "mw/common/scoped_fd.h"
#include "mw/common/status.h"

namespace mw {

class TensorBuffer {
 public:
  TensorBuffer() : size_(0) {}

  MW_DISALLOW_COPY_AND_ASSIGN(TensorBuffer);

  Status Allocate(BufferAllocator* allocator, size_t size);

  int fd() const { return fd_.get(); }
  uint8_t* data() const { return mapping_.data(); }
  size_t size() const { return size_; }

  void BeginCpuWrite() { mapping_.BeginCpuAccess(true); }
  void EndCpuWrite() { mapping_.EndCpuAccess(true); }

 private:
  ScopedFd fd_;
  DmaBufMapping mapping_;
  size_t size_;
};

}  // namespace mw

#endif  // MW_NPU_TENSOR_BUFFER_H_
