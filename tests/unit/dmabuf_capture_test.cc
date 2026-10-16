// Copyright 2026 Genio Middleware Authors.
//
// Native-format capture: frame layout helpers, adopted dma-bufs in the pool
// and the V4L2 EXPBUF / DMABUF capture source (driven by the synthetic
// driver, because CI machines have neither a camera nor a dma-heap).

#include <stdint.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <memory>
#include <vector>

#include "mw/common/frame_types.h"
#include "mw/media/dmabuf_source.h"
#include "mw/media/frame_pool.h"
#include "mw/media/media_config.h"
#include "mw/media/synthetic_v4l2_device.h"
#include "mw/media/v4l2_dmabuf_source.h"
#include "tests/test_framework.h"

namespace mw {
namespace {

struct Rig {
  Rig(uint32_t fourcc, const std::string& io, const SyntheticFaults& faults)
      : synthetic(nullptr) {
    config.source = "test_pattern";
    config.width = 64;
    config.height = 48;
    config.fps = 120;
    config.pixel_format = fourcc;
    config.capture_io = io;
    std::unique_ptr<SyntheticV4l2Device> device(new SyntheticV4l2Device(120));
    device->set_faults(faults);
    synthetic = device.get();
    source.reset(new V4l2DmaBufSource(config, std::move(device)));
    CreateBufferAllocator("memfd", &allocator);
  }

  VideoStreamConfig config;
  SyntheticV4l2Device* synthetic;
  std::unique_ptr<V4l2DmaBufSource> source;
  std::unique_ptr<BufferAllocator> allocator;
};

// Reads byte |offset| of the dma-buf |fd| through a fresh mapping.
int ByteAt(int fd, uint32_t length, size_t offset) {
  void* p = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    return -1;
  }
  const int value = static_cast<const uint8_t*>(p)[offset];
  munmap(p, length);
  return value;
}

// Opens, prepares, queues everything and starts streaming.
Status Bring(Rig* rig, uint32_t count, std::vector<ScopedFd>* fds,
             uint32_t* length, CameraFormat* format) {
  MW_RETURN_IF_ERROR(rig->source->Open(format));
  MW_RETURN_IF_ERROR(
      rig->source->Prepare(count, rig->allocator.get(), fds, length));
  for (uint32_t i = 0; i < fds->size(); ++i) {
    MW_RETURN_IF_ERROR(rig->source->Queue(i));
  }
  return rig->source->Start();
}

}  // namespace

MW_TEST(FrameFormat, FromCameraKeepsDriverLayout) {
  const FrameFormat yuyv =
      FrameFormat::FromCamera(640, 480, kFourccYuyv, 1280, 614400);
  MW_EXPECT_EQ(yuyv.stride, 1280u);
  MW_EXPECT_EQ(yuyv.uv_offset, 0u);
  MW_EXPECT_EQ(yuyv.buffer_size, 614400u);  // Already page aligned.
  MW_EXPECT_OK(ValidateFrameFormat(yuyv));

  const FrameFormat nv12 =
      FrameFormat::FromCamera(640, 480, kFourccNv12, 640, 460800);
  MW_EXPECT_EQ(nv12.uv_offset, 640u * 480u);
  MW_EXPECT_OK(ValidateFrameFormat(nv12));

  const FrameFormat mjpg =
      FrameFormat::FromCamera(1280, 720, kFourccMjpeg, 0, 1843200);
  MW_EXPECT_OK(ValidateFrameFormat(mjpg));
}

