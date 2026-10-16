// Copyright 2026 Genio Middleware Authors.
//
// V4L2 capture (UVC cameras and other single-plane streaming devices) using
// memory-mapped driver buffers.
//
// NOTE on zero copy: uvcvideo fills vmalloc-backed vb2 buffers with the CPU
// (USB packets are copied by the driver), and such buffers are not physically
// contiguous, so hardware blocks cannot consume them. The ingest stage
// therefore converts/copies once into the dma-heap pool; from the pool onward
// the pipeline is zero copy.

#ifndef MW_MEDIA_V4L2_FRAME_SOURCE_H_
#define MW_MEDIA_V4L2_FRAME_SOURCE_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "mw/common/macros.h"
#include "mw/common/scoped_fd.h"
#include "mw/media/frame_source.h"

namespace mw {

class V4l2FrameSource : public FrameSource {
 public:
  explicit V4l2FrameSource(const VideoStreamConfig& config);
  ~V4l2FrameSource() override;

  MW_DISALLOW_COPY_AND_ASSIGN(V4l2FrameSource);

  Status Open() override;
  Status Start() override;
  void Stop() override;
  Status Capture(int timeout_ms, CapturedFrame* frame) override;
  void Release(const CapturedFrame& frame) override;
  std::string Describe() const override;

 private:
  struct MappedBuffer {
    MappedBuffer() : start(nullptr), length(0) {}
    void* start;
    size_t length;
  };

  void CloseDevice();

  VideoStreamConfig config_;
  ScopedFd fd_;
  std::vector<MappedBuffer> buffers_;
  uint32_t stride_;
  uint32_t image_size_;
  bool streaming_;
};

}  // namespace mw

#endif  // MW_MEDIA_V4L2_FRAME_SOURCE_H_
