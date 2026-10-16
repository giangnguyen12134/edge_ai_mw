// Copyright 2026 Genio Middleware Authors.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include "mw/npu/image_scaler.h"
#include "mw/npu/npu_config.h"
#include "mw/npu/postprocess.h"
#include "mw/npu/speaker_db.h"
#include "mw/npu/tensor_buffer.h"
#include "mw/npu/thermal_monitor.h"
#include "tests/test_framework.h"

namespace mw {
namespace {

ModelConfig DetectionConfig() {
  ModelConfig c;
  c.name = "object";
  c.kind = "detection";
  c.box_format = "xyxy";
  c.coords_normalized = false;
  c.score_threshold = 0.5f;
  c.labels.push_back("person");
  c.labels.push_back("cup");
  return c;
}

LetterboxInfo Geometry(float pad_x, float pad_y, float cw, float ch, float iw,
                       float ih) {
  LetterboxInfo g;
  g.pad_x = pad_x;
  g.pad_y = pad_y;
  g.content_w = cw;
  g.content_h = ch;
  g.input_w = iw;
  g.input_h = ih;
  return g;
}

OutputTensor Rows(const std::vector<float>& values) {
  OutputTensor t;
  t.data = values;
  t.dims.push_back(static_cast<int>(values.size() / 6));
  t.dims.push_back(6);
  return t;
}

}  // namespace

MW_TEST(NpuConfig, LoadsAndValidatesDevConfig) {
  NpuConfig config;
  MW_ASSERT_OK(LoadNpuConfig(MW_SOURCE_DIR "/config/dev/npu.json", &config));
  MW_EXPECT_OK(ValidateNpuConfig(config));
  MW_EXPECT_EQ(config.models.size(), static_cast<size_t>(4));
}

MW_TEST(NpuConfig, ProductionConfigAlsoLoads) {
  NpuConfig config;
  MW_ASSERT_OK(LoadNpuConfig(MW_SOURCE_DIR "/config/npu.json", &config));
  MW_EXPECT_OK(ValidateNpuConfig(config));
}

MW_TEST(NpuConfig, DuplicateAndUnknownGateAreRejected) {
  NpuConfig config;
  MW_ASSERT_OK(LoadNpuConfig(MW_SOURCE_DIR "/config/dev/npu.json", &config));
  NpuConfig dup = config;
  dup.models.push_back(dup.models[0]);
  MW_EXPECT_FALSE(ValidateNpuConfig(dup).ok());
  NpuConfig gate = config;
  gate.models[0].gated_by = "no_such_model";
  MW_EXPECT_FALSE(ValidateNpuConfig(gate).ok());
}

MW_TEST(Postprocess, DetectionMapsStretchedBoxToFrame) {
  std::unique_ptr<Postprocessor> post;
  MW_ASSERT_OK(CreatePostprocessor(DetectionConfig(), &post));
  const LetterboxInfo g = Geometry(0, 0, 320, 320, 320, 320);
  std::vector<OutputTensor> out;
  out.push_back(Rows({80, 160, 240, 320, 0.9f, 1}));
  std::vector<ResultItem> items;
  MW_ASSERT_OK(post->Process(out, g, &items));
  MW_ASSERT_TRUE(items.size() == 1);
  MW_EXPECT_EQ(items[0].label, std::string("cup"));
  MW_EXPECT_NEAR(items[0].x, 0.25, 1e-6);
  MW_EXPECT_NEAR(items[0].y, 0.5, 1e-6);
  MW_EXPECT_NEAR(items[0].w, 0.5, 1e-6);
  MW_EXPECT_NEAR(items[0].h, 0.5, 1e-6);
}

MW_TEST(Postprocess, LetterboxPaddingIsRemoved) {
  std::unique_ptr<Postprocessor> post;
  MW_ASSERT_OK(CreatePostprocessor(DetectionConfig(), &post));
  // 4:3 frame in a 320x320 input: content 320x240, 40 px bars top/bottom.
  const LetterboxInfo g = Geometry(0, 40, 320, 240, 320, 320);
  std::vector<OutputTensor> out;
  out.push_back(Rows({0, 40, 320, 280, 0.8f, 0}));  // Whole content area.
  std::vector<ResultItem> items;
  MW_ASSERT_OK(post->Process(out, g, &items));
  MW_ASSERT_TRUE(items.size() == 1);
  MW_EXPECT_NEAR(items[0].x, 0.0, 1e-6);
  MW_EXPECT_NEAR(items[0].y, 0.0, 1e-6);
  MW_EXPECT_NEAR(items[0].w, 1.0, 1e-6);
  MW_EXPECT_NEAR(items[0].h, 1.0, 1e-6);
}

MW_TEST(Postprocess, ThresholdSortingAndUnknownClass) {
  std::unique_ptr<Postprocessor> post;
  MW_ASSERT_OK(CreatePostprocessor(DetectionConfig(), &post));
  const LetterboxInfo g = Geometry(0, 0, 100, 100, 100, 100);
  std::vector<OutputTensor> out;
  out.push_back(Rows({0, 0, 10, 10, 0.6f, 0,     //
                      0, 0, 20, 20, 0.9f, 7,     //
                      0, 0, 30, 30, 0.1f, 0}));  // Below threshold.
  std::vector<ResultItem> items;
  MW_ASSERT_OK(post->Process(out, g, &items));
  MW_ASSERT_TRUE(items.size() == 2);
  MW_EXPECT_NEAR(items[0].score, 0.9, 1e-6);  // Best first.
  MW_EXPECT_EQ(items[0].label, std::string("class_7"));
}

MW_TEST(Postprocess, DegenerateBoxesAreDropped) {
  std::unique_ptr<Postprocessor> post;
  MW_ASSERT_OK(CreatePostprocessor(DetectionConfig(), &post));
  const LetterboxInfo g = Geometry(0, 0, 100, 100, 100, 100);
  std::vector<OutputTensor> out;
  out.push_back(Rows({50, 50, 50, 60, 0.9f, 0,         // Zero width.
                      -30, -30, -10, -10, 0.9f, 0}));  // Fully outside.
  std::vector<ResultItem> items;
  MW_ASSERT_OK(post->Process(out, g, &items));
  MW_EXPECT_EQ(items.size(), static_cast<size_t>(0));
}

MW_TEST(Postprocess, DetectionRejectsWrongShape) {
  std::unique_ptr<Postprocessor> post;
  MW_ASSERT_OK(CreatePostprocessor(DetectionConfig(), &post));
  std::vector<OutputTensor> out(1);
  out[0].data.assign(7, 0.0f);
  std::vector<ResultItem> items;
  MW_EXPECT_FALSE(post->Process(out, LetterboxInfo(), &items).ok());
}

MW_TEST(Postprocess, ClassificationSoftmaxPicksBest) {
  ModelConfig c;
  c.kind = "classification";
  c.apply_softmax = true;
  c.score_threshold = 0.3f;
  c.labels.push_back("wave");
  c.labels.push_back("fist");
  c.labels.push_back("palm");
  std::unique_ptr<Postprocessor> post;
  MW_ASSERT_OK(CreatePostprocessor(c, &post));
  std::vector<OutputTensor> out(1);
  out[0].data.push_back(0.1f);
  out[0].data.push_back(3.0f);
  out[0].data.push_back(0.2f);
  std::vector<ResultItem> items;
  MW_ASSERT_OK(post->Process(out, LetterboxInfo(), &items));
  MW_ASSERT_TRUE(items.size() == 1);
  MW_EXPECT_EQ(items[0].label, std::string("fist"));
  MW_EXPECT_TRUE(items[0].score > 0.8 && items[0].score <= 1.0);
}

MW_TEST(Postprocess, ClassificationBelowThresholdGivesNothing) {
  ModelConfig c;
  c.kind = "classification";
  c.apply_softmax = true;
  c.score_threshold = 0.9f;
  std::unique_ptr<Postprocessor> post;
  MW_ASSERT_OK(CreatePostprocessor(c, &post));
  std::vector<OutputTensor> out(1);
  out[0].data.assign(4, 1.0f);  // Uniform: 0.25 each.
  std::vector<ResultItem> items;
  MW_ASSERT_OK(post->Process(out, LetterboxInfo(), &items));
  MW_EXPECT_EQ(items.size(), static_cast<size_t>(0));
}

MW_TEST(Postprocess, UnknownKindIsRejected) {
  ModelConfig c;
  c.kind = "segmentation";
  std::unique_ptr<Postprocessor> post;
  MW_EXPECT_FALSE(CreatePostprocessor(c, &post).ok());
}

MW_TEST(Vector, L2NormalizeAndCosine) {
  std::vector<float> v;
  v.push_back(3.0f);
  v.push_back(4.0f);
  MW_ASSERT_TRUE(L2Normalize(&v));
  MW_EXPECT_NEAR(v[0], 0.6, 1e-6);
  MW_EXPECT_NEAR(v[1], 0.8, 1e-6);
  std::vector<float> zero(3, 0.0f);
  MW_EXPECT_FALSE(L2Normalize(&zero));

  std::vector<float> a(2, 0.0f), b(2, 0.0f), c(2, 0.0f);
  a[0] = 1;
  b[0] = 2;
  c[1] = 1;
  MW_EXPECT_NEAR(CosineSimilarity(a, b), 1.0, 1e-6);
  MW_EXPECT_NEAR(CosineSimilarity(a, c), 0.0, 1e-6);
  MW_EXPECT_NEAR(CosineSimilarity(a, std::vector<float>(3, 1.0f)), 0.0, 1e-6);
}

MW_TEST(SpeakerDb, MatchAddRemove) {
  SpeakerDb db("");
  std::string name;
  float score = -1;
  db.Match(std::vector<float>(2, 1.0f), &name, &score);
  MW_EXPECT_EQ(name, std::string("unknown"));
  MW_EXPECT_NEAR(score, 0.0, 1e-9);

  std::vector<float> alice(3, 0.0f), bob(3, 0.0f);
  alice[0] = 1;
  bob[1] = 1;
  MW_ASSERT_OK(db.Add("alice", alice));
  MW_ASSERT_OK(db.Add("bob", bob));
  db.Match(alice, &name, &score);
  MW_EXPECT_EQ(name, std::string("alice"));
  MW_EXPECT_NEAR(score, 1.0, 1e-5);
  MW_EXPECT_EQ(db.Names().size(), static_cast<size_t>(2));

  MW_ASSERT_OK(db.Remove("alice"));
  MW_EXPECT_CODE(db.Remove("alice"), Status::kNotFound);
  MW_EXPECT_EQ(db.Names().size(), static_cast<size_t>(1));
}

MW_TEST(SpeakerDb, RejectsEmptyNameAndEmbedding) {
  SpeakerDb db("");
  MW_EXPECT_FALSE(db.Add("", std::vector<float>(3, 1.0f)).ok());
  MW_EXPECT_FALSE(db.Add("x", std::vector<float>()).ok());
}

MW_TEST(SpeakerDb, PersistsAcrossInstances) {
  char path[] = "/tmp/mw_speakers_XXXXXX";
  const int fd = mkstemp(path);
  MW_ASSERT_TRUE(fd >= 0);
  close(fd);
  unlink(path);  // mkstemp only reserved a unique name; start with no file.
  std::vector<float> e(4, 0.0f);
  e[2] = 1;
  {
    SpeakerDb db(path);
    MW_ASSERT_OK(db.Load());
    MW_ASSERT_OK(db.Add("carol", e));
  }
  SpeakerDb reloaded(path);
  MW_ASSERT_OK(reloaded.Load());
  std::string name;
  float score = 0;
  reloaded.Match(e, &name, &score);
  MW_EXPECT_EQ(name, std::string("carol"));
  MW_EXPECT_TRUE(score > 0.99f);
  unlink(path);
}

MW_TEST(SpeakerDb, MissingFileStartsEmpty) {
  SpeakerDb db("/tmp/mw_does_not_exist_speakers.json");
  MW_EXPECT_OK(db.Load());
  MW_EXPECT_EQ(db.Names().size(), static_cast<size_t>(0));
}

MW_TEST(Thermal, DisabledMonitorNeverThrottles) {
  ThermalMonitor monitor("", 85.0, 0.5);
  MW_EXPECT_NEAR(monitor.RateScale(), 1.0, 1e-9);
  MW_EXPECT_FALSE(monitor.throttled());
}

MW_TEST(Thermal, HotSensorScalesRateDown) {
  char path[] = "/tmp/mw_thermal_XXXXXX";
  const int fd = mkstemp(path);
  MW_ASSERT_TRUE(fd >= 0);
  const char* hot = "90000\n";  // Millidegrees Celsius.
  MW_ASSERT_TRUE(write(fd, hot, 6) == 6);
  close(fd);
  ThermalMonitor monitor(path, 85.0, 0.5);
  MW_EXPECT_NEAR(monitor.RateScale(), 0.5, 1e-9);
  MW_EXPECT_TRUE(monitor.throttled());
  MW_EXPECT_NEAR(monitor.last_temp_c(), 90.0, 1e-6);
  unlink(path);
}

MW_TEST(Thermal, UnreadableSensorIsHarmless) {
  ThermalMonitor monitor("/nonexistent/temp", 85.0, 0.5);
  MW_EXPECT_NEAR(monitor.RateScale(), 1.0, 1e-9);
}

namespace {

// Builds an NV12 image where Y = |y| and U = V = 128 (a flat grey).
void FlatNv12(uint32_t w, uint32_t h, uint8_t y, std::vector<uint8_t>* buf,
              FrameFormat* format) {
  *format = FrameFormat::Nv12(w, h);
  buf->assign(format->buffer_size, 0);
  for (uint32_t row = 0; row < h; ++row) {
    memset(buf->data() + row * format->stride, y, w);
  }
  memset(buf->data() + format->uv_offset, 128, format->stride * h / 2);
}

}  // namespace

MW_TEST(ImageScaler, StretchFillsWholeInput) {
  std::unique_ptr<BufferAllocator> allocator;
  MW_ASSERT_OK(CreateBufferAllocator("memfd", &allocator));
  std::unique_ptr<ImageScaler> scaler;
  MW_ASSERT_OK(CreateImageScaler("cpu", &scaler));

  std::vector<uint8_t> frame;
  SourceImage src;
  FlatNv12(64, 48, 235, &frame, &src.format);  // White in limited range.
  src.data = frame.data();

  ModelConfig config;
  config.resize_mode = "stretch";
  config.color_order = "rgb";
  InputSpec spec;
  spec.width = 32;
  spec.height = 32;
  spec.channels = 3;
  spec.type = kTensorUint8;
  TensorBuffer dst;
  MW_ASSERT_OK(dst.Allocate(allocator.get(), spec.bytes()));
  LetterboxInfo g;
  MW_ASSERT_OK(scaler->Convert(src, config, spec, &dst, &g));
  MW_EXPECT_NEAR(g.pad_x, 0.0, 1e-9);
  MW_EXPECT_NEAR(g.content_w, 32.0, 1e-9);
  for (size_t i = 0; i < spec.bytes(); ++i) {
    MW_EXPECT_TRUE(dst.data()[i] >= 250);
  }
}

MW_TEST(ImageScaler, LetterboxPadsAndReportsGeometry) {
  std::unique_ptr<BufferAllocator> allocator;
  MW_ASSERT_OK(CreateBufferAllocator("memfd", &allocator));
  std::unique_ptr<ImageScaler> scaler;
  MW_ASSERT_OK(CreateImageScaler("cpu", &scaler));

  std::vector<uint8_t> frame;
  SourceImage src;
  FlatNv12(64, 32, 235, &frame, &src.format);  // 2:1 frame.
  src.data = frame.data();

  ModelConfig config;
  config.resize_mode = "letterbox";
  config.color_order = "rgb";
  InputSpec spec;
  spec.width = 32;
  spec.height = 32;
  spec.channels = 3;
  spec.type = kTensorUint8;
  TensorBuffer dst;
  MW_ASSERT_OK(dst.Allocate(allocator.get(), spec.bytes()));
  LetterboxInfo g;
  MW_ASSERT_OK(scaler->Convert(src, config, spec, &dst, &g));
  MW_EXPECT_NEAR(g.content_w, 32.0, 1e-9);
  MW_EXPECT_NEAR(g.content_h, 16.0, 1e-9);
  MW_EXPECT_NEAR(g.pad_y, 8.0, 1e-9);
  MW_EXPECT_EQ(dst.data()[0], static_cast<uint8_t>(114));  // Top bar grey.
  const size_t centre = (16 * 32 + 16) * 3;
  MW_EXPECT_TRUE(dst.data()[centre] >= 250);  // Picture white.
}

MW_TEST(ImageScaler, RejectsNonNv12AndMissingData) {
  std::unique_ptr<BufferAllocator> allocator;
  MW_ASSERT_OK(CreateBufferAllocator("memfd", &allocator));
  std::unique_ptr<ImageScaler> scaler;
  MW_ASSERT_OK(CreateImageScaler("cpu", &scaler));
  InputSpec spec;
  spec.width = 8;
  spec.height = 8;
  spec.channels = 3;
  TensorBuffer dst;
  MW_ASSERT_OK(dst.Allocate(allocator.get(), spec.bytes()));
  LetterboxInfo g;
  SourceImage src;  // data == nullptr.
  src.format = FrameFormat::Nv12(16, 16);
  MW_EXPECT_FALSE(scaler->Convert(src, ModelConfig(), spec, &dst, &g).ok());
  std::vector<uint8_t> frame(src.format.buffer_size);
  src.data = frame.data();
  src.format.fourcc = kFourccYuyv;
  MW_EXPECT_FALSE(scaler->Convert(src, ModelConfig(), spec, &dst, &g).ok());
  std::unique_ptr<ImageScaler> other;
  MW_EXPECT_FALSE(CreateImageScaler("gpu", &other).ok());
}

}  // namespace mw
