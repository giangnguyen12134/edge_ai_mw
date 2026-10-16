// Copyright 2026 VMX Middleware Team.

#include "mw/media/frame_pool.h"

#include "mw/common/logging.h"

namespace mw {

FramePool::FramePool() {}

FramePool::~FramePool() {}

Status FramePool::Init(BufferAllocator* allocator, uint32_t slot_count,
                       const FrameFormat& format) {
  if (slot_count == 0 || format.buffer_size == 0) {
    return InvalidArgumentError("empty frame pool requested");
  }
  format_ = format;
  slots_.clear();
  free_list_.clear();
  for (uint32_t i = 0; i < slot_count; ++i) {
    std::unique_ptr<Slot> slot(new Slot());
    MW_RETURN_IF_ERROR(allocator->Allocate(format.buffer_size, &slot->fd));
    MW_RETURN_IF_ERROR(
        slot->mapping.Map(slot->fd.get(), format.buffer_size, true));
    slots_.push_back(std::move(slot));
  }
  // Hand out low slot numbers first.
  for (uint32_t i = slot_count; i > 0; --i) {
    free_list_.push_back(i - 1);
  }
  MW_LOG(Info) << "frame pool: " << slot_count << " x " << format.buffer_size
               << " bytes (" << format.width << "x" << format.height
               << " NV12) from " << allocator->Describe();
  return Status::Ok();
}

Status FramePool::InitFromFds(std::vector<ScopedFd>&& fds,
                              const FrameFormat& format) {
  if (fds.empty() || format.buffer_size == 0) {
    return InvalidArgumentError("empty frame pool requested");
  }
  format_ = format;
  slots_.clear();
  free_list_.clear();
  for (size_t i = 0; i < fds.size(); ++i) {
    if (!fds[i].valid()) {
      return InvalidArgumentError("invalid dma-buf descriptor for slot");
    }
    std::unique_ptr<Slot> slot(new Slot());
    slot->fd = std::move(fds[i]);
    slots_.push_back(std::move(slot));
  }
  for (uint32_t i = slot_count(); i > 0; --i) {
    free_list_.push_back(i - 1);
  }
  MW_LOG(Info) << "frame pool: " << slots_.size() << " x " << format.buffer_size
               << " bytes (" << format.width << "x" << format.height << " "
               << FourccToString(format.fourcc) << ", camera-owned dma-bufs)";
  return Status::Ok();
}

int FramePool::Acquire() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (free_list_.empty()) {
    return -1;
  }
  const uint32_t slot = free_list_.back();
  free_list_.pop_back();
  slots_[slot]->refs = 1;
  return static_cast<int>(slot);
}

void FramePool::AddRef(uint32_t slot) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (slot < slots_.size() && slots_[slot]->refs > 0) {
    ++slots_[slot]->refs;
  }
}

void FramePool::Unref(uint32_t slot) {
  bool freed = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= slots_.size() || slots_[slot]->refs == 0) {
      return;
    }
    if (--slots_[slot]->refs == 0) {
      free_list_.push_back(slot);
      freed = true;
    }
  }
  if (freed && free_notifier_) {
    free_notifier_();
  }
}

uint32_t FramePool::free_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<uint32_t>(free_list_.size());
}

}  // namespace mw