MW_TEST(FrameFormat, ValidateRejectsInconsistentLayouts) {
  FrameFormat f = FrameFormat::FromCamera(640, 480, kFourccYuyv, 1280, 614400);
  f.buffer_size = 1000;  // Smaller than stride * height.
  MW_EXPECT_FALSE(ValidateFrameFormat(f).ok());
  f = FrameFormat::FromCamera(640, 480, kFourccYuyv, 100, 614400);
  MW_EXPECT_FALSE(ValidateFrameFormat(f).ok());  // Stride < 2 * width.
  f = FrameFormat::FromCamera(640, 480, kFourccNv12, 640, 460800);
  f.uv_offset = 0xFFFFFFF0u;  // Would overflow 32-bit arithmetic.
  MW_EXPECT_FALSE(ValidateFrameFormat(f).ok());
  f = FrameFormat();
  MW_EXPECT_FALSE(ValidateFrameFormat(f).ok());
  f = FrameFormat::FromCamera(640, 480, MakeFourcc('A', 'B', 'C', 'D'), 640,
                              460800);
  MW_EXPECT_FALSE(ValidateFrameFormat(f).ok());
}

MW_TEST(FramePool, AdoptsExternalBuffersAndNotifiesOnFree) {
  std::unique_ptr<BufferAllocator> allocator;
  MW_ASSERT_OK(CreateBufferAllocator("memfd", &allocator));
  std::vector<ScopedFd> fds(3);
  for (size_t i = 0; i < fds.size(); ++i) {
    MW_ASSERT_OK(allocator->Allocate(4096, &fds[i]));
  }
  const int raw0 = fds[0].get();
  FramePool pool;
  MW_ASSERT_OK(pool.InitFromFds(
      std::move(fds), FrameFormat::FromCamera(64, 32, kFourccMjpeg, 0, 4096)));
  MW_EXPECT_EQ(pool.slot_count(), 3u);
  MW_EXPECT_EQ(pool.fd(0), raw0);
  MW_EXPECT_TRUE(pool.data(0) == nullptr);  // Not CPU mapped by the service.

  int notified = 0;
  pool.SetFreeNotifier([&notified]() { ++notified; });
  const int slot = pool.Acquire();
  MW_ASSERT_TRUE(slot >= 0);
  pool.AddRef(static_cast<uint32_t>(slot));
  pool.Unref(static_cast<uint32_t>(slot));
  MW_EXPECT_EQ(notified, 0);  // Still referenced once.
  pool.Unref(static_cast<uint32_t>(slot));
  MW_EXPECT_EQ(notified, 1);
  MW_EXPECT_EQ(pool.free_count(), 3u);
}

MW_TEST(DmaBufCapture, ExpbufDeliversNativeNv12IntoExportedBuffers) {
  Rig rig(kFourccNv12, "expbuf", SyntheticFaults());
  std::vector<ScopedFd> fds;
  uint32_t length = 0;
  CameraFormat format;
  MW_ASSERT_OK(Bring(&rig, 4, &fds, &length, &format));
  MW_EXPECT_EQ(rig.source->io_mode(), std::string("expbuf"));
  MW_EXPECT_EQ(format.fourcc, kFourccNv12);
  MW_EXPECT_EQ(format.bytes_per_line, 64u);
  MW_EXPECT_EQ(fds.size(), 4u);
  MW_EXPECT_EQ(rig.source->queued_count(), 4u);

  DequeuedBuffer got;
  MW_ASSERT_OK(rig.source->Dequeue(1000, &got));
  MW_EXPECT_EQ(got.bytes_used, format.size_image);
  MW_EXPECT_EQ(rig.source->queued_count(), 3u);
  // The pattern is visible through the exported fd: no copy happened.
  MW_EXPECT_EQ(ByteAt(fds[got.index].get(), length, 10),
               static_cast<int>((10 + got.sequence) & 0xFF));
  MW_ASSERT_OK(rig.source->Queue(got.index));
  MW_EXPECT_EQ(rig.source->queued_count(), 4u);
}

MW_TEST(DmaBufCapture, DmabufImportFillsOurOwnBuffers) {
  Rig rig(kFourccYuyv, "dmabuf", SyntheticFaults());
  std::vector<ScopedFd> fds;
  uint32_t length = 0;
  CameraFormat format;
  MW_ASSERT_OK(Bring(&rig, 3, &fds, &length, &format));
  MW_EXPECT_EQ(rig.source->io_mode(), std::string("dmabuf"));
  MW_EXPECT_EQ(format.bytes_per_line, 128u);
  MW_EXPECT_EQ(length % 4096u, 0u);

  DequeuedBuffer got;
  MW_ASSERT_OK(rig.source->Dequeue(1000, &got));
  MW_EXPECT_EQ(got.bytes_used, 64u * 48u * 2u);
  // The buffer the allocator gave us (and we hold) contains the frame.
  MW_EXPECT_EQ(ByteAt(fds[got.index].get(), length, 7),
               static_cast<int>((7 + got.sequence) & 0xFF));
}

