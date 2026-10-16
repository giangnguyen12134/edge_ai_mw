// Copyright 2026 Genio Middleware Authors.
//
// CPU mapping of a dma-buf with the cache-coherency bracketing required by
// DMA_BUF_IOCTL_SYNC. Hardware (NPU, scaler) access needs no bracketing.

#ifndef MW_COMMON_DMA_BUF_MAPPING_H_
#define MW_COMMON_DMA_BUF_MAPPING_H_

#include <stddef.h>
#include <stdint.h>

#include "mw/common/macros.h"
#include "mw/common/status.h"

namespace mw {

class DmaBufMapping {
 public:
  DmaBufMapping();
  ~DmaBufMapping();

  MW_DISALLOW_COPY_AND_ASSIGN(DmaBufMapping);

  // Maps |size| bytes of |fd|. The caller keeps |fd| open for as long as this
  // object is used (it is needed for the sync ioctl) and must not close it
  // before Unmap().
  Status Map(int fd, size_t size, bool writable);
  void Unmap();

  bool mapped() const { return data_ != nullptr; }
  uint8_t* data() const { return data_; }
  size_t size() const { return size_; }

  // Bracket CPU access. Harmless no-ops for memfd buffers.
  void BeginCpuAccess(bool write);
  void EndCpuAccess(bool write);

 private:
  void Sync(bool start, bool write);

  int fd_;
  uint8_t* data_;
  size_t size_;
};

}  // namespace mw

#endif  // MW_COMMON_DMA_BUF_MAPPING_H_
