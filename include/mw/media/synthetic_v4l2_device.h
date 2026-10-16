// Copyright 2026 Genio Middleware Authors.
//
// A software "capture driver" implementing the V4L2 ioctls MediaManager uses
// (QUERYCAP, S_FMT, S_PARM, REQBUFS, QUERYBUF, EXPBUF, QBUF, DQBUF, STREAMON,
// STREAMOFF) for both V4L2_MEMORY_MMAP and V4L2_MEMORY_DMABUF. Frames come
// from a timerfd at the requested frame rate and are written straight into
// the queued buffers, exactly like a real driver. Used by the unit tests, the
// end-to-end test and `video.source = "test_pattern"`.
//
// Frame content (so tests can verify it): byte i of frame n is
// (i + n) & 0xFF. MJPEG frames are a variable-length blob framed by the JPEG
// SOI (FF D8) and EOI (FF D9) markers.

#ifndef MW_MEDIA_SYNTHETIC_V4L2_DEVICE_H_
#define MW_MEDIA_SYNTHETIC_V4L2_DEVICE_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "mw/common/macros.h"
#include "mw/common/scoped_fd.h"
#include "mw/media/v4l2_device.h"

namespace mw {

// Failure injection for tests. All off by default.
struct SyntheticFaults {
  SyntheticFaults()
      : fail_mmap_reqbufs(false),
        fail_expbuf(false),
        fail_dmabuf_reqbufs(false),
        fail_dmabuf_qbuf(false),
        vanish_after_frames(0),
        stall_after_frames(0) {}
  bool fail_mmap_reqbufs;        // REQBUFS(MMAP) -> EINVAL
  bool fail_expbuf;              // EXPBUF -> ENOTTY
  bool fail_dmabuf_reqbufs;      // REQBUFS(DMABUF) -> EINVAL
  bool fail_dmabuf_qbuf;         // QBUF with a dma-buf fd -> EINVAL
  uint32_t vanish_after_frames;  // then DQBUF -> ENODEV (USB unplug)
  uint32_t stall_after_frames;   // then no more frames
};

class SyntheticV4l2Device : public V4l2Device {
 public:
  explicit SyntheticV4l2Device(uint32_t default_fps);
  ~SyntheticV4l2Device() override;

  MW_DISALLOW_COPY_AND_ASSIGN(SyntheticV4l2Device);

  void set_faults(const SyntheticFaults& faults) { faults_ = faults; }

  Status Open(const std::string& path) override;
  void Close() override;
  int Ioctl(unsigned long request, void* arg) override;
  int fd() const override { return timer_.get(); }
  std::string Describe() const override { return "synthetic"; }

  // Fills |size| bytes at |data| with the pattern of frame |sequence|.
  static void FillPattern(uint32_t fourcc, uint32_t sequence, uint8_t* data,
                          size_t size, size_t* bytes_used);

 private:
  struct Buffer {
    Buffer() : mmap_base(nullptr), queued(false), map_base(nullptr) {}
    ScopedFd memfd;      // MMAP mode: the buffer itself.
    uint8_t* mmap_base;  // MMAP mode: our CPU mapping.
    bool queued;
    ScopedFd imported;  // DMABUF mode: the fd passed to QBUF.
    uint8_t* map_base;  // DMABUF mode: mapping of |imported|.
  };

  int DoReqbufs(void* arg);
  int DoQbuf(void* arg);
  int DoDqbuf(void* arg);
  int DoExpbuf(void* arg);
  void FreeBuffers();
  void SetTimer(bool on);

  SyntheticFaults faults_;
  ScopedFd timer_;
  uint32_t default_fps_;
  uint32_t fps_;
  uint32_t width_;
  uint32_t height_;
  uint32_t fourcc_;
  uint32_t bytes_per_line_;
  uint32_t size_image_;
  uint32_t buffer_length_;
  uint32_t memory_;  // V4L2_MEMORY_* of the current allocation, 0 if none.
  bool streaming_;
  uint32_t sequence_;
  std::vector<Buffer> buffers_;
  std::vector<uint32_t> queue_;  // Indices in QBUF order.
};

}  // namespace mw

#endif  // MW_MEDIA_SYNTHETIC_V4L2_DEVICE_H_
