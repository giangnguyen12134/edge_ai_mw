// Copyright 2026 Genio Middleware Authors.

#include <stdint.h>
#include <string.h>

#include <memory>
#include <vector>

#include "mw/media/frame_pool.h"
#include "mw/media/media_config.h"
#include "mw/media/test_pattern_source.h"
#include "tests/test_framework.h"

namespace mw {
namespace {

std::unique_ptr<BufferAllocator> MemfdAllocator() {
  std::unique_ptr<BufferAllocator> allocator;
  CreateBufferAllocator("memfd", &allocator);
  return allocator;
}

}  // namespace

MW_TEST(MediaConfig, DefaultsValidate) {
  MediaConfig config;
  MW_EXPECT_OK(ValidateMediaConfig(config));
}

MW_TEST(MediaConfig, OverridesApply) {
  MediaConfig config;
  MW_ASSERT_OK(ApplyMediaConfigOverride("video.width=1280", &config));
  MW_ASSERT_OK(ApplyMediaConfigOverride("video.height=720", &config));
  MW_ASSERT_OK(ApplyMediaConfigOverride("video.pixel_format=MJPG", &config));
  MW_EXPECT_EQ(config.video.width, 1280u);
  MW_EXPECT_EQ(config.video.height, 720u);
  MW_EXPECT_EQ(config.video.pixel_format, kFourccMjpeg);
  MW_EXPECT_OK(ValidateMediaConfig(config));
}

MW_TEST(MediaConfig, BadOverridesAreRejected) {
  MediaConfig config;
  MW_EXPECT_FALSE(ApplyMediaConfigOverride("video.width=abc", &config).ok());
  MW_EXPECT_FALSE(ApplyMediaConfigOverride("no_such_key=1", &config).ok());
  MW_EXPECT_FALSE(ApplyMediaConfigOverride("novalue", &config).ok());
  MW_EXPECT_FALSE(
      ApplyMediaConfigOverride("video.pixel_format=XXXX", &config).ok());
}

MW_TEST(MediaConfig, InvalidValuesFailValidation) {
  MediaConfig config;
  config.video.pool_slots = 0;
  MW_EXPECT_FALSE(ValidateMediaConfig(config).ok());
  config = MediaConfig();
  config.video.pool_slots = 64;  // Above the documented maximum of 16.
  MW_EXPECT_FALSE(ValidateMediaConfig(config).ok());
  config = MediaConfig();
  config.video.width = 0;
  MW_EXPECT_FALSE(ValidateMediaConfig(config).ok());
  config = MediaConfig();
  config.bus = "session";
  MW_EXPECT_FALSE(ValidateMediaConfig(config).ok());
}

MW_TEST(FramePool, AcquireUntilExhaustedThenRelease) {
  std::unique_ptr<BufferAllocator> allocator = MemfdAllocator();
  FramePool pool;
  const FrameFormat format = FrameFormat::Nv12(64, 48);
  MW_ASSERT_OK(pool.Init(allocator.get(), 3, format));
  MW_EXPECT_EQ(pool.slot_count(), 3u);
  MW_EXPECT_EQ(pool.free_count(), 3u);

  const int a = pool.Acquire();
  const int b = pool.Acquire();
  const int c = pool.Acquire();
  MW_EXPECT_TRUE(a >= 0 && b >= 0 && c >= 0);
  MW_EXPECT_TRUE(a != b && b != c && a != c);
  MW_EXPECT_EQ(pool.Acquire(), -1);
  MW_EXPECT_EQ(pool.free_count(), 0u);

  pool.Unref(static_cast<uint32_t>(b));
  MW_EXPECT_EQ(pool.free_count(), 1u);
  MW_EXPECT_EQ(pool.Acquire(), b);
}

MW_TEST(FramePool, SlotStaysBusyUntilLastReference) {
  std::unique_ptr<BufferAllocator> allocator = MemfdAllocator();
  FramePool pool;
  MW_ASSERT_OK(pool.Init(allocator.get(), 2, FrameFormat::Nv12(64, 48)));
  const int slot = pool.Acquire();
  MW_ASSERT_TRUE(slot >= 0);
  pool.AddRef(static_cast<uint32_t>(slot));
  pool.AddRef(static_cast<uint32_t>(slot));
  pool.Unref(static_cast<uint32_t>(slot));
  pool.Unref(static_cast<uint32_t>(slot));
  MW_EXPECT_EQ(pool.free_count(), 1u);  // One reference still held.
  pool.Unref(static_cast<uint32_t>(slot));
  MW_EXPECT_EQ(pool.free_count(), 2u);
}

MW_TEST(FramePool, SlotsAreWritableAndDistinct) {
  std::unique_ptr<BufferAllocator> allocator = MemfdAllocator();
  FramePool pool;
  MW_ASSERT_OK(pool.Init(allocator.get(), 2, FrameFormat::Nv12(64, 48)));
  MW_EXPECT_TRUE(pool.fd(0) >= 0 && pool.fd(1) >= 0);
  MW_EXPECT_TRUE(pool.fd(0) != pool.fd(1));
  pool.data(0)[0] = 0xAB;
  pool.data(1)[0] = 0xCD;
  MW_EXPECT_EQ(pool.data(0)[0], static_cast<uint8_t>(0xAB));
  MW_EXPECT_EQ(pool.data(1)[0], static_cast<uint8_t>(0xCD));
}

MW_TEST(TestPattern, ProducesFramesInConfiguredFormat) {
  VideoStreamConfig config;
  config.source = "test_pattern";
  config.width = 64;
  config.height = 48;
  config.fps = 100;
  config.pixel_format = kFourccNv12;
  std::unique_ptr<FrameSource> source;
  MW_ASSERT_OK(CreateFrameSource(config, &source));
  MW_ASSERT_OK(source->Open());
  MW_ASSERT_OK(source->Start());
  CapturedFrame frame;
  Status status = TimeoutError("none yet");
  for (int i = 0; i < 50 && !status.ok(); ++i) {
    status = source->Capture(200, &frame);
  }
  MW_ASSERT_OK(status);
  MW_EXPECT_TRUE(frame.bytes_used > 0);
  source->Release(frame);
  source->Stop();
}

}  // namespace mw
