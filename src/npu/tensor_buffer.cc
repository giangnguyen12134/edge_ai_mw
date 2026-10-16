// Copyright 2026 Genio Middleware Authors.

#include "mw/npu/tensor_buffer.h"

#include "mw/common/frame_types.h"

namespace mw {

Status TensorBuffer::Allocate(BufferAllocator* allocator, size_t size) {
  if (size == 0) {
    return InvalidArgumentError("empty tensor buffer requested");
  }
  const size_t aligned = AlignUp(static_cast<uint32_t>(size), 4096);
  MW_RETURN_IF_ERROR(allocator->Allocate(aligned, &fd_));
  MW_RETURN_IF_ERROR(mapping_.Map(fd_.get(), aligned, true));
  size_ = size;
  return Status::Ok();
}

}  // namespace mw
