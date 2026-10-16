// Copyright 2026 VMX Middleware Team.
//
// Camera capture straight into dma-bufs, in the camera's native pixel format.
//
// Life cycle (one "session" per camera open):
//   Open(&format)            negotiate size / pixel format with the driver
//   Prepare(...)             create the buffers, hand their dma-buf fds out
//   Queue(i) ...             give buffers to the driver
//   Start()                  VIDIOC_STREAMON
//   { Dequeue(); Queue(i) }* a buffer is the driver's from Queue() until it
//                            is returned by Dequeue()
//   Stop()                   VIDIOC_STREAMOFF (buffers return to the caller)
// After any failure call Stop() and begin again at Open(); the buffers of the
// previous session must not be used any more.

#ifndef MW_MEDIA_DMABUF_SOURCE_H_
#define MW_MEDIA_DMABUF_SOURCE_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "mw/common/buffer_allocator.h"
#include "mw/common/scoped_fd.h"
#include "mw/common/status.h"
#include "mw/media/media_config.h"

namespace mw {

// What the driver agreed to deliver (the result of VIDIOC_S_FMT).
struct CameraFormat {
  CameraFormat()
      : width(0), height(0), fourcc(0), bytes_per_line(0), size_image(0) {}
  uint32_t width;
  uint32_t height;
  uint32_t fourcc;
  uint32_t bytes_per_line;  // 0 for compressed formats.
  uint32_t size_image;
};

struct DequeuedBuffer {
  DequeuedBuffer() : index(0), bytes_used(0), sequence(0), timestamp_ns(0) {}
  uint32_t index;
  uint32_t bytes_used;
  uint32_t sequence;
  uint64_t timestamp_ns;  // CLOCK_MONOTONIC.
};

class DmaBufSource {
 public:
  virtual ~DmaBufSource() {}

  virtual Status Open(CameraFormat* format) = 0;

  // Creates up to |count| buffers. On success |*fds| holds one dma-buf fd per
  // buffer (index == position) for the caller to keep, and
  // |*buffer_length| is the size of each. Buffers are not queued yet.
  //
  // Which kernel mechanism is used follows video.capture_io:
  //   expbuf  VIDIOC_REQBUFS(MMAP) + VIDIOC_EXPBUF: the driver allocates.
  //   dmabuf  |allocator| (dma-heap) allocates; VIDIOC_REQBUFS(DMABUF) and
  //           VIDIOC_QBUF with the fd import them.
  //   auto    dmabuf when |allocator| is a real dma-heap, else expbuf; falls
  //           back to the other one if the driver refuses.
  virtual Status Prepare(uint32_t count, BufferAllocator* allocator,
                         std::vector<ScopedFd>* fds,
                         uint32_t* buffer_length) = 0;

  virtual Status Queue(uint32_t index) = 0;
  virtual Status Start() = 0;
  virtual void Stop() = 0;

  // Waits up to |timeout_ms| for a filled buffer.
  //   kOk           |*buffer| returned; Queue() it again when done.
  //   kTimeout      nothing yet, or WakeUp() was called.
  //   kUnavailable  the device went away.
  virtual Status Dequeue(int timeout_ms, DequeuedBuffer* buffer) = 0;

  // Descriptor (an eventfd owned by the caller) that interrupts Dequeue().
  // Must be set before Start() and stay valid while the source exists.
  virtual void SetWakeFd(int fd) = 0;

  // Buffers currently owned by the driver.
  virtual uint32_t queued_count() const = 0;

  // "expbuf" or "dmabuf" once Prepare() succeeded.
  virtual std::string io_mode() const = 0;

  // Frames discarded because the driver flagged them corrupt or short.
  virtual uint64_t bad_frames() const = 0;

  virtual std::string Describe() const = 0;
};

// Builds the source for |config.source|: "v4l2" talks to config.device,
// "test_pattern" uses the synthetic driver.
Status CreateDmaBufSource(const VideoStreamConfig& config,
                          std::unique_ptr<DmaBufSource>* out);

}  // namespace mw

#endif  // MW_MEDIA_DMABUF_SOURCE_H_
