// Copyright 2026 Genio Middleware Authors.
//
// DmaBufSource on top of a V4l2Device (real or synthetic).
//
// About zero copy with UVC: uvcvideo copies USB payloads into the buffer with
// the CPU inside the kernel - that one copy is unavoidable for a USB camera.
// What this class removes is every copy after it: the buffer the driver
// fills is the dma-buf that all clients map.

#ifndef MW_MEDIA_V4L2_DMABUF_SOURCE_H_
#define MW_MEDIA_V4L2_DMABUF_SOURCE_H_

#include <stdint.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "mw/common/macros.h"
#include "mw/media/dmabuf_source.h"
#include "mw/media/v4l2_device.h"

namespace mw {

class V4l2DmaBufSource : public DmaBufSource {
 public:
  V4l2DmaBufSource(const VideoStreamConfig& config,
                   std::unique_ptr<V4l2Device> device);
  ~V4l2DmaBufSource() override;

  MW_DISALLOW_COPY_AND_ASSIGN(V4l2DmaBufSource);

  Status Open(CameraFormat* format) override;
  Status Prepare(uint32_t count, BufferAllocator* allocator,
                 std::vector<ScopedFd>* fds, uint32_t* buffer_length) override;
  Status Queue(uint32_t index) override;
  Status Start() override;
  void Stop() override;
  Status Dequeue(int timeout_ms, DequeuedBuffer* buffer) override;
  void SetWakeFd(int fd) override { wake_fd_ = fd; }
  uint32_t queued_count() const override { return queued_.load(); }
  std::string io_mode() const override;
  uint64_t bad_frames() const override { return bad_frames_.load(); }
  std::string Describe() const override;

 private:
  enum IoMode { kIoNone = 0, kIoExpbuf = 1, kIoDmabuf = 2 };

  Status PrepareExpbuf(uint32_t count, std::vector<ScopedFd>* fds,
                       uint32_t* buffer_length);
  Status PrepareDmabuf(uint32_t count, BufferAllocator* allocator,
                       std::vector<ScopedFd>* fds, uint32_t* buffer_length);
  void ReleaseBuffers();
  Status QueueIndex(uint32_t index);

  VideoStreamConfig config_;
  std::unique_ptr<V4l2Device> device_;
  CameraFormat format_;
  IoMode mode_;
  std::vector<ScopedFd> import_fds_;  // dmabuf mode: our copies for QBUF.
  uint32_t buffer_length_;
  uint32_t buffer_count_;
  std::atomic<uint32_t> queued_;
  std::atomic<uint64_t> bad_frames_;
  bool streaming_;
  int wake_fd_;
};

}  // namespace mw

#endif  // MW_MEDIA_V4L2_DMABUF_SOURCE_H_
