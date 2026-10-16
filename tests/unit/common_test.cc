// Copyright 2026 Genio Middleware Authors.

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "mw/common/audio_ring.h"
#include "mw/common/buffer_allocator.h"
#include "mw/common/dma_buf_mapping.h"
#include "mw/common/frame_types.h"
#include "mw/common/json.h"
#include "mw/common/status.h"
#include "tests/test_framework.h"

namespace mw {

MW_TEST(Status, OkAndErrors) {
  MW_EXPECT_TRUE(Status::Ok().ok());
  MW_EXPECT_EQ(Status::Ok().ToString(), std::string("OK"));
  const Status s = NotFoundError("thing");
  MW_EXPECT_FALSE(s.ok());
  MW_EXPECT_CODE(s, Status::kNotFound);
  MW_EXPECT_EQ(s.ToString(), std::string("NotFound: thing"));
}

MW_TEST(Status, ErrnoMapping) {
  MW_EXPECT_CODE(ErrnoStatus("open", ENOENT), Status::kNotFound);
  MW_EXPECT_CODE(ErrnoStatus("open", EACCES), Status::kPermissionDenied);
  MW_EXPECT_FALSE(ErrnoStatus("x", EIO).ok());
}

MW_TEST(Json, ParsesNestedDocument) {
  JsonValue v;
  MW_ASSERT_OK(JsonValue::Parse(
      "{\"a\": 1.5, \"b\": [true, null, \"x\\n\"], \"c\": {\"d\": \"e\"}}",
      &v));
  MW_EXPECT_TRUE(v.is_object());
  MW_EXPECT_NEAR(v.GetDouble("a", 0), 1.5, 1e-9);
  const JsonValue* b = v.Find("b");
  MW_ASSERT_TRUE(b != nullptr && b->is_array());
  MW_EXPECT_EQ(b->array().size(), static_cast<size_t>(3));
  MW_EXPECT_EQ(b->array()[2].string_value(), std::string("x\n"));
  MW_EXPECT_EQ(v.Find("c")->GetString("d", ""), std::string("e"));
}

MW_TEST(Json, DefaultsForMissingOrWrongType) {
  JsonValue v;
  MW_ASSERT_OK(JsonValue::Parse("{\"n\": \"text\"}", &v));
  MW_EXPECT_EQ(v.GetInt("n", 7), 7);
  MW_EXPECT_EQ(v.GetInt("missing", 9), 9);
  MW_EXPECT_TRUE(v.Find("missing") == nullptr);
}

MW_TEST(Json, RejectsMalformedInput) {
  JsonValue v;
  MW_EXPECT_FALSE(JsonValue::Parse("{", &v).ok());
  MW_EXPECT_FALSE(JsonValue::Parse("{\"a\": }", &v).ok());
  MW_EXPECT_FALSE(JsonValue::Parse("[1, 2,]", &v).ok());
  MW_EXPECT_FALSE(JsonValue::Parse("{} trailing", &v).ok());
  MW_EXPECT_FALSE(JsonValue::Parse("", &v).ok());
}

MW_TEST(Json, MissingFileIsAnError) {
  JsonValue v;
  MW_EXPECT_FALSE(JsonValue::ParseFile("/nonexistent/mw.json", &v).ok());
}

MW_TEST(FrameTypes, Nv12Layout) {
  const FrameFormat f = FrameFormat::Nv12(640, 480);
  MW_EXPECT_EQ(f.fourcc, kFourccNv12);
  MW_EXPECT_EQ(f.stride % 16, 0u);
  MW_EXPECT_TRUE(f.stride >= 640);
  MW_EXPECT_EQ(f.uv_offset, f.stride * 480);
  MW_EXPECT_TRUE(f.buffer_size >= f.stride * 480 * 3 / 2);
  MW_EXPECT_EQ(f.buffer_size % 4096, 0u);
}

MW_TEST(FrameTypes, OddWidthIsAligned) {
  const FrameFormat f = FrameFormat::Nv12(642, 480);
  MW_EXPECT_EQ(f.stride, 656u);
}

MW_TEST(FrameTypes, PixelFormatNames) {
  uint32_t fourcc = 0;
  MW_EXPECT_TRUE(ParsePixelFormat("NV12", &fourcc));
  MW_EXPECT_EQ(fourcc, kFourccNv12);
  MW_EXPECT_TRUE(ParsePixelFormat("YUYV", &fourcc));
  MW_EXPECT_EQ(fourcc, kFourccYuyv);
  MW_EXPECT_TRUE(ParsePixelFormat("MJPG", &fourcc));
  MW_EXPECT_EQ(fourcc, kFourccMjpeg);
  MW_EXPECT_FALSE(ParsePixelFormat("RGB3", &fourcc));
  MW_EXPECT_EQ(FourccToString(kFourccNv12), std::string("NV12"));
}

MW_TEST(FrameTypes, AlignUp) {
  MW_EXPECT_EQ(AlignUp(1, 16), 16u);
  MW_EXPECT_EQ(AlignUp(16, 16), 16u);
  MW_EXPECT_EQ(AlignUp(17, 16), 32u);
}

MW_TEST(Allocator, MemfdMapsAndShares) {
  std::unique_ptr<BufferAllocator> allocator;
  MW_ASSERT_OK(CreateBufferAllocator("memfd", &allocator));
  MW_EXPECT_FALSE(allocator->IsDmaBuf());
  ScopedFd fd;
  MW_ASSERT_OK(allocator->Allocate(8192, &fd));
  MW_ASSERT_TRUE(fd.valid());

  DmaBufMapping writer;
  MW_ASSERT_OK(writer.Map(fd.get(), 8192, true));
  writer.BeginCpuAccess(true);
  memcpy(writer.data(), "hello", 6);
  writer.EndCpuAccess(true);

  // A second mapping of the same fd sees the same bytes (what a client does).
  DmaBufMapping reader;
  MW_ASSERT_OK(reader.Map(fd.get(), 8192, false));
  MW_EXPECT_EQ(std::string(reinterpret_cast<const char*>(reader.data())),
               std::string("hello"));
}

MW_TEST(Allocator, UnknownSpecIsRejected) {
  std::unique_ptr<BufferAllocator> allocator;
  MW_EXPECT_FALSE(CreateBufferAllocator("bogus", &allocator).ok());
}

MW_TEST(AudioRing, WriteThenRead) {
  AudioRingWriter writer;
  MW_ASSERT_OK(writer.Create(16000, 1, 100));
  AudioRingReader reader;
  MW_ASSERT_OK(reader.Attach(writer.fd()));
  MW_EXPECT_EQ(reader.sample_rate(), 16000u);
  MW_EXPECT_EQ(reader.capacity_frames(), writer.capacity_frames());

  std::vector<int16_t> in(320);
  for (size_t i = 0; i < in.size(); ++i) {
    in[i] = static_cast<int16_t>(i);
  }
  writer.Write(in.data(), in.size());
  MW_EXPECT_EQ(reader.WritePosition(), static_cast<uint64_t>(320));

  uint64_t cursor = 0;
  uint64_t lost = 0;
  std::vector<int16_t> out(400, -1);
  const size_t n = reader.Read(&cursor, out.data(), out.size(), &lost);
  MW_EXPECT_EQ(n, static_cast<size_t>(320));
  MW_EXPECT_EQ(cursor, static_cast<uint64_t>(320));
  MW_EXPECT_EQ(lost, static_cast<uint64_t>(0));
  MW_EXPECT_EQ(out[0], static_cast<int16_t>(0));
  MW_EXPECT_EQ(out[319], static_cast<int16_t>(319));
  // Nothing new: a further read returns zero frames.
  MW_EXPECT_EQ(reader.Read(&cursor, out.data(), out.size(), &lost),
               static_cast<size_t>(0));
}

MW_TEST(AudioRing, SlowReaderLosesFramesAndResyncs) {
  AudioRingWriter writer;
  MW_ASSERT_OK(writer.Create(16000, 1, 100));  // 1600 frames.
  AudioRingReader reader;
  MW_ASSERT_OK(reader.Attach(writer.fd()));
  const uint32_t capacity = writer.capacity_frames();

  std::vector<int16_t> block(capacity, 5);
  writer.Write(block.data(), block.size());
  writer.Write(block.data(), block.size() / 2);  // Lap the reader.

  uint64_t cursor = 0;
  uint64_t lost = 0;
  std::vector<int16_t> out(capacity * 2);
  const size_t n = reader.Read(&cursor, out.data(), out.size(), &lost);
  MW_EXPECT_TRUE(lost > 0);
  MW_EXPECT_TRUE(n <= capacity);
  MW_EXPECT_EQ(cursor, reader.WritePosition());
}

MW_TEST(AudioRing, RejectsGarbageFd) {
  std::unique_ptr<BufferAllocator> allocator;
  MW_ASSERT_OK(CreateBufferAllocator("memfd", &allocator));
  ScopedFd fd;
  MW_ASSERT_OK(allocator->Allocate(4096, &fd));  // Zeroed: bad magic.
  AudioRingReader reader;
  MW_EXPECT_FALSE(reader.Attach(fd.get()).ok());
}

}  // namespace mw