MW_TEST(DmaBufCapture, MjpegReportsVariableBytesUsed) {
  Rig rig(kFourccMjpeg, "expbuf", SyntheticFaults());
  std::vector<ScopedFd> fds;
  uint32_t length = 0;
  CameraFormat format;
  MW_ASSERT_OK(Bring(&rig, 4, &fds, &length, &format));
  DequeuedBuffer got;
  MW_ASSERT_OK(rig.source->Dequeue(1000, &got));
  MW_EXPECT_TRUE(got.bytes_used > 4);
  MW_EXPECT_TRUE(got.bytes_used < format.size_image);
  MW_EXPECT_EQ(ByteAt(fds[got.index].get(), length, 0), 0xFF);
  MW_EXPECT_EQ(ByteAt(fds[got.index].get(), length, 1), 0xD8);
  MW_EXPECT_EQ(ByteAt(fds[got.index].get(), length, got.bytes_used - 1), 0xD9);
}

MW_TEST(DmaBufCapture, AutoFallsBackToDmabufWhenExpbufFails) {
  SyntheticFaults faults;
  faults.fail_expbuf = true;
  Rig rig(kFourccNv12, "auto", faults);
  std::vector<ScopedFd> fds;
  uint32_t length = 0;
  CameraFormat format;
  MW_ASSERT_OK(Bring(&rig, 4, &fds, &length, &format));
  MW_EXPECT_EQ(rig.source->io_mode(), std::string("dmabuf"));
}

MW_TEST(DmaBufCapture, AutoFallsBackToExpbufWhenImportIsRejectedAtQbuf) {
  // A driver that refuses foreign dma-bufs only when they are queued. With a
  // real dma-heap allocator "auto" tries dmabuf first; here the order is
  // forced by asking for "auto" with the failing import and a working
  // export, which must still end up on expbuf.
  SyntheticFaults faults;
  faults.fail_dmabuf_qbuf = true;
  Rig rig(kFourccNv12, "auto", faults);
  std::vector<ScopedFd> fds;
  uint32_t length = 0;
  CameraFormat format;
  MW_ASSERT_OK(Bring(&rig, 4, &fds, &length, &format));
  MW_EXPECT_EQ(rig.source->io_mode(), std::string("expbuf"));
}

MW_TEST(DmaBufCapture, ExplicitModeDoesNotFallBack) {
  SyntheticFaults faults;
  faults.fail_expbuf = true;
  Rig rig(kFourccNv12, "expbuf", faults);
  std::vector<ScopedFd> fds;
  uint32_t length = 0;
  CameraFormat format;
  MW_ASSERT_OK(rig.source->Open(&format));
  MW_EXPECT_FALSE(
      rig.source->Prepare(4, rig.allocator.get(), &fds, &length).ok());
}

MW_TEST(DmaBufCapture, FailsWhenNeitherIoMethodWorks) {
  SyntheticFaults faults;
  faults.fail_expbuf = true;
  faults.fail_dmabuf_reqbufs = true;
  Rig rig(kFourccNv12, "auto", faults);
  std::vector<ScopedFd> fds;
  uint32_t length = 0;
  CameraFormat format;
  MW_ASSERT_OK(rig.source->Open(&format));
  MW_EXPECT_FALSE(
      rig.source->Prepare(4, rig.allocator.get(), &fds, &length).ok());
  MW_EXPECT_EQ(fds.size(), 0u);
}

MW_TEST(DmaBufCapture, OpenRejectsFormatTheCameraCannotDeliver) {
  Rig rig(MakeFourcc('A', 'B', 'C', 'D'), "auto", SyntheticFaults());
  CameraFormat format;
  MW_EXPECT_CODE(rig.source->Open(&format), Status::kFailedPrecondition);
}

