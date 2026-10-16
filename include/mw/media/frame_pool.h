// Copyright 2026 Genio Middleware Authors.
//
// Fixed pool of dma-bufs shared by all consumers. The pool is created
// once; its file descriptors are sent to each subscriber once. Afterwards a
// frame is identified purely by its slot index (zero copy, no per-frame fds).
//
// Every slot has a reference count: one reference for the producer while it
// fills the slot plus one per subscriber the frame was delivered to. A slot
// returns to the free list when the count reaches zero.

#ifndef MW_MEDIA_FRAME_POOL_H_
#define MW_MEDIA_FRAME_POOL_H_

#include <stdint.h>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "mw/common/buffer_allocator.h"
#include "mw/common/dma_buf_mapping.h"
#include "mw/common/frame_types.h"
#include "mw/common/macros.h"
#include "mw/common/scoped_fd.h"
#include "mw/common/status.h"

namespace mw {

class FramePool {
 public:
  FramePool();
  ~FramePool();

  MW_DISALLOW_COPY_AND_ASSIGN(FramePool);

  // Allocates |slot_count| buffers of |format.buffer_size| bytes and maps
  // each one for CPU writing (needed by the ingest stage).
  Status Init(BufferAllocator* allocator, uint32_t slot_count,
              const FrameFormat& format);

  // Adopts existing dma-bufs (exported by the camera driver or allocated for
  // it). Slot i is |fds[i]|. The buffers are not mapped: the media service
  // never touches pixels in native-format mode, clients map them themselves.
  Status InitFromFds(std::vector<ScopedFd>&& fds, const FrameFormat& format);

  // Called, without any pool lock held, whenever a slot returns to the free
  // list. Set it before any slot is handed out; it must be fast and must not
  // call back into the pool.
  void SetFreeNotifier(const std::function<void()>& notifier) {
    free_notifier_ = notifier;
  }

  const FrameFormat& format() const { return format_; }
  uint32_t slot_count() const { return static_cast<uint32_t>(slots_.size()); }

  // dma-buf descriptor of |slot|. Owned by the pool.
  int fd(uint32_t slot) const { return slots_[slot]->fd.get(); }

  // CPU pointer to |slot| (valid for the lifetime of the pool).
  // nullptr for pools created with InitFromFds().
  uint8_t* data(uint32_t slot) const { return slots_[slot]->mapping.data(); }

  void BeginCpuWrite(uint32_t slot) {
    slots_[slot]->mapping.BeginCpuAccess(true);
  }
  void EndCpuWrite(uint32_t slot) { slots_[slot]->mapping.EndCpuAccess(true); }

  // Takes a free slot with reference count 1, or returns -1 when exhausted.
  int Acquire();

  // Adds one reference. |slot| must currently be referenced.
  void AddRef(uint32_t slot);

  // Drops one reference, freeing the slot at zero.
  void Unref(uint32_t slot);

  uint32_t free_count() const;

 private:
  struct Slot {
    Slot() : refs(0) {}
    ScopedFd fd;
    DmaBufMapping mapping;
    uint32_t refs;
  };

  FrameFormat format_;
  std::vector<std::unique_ptr<Slot>> slots_;
  mutable std::mutex mutex_;
  std::vector<uint32_t> free_list_;
  std::function<void()> free_notifier_;
};

}  // namespace mw

#endif  // MW_MEDIA_FRAME_POOL_H_