MW_TEST(DmaBufCapture, UnplugIsReportedAsUnavailable) {
  SyntheticFaults faults;
  faults.vanish_after_frames = 2;
  Rig rig(kFourccNv12, "expbuf", faults);
  std::vector<ScopedFd> fds;
  uint32_t length = 0;
  CameraFormat format;
  MW_ASSERT_OK(Bring(&rig, 4, &fds, &length, &format));
  DequeuedBuffer got;
  MW_ASSERT_OK(rig.source->Dequeue(1000, &got));
  MW_ASSERT_OK(rig.source->Queue(got.index));
  MW_ASSERT_OK(rig.source->Dequeue(1000, &got));
  MW_ASSERT_OK(rig.source->Queue(got.index));
  MW_EXPECT_CODE(rig.source->Dequeue(1000, &got), Status::kUnavailable);
}

MW_TEST(DmaBufCapture, StalledCameraTimesOutWithBuffersQueued) {
  SyntheticFaults faults;
  faults.stall_after_frames = 1;
  Rig rig(kFourccNv12, "expbuf", faults);
  std::vector<ScopedFd> fds;
  uint32_t length = 0;
  CameraFormat format;
  MW_ASSERT_OK(Bring(&rig, 4, &fds, &length, &format));
  DequeuedBuffer got;
  MW_ASSERT_OK(rig.source->Dequeue(1000, &got));
  MW_EXPECT_CODE(rig.source->Dequeue(100, &got), Status::kTimeout);
  MW_EXPECT_TRUE(rig.source->queued_count() > 0);
}

MW_TEST(DmaBufCapture, WakeUpInterruptsTheWait) {
  Rig rig(kFourccNv12, "expbuf", SyntheticFaults());
  ScopedFd wake(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  MW_ASSERT_TRUE(wake.valid());
  rig.source->SetWakeFd(wake.get());
  CameraFormat format;
  std::vector<ScopedFd> fds;
  uint32_t length = 0;
  MW_ASSERT_OK(rig.source->Open(&format));
  MW_ASSERT_OK(rig.source->Prepare(4, rig.allocator.get(), &fds, &length));
  // Nothing queued: the synthetic camera cannot deliver, only the wake fd.
  MW_ASSERT_OK(rig.source->Start());
  const uint64_t one = 1;
  MW_ASSERT_TRUE(write(wake.get(), &one, sizeof(one)) == sizeof(one));
  DequeuedBuffer got;
  MW_EXPECT_CODE(rig.source->Dequeue(5000, &got), Status::kTimeout);
}

MW_TEST(DmaBufCapture, FactoryBuildsSyntheticSourceForTestPattern) {
  VideoStreamConfig config;
  config.source = "test_pattern";
  std::unique_ptr<DmaBufSource> source;
  MW_ASSERT_OK(CreateDmaBufSource(config, &source));
  config.source = "bogus";
  MW_EXPECT_FALSE(CreateDmaBufSource(config, &source).ok());
}

MW_TEST(MediaConfig, PoolFormatAndCaptureIoAreValidated) {
  MediaConfig config;
  MW_EXPECT_EQ(config.video.pool_format, std::string("native"));
  MW_EXPECT_EQ(config.video.capture_io, std::string("auto"));
  MW_ASSERT_OK(ApplyMediaConfigOverride("video.pool_format=nv12", &config));
  MW_EXPECT_OK(ValidateMediaConfig(config));
  MW_ASSERT_OK(ApplyMediaConfigOverride("video.capture_io=dmabuf", &config));
  MW_EXPECT_OK(ValidateMediaConfig(config));
  config.video.capture_io = "mmap";
  MW_EXPECT_FALSE(ValidateMediaConfig(config).ok());
  config.video.capture_io = "auto";
  config.video.pool_format = "rgb";
  MW_EXPECT_FALSE(ValidateMediaConfig(config).ok());
}

}  // namespace mw
