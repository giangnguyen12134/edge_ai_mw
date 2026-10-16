// Copyright 2026 Genio Middleware Authors.
//
// Tests of the ONNX Runtime CPU backend with the synthetic models in
// tests/data (see tools/make_test_models.py). They need a libonnxruntime.so:
// set MW_ONNX_LIBRARY to its path, otherwise the default soname is tried and
// the tests are skipped when it cannot be loaded.

#include "mw/npu/cpu_runtime.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include "mw/npu/image_scaler.h"
#include "mw/npu/postprocess.h"
#include "mw/npu/tensor_buffer.h"
#include "tests/test_framework.h"

namespace mw {
namespace {

const char kData[] = MW_SOURCE_DIR "/tests/data/";

NpuConfig CpuConfig() {
  NpuConfig config;
  config.runtime = "cpu";
  const char* library = getenv("MW_ONNX_LIBRARY");
  if (library != nullptr && library[0] != '\0') {
    config.onnx_library = library;
  }
  config.cpu_threads = 1;
  return config;
}

// Returns false (and the test should skip) when ONNX Runtime is unavailable.
bool CreateRuntime(std::unique_ptr<NpuRuntime>* runtime) {
  return CreateCpuRuntime(CpuConfig(), runtime).ok();
}

ModelConfig ImageModel(const std::string& file, uint32_t size) {
  ModelConfig config;
  config.name = "m";
  config.kind = "detection";
  config.path = std::string(kData) + file;
  config.input_width = size;
  config.input_height = size;
  return config;
}

// NHWC image whose channels are constant: R=10, G=100, B=200.
void FillColour(TensorBuffer* buffer, const InputSpec& spec) {
  const size_t pixels = static_cast<size_t>(spec.width) * spec.height;
  const uint8_t colour[3] = {10, 100, 200};
  for (size_t i = 0; i < pixels; ++i) {
    for (size_t c = 0; c < 3; ++c) {
      if (spec.type == kTensorFloat32) {
        reinterpret_cast<float*>(buffer->data())[i * 3 + c] = colour[c];
      } else {
        buffer->data()[i * 3 + c] = colour[c];
      }
    }
  }
}

// Loads |file|, runs it on a colour image and checks the per-channel means.
void ExpectChannelMeans(const std::string& file, uint32_t size,
                        const std::string& input_type,
                        const std::string& layout) {
  std::unique_ptr<NpuRuntime> runtime;
  if (!CreateRuntime(&runtime)) {
    return;
  }
  ModelConfig config = ImageModel(file, size);
  config.input_type = input_type;
  config.input_layout = layout;
  std::unique_ptr<NpuModel> model;
  MW_ASSERT_OK(runtime->Load(config, &model));

  std::unique_ptr<BufferAllocator> allocator;
  MW_ASSERT_OK(CreateBufferAllocator("memfd", &allocator));
  TensorBuffer input;
  MW_ASSERT_OK(input.Allocate(allocator.get(), model->input_spec().bytes()));
  FillColour(&input, model->input_spec());

  std::vector<OutputTensor> outputs;
  MW_ASSERT_OK(model->Run(input, &outputs));
  MW_ASSERT_TRUE(outputs.size() == 1 && outputs[0].data.size() == 3);
  MW_EXPECT_NEAR(outputs[0].data[0], 10.0, 1e-3);
  MW_EXPECT_NEAR(outputs[0].data[1], 100.0, 1e-3);
  MW_EXPECT_NEAR(outputs[0].data[2], 200.0, 1e-3);
}

}  // namespace

MW_TEST(CpuRuntime, MissingLibraryIsReported) {
  NpuConfig config = CpuConfig();
  config.onnx_library = "/nonexistent/libonnxruntime.so";
  std::unique_ptr<NpuRuntime> runtime;
  MW_EXPECT_CODE(CreateCpuRuntime(config, &runtime), Status::kUnavailable);
  MW_EXPECT_TRUE(runtime == nullptr);
}

MW_TEST(CpuRuntime, WrongLibraryIsReported) {
  NpuConfig config = CpuConfig();
  config.onnx_library = "libm.so.6";  // Loads, but is not ONNX Runtime.
  std::unique_ptr<NpuRuntime> runtime;
  MW_EXPECT_CODE(CreateCpuRuntime(config, &runtime), Status::kInvalidArgument);
}

MW_TEST(CpuRuntime, FactoryDispatchesOnRuntimeName) {
  NpuConfig config = CpuConfig();
  config.onnx_library = "/nonexistent/libonnxruntime.so";
  std::unique_ptr<NpuRuntime> runtime;
  MW_EXPECT_CODE(CreateNpuRuntime(config, &runtime), Status::kUnavailable);
  config.runtime = "bogus";
  MW_EXPECT_CODE(CreateNpuRuntime(config, &runtime), Status::kInvalidArgument);
}

MW_TEST(CpuRuntime, DescribesItself) {
  std::unique_ptr<NpuRuntime> runtime;
  if (!CreateRuntime(&runtime)) {
    MW_SKIP("libonnxruntime not available (set MW_ONNX_LIBRARY)");
  }
  MW_EXPECT_TRUE(runtime->Describe().find("cpu") == 0);
}

MW_TEST(CpuRuntime, NhwcFloatModel) {
  ExpectChannelMeans("chan_nhwc_f32.onnx", 32, "float32", "auto");
}

MW_TEST(CpuRuntime, NchwFloatModelGetsTransposedInput) {
  ExpectChannelMeans("chan_nchw_f32.onnx", 32, "float32", "auto");
}

MW_TEST(CpuRuntime, DynamicShapeModelFollowsConfiguredSize) {
  ExpectChannelMeans("chan_nchw_dyn.onnx", 24, "float32", "auto");
  ExpectChannelMeans("chan_nchw_dyn.onnx", 40, "float32", "nchw");
}

MW_TEST(CpuRuntime, Uint8SourceIsCastForFloatModel) {
  ExpectChannelMeans("chan_nhwc_f32.onnx", 32, "uint8", "auto");
  ExpectChannelMeans("chan_nchw_f32.onnx", 32, "uint8", "auto");
}

MW_TEST(CpuRuntime, Uint8Model) {
  ExpectChannelMeans("chan_nhwc_u8.onnx", 32, "uint8", "auto");
}

MW_TEST(CpuRuntime, Uint8ModelRejectsFloatConfig) {
  std::unique_ptr<NpuRuntime> runtime;
  if (!CreateRuntime(&runtime)) {
    MW_SKIP("libonnxruntime not available (set MW_ONNX_LIBRARY)");
  }
  ModelConfig config = ImageModel("chan_nhwc_u8.onnx", 32);
  config.input_type = "float32";
  std::unique_ptr<NpuModel> model;
  MW_EXPECT_CODE(runtime->Load(config, &model), Status::kInvalidArgument);
}

MW_TEST(CpuRuntime, ShapeMismatchAndBadFilesAreErrors) {
  std::unique_ptr<NpuRuntime> runtime;
  if (!CreateRuntime(&runtime)) {
    MW_SKIP("libonnxruntime not available (set MW_ONNX_LIBRARY)");
  }
  std::unique_ptr<NpuModel> model;
  MW_EXPECT_CODE(runtime->Load(ImageModel("chan_nhwc_f32.onnx", 64), &model),
                 Status::kInvalidArgument);
  MW_EXPECT_FALSE(runtime->Load(ImageModel("missing.onnx", 32), &model).ok());
  ModelConfig bad = ImageModel("chan_nhwc_f32.onnx", 32);
  bad.path = MW_SOURCE_DIR "/CMakeLists.txt";  // Not an ONNX file.
  MW_EXPECT_FALSE(runtime->Load(bad, &model).ok());
  MW_EXPECT_TRUE(model == nullptr);
}

MW_TEST(CpuRuntime, RepeatedRunsAreStable) {
  std::unique_ptr<NpuRuntime> runtime;
  if (!CreateRuntime(&runtime)) {
    MW_SKIP("libonnxruntime not available (set MW_ONNX_LIBRARY)");
  }
  std::unique_ptr<NpuModel> model;
  MW_ASSERT_OK(runtime->Load(ImageModel("chan_nchw_f32.onnx", 32), &model));
  std::unique_ptr<BufferAllocator> allocator;
  MW_ASSERT_OK(CreateBufferAllocator("memfd", &allocator));
  TensorBuffer input;
  MW_ASSERT_OK(input.Allocate(allocator.get(), model->input_spec().bytes()));
  FillColour(&input, model->input_spec());
  for (int i = 0; i < 50; ++i) {
    std::vector<OutputTensor> outputs;
    MW_ASSERT_OK(model->Run(input, &outputs));
    MW_ASSERT_TRUE(outputs.size() == 1 && outputs[0].data.size() == 3);
  }
}

MW_TEST(CpuRuntime, ModelOutlivesRuntimeObject) {
  std::unique_ptr<NpuRuntime> runtime;
  if (!CreateRuntime(&runtime)) {
    MW_SKIP("libonnxruntime not available (set MW_ONNX_LIBRARY)");
  }
  std::unique_ptr<NpuModel> model;
  MW_ASSERT_OK(runtime->Load(ImageModel("chan_nhwc_f32.onnx", 32), &model));
  runtime.reset();  // The model keeps the library state alive.
  std::unique_ptr<BufferAllocator> allocator;
  MW_ASSERT_OK(CreateBufferAllocator("memfd", &allocator));
  TensorBuffer input;
  MW_ASSERT_OK(input.Allocate(allocator.get(), model->input_spec().bytes()));
  FillColour(&input, model->input_spec());
  std::vector<OutputTensor> outputs;
  MW_EXPECT_OK(model->Run(input, &outputs));
}

MW_TEST(CpuRuntime, DetectionModelThroughScalerAndPostprocessor) {
  std::unique_ptr<NpuRuntime> runtime;
  if (!CreateRuntime(&runtime)) {
    MW_SKIP("libonnxruntime not available (set MW_ONNX_LIBRARY)");
  }
  ModelConfig config = ImageModel("det_face.onnx", 320);
  config.labels.push_back("face");
  config.resize_mode = "stretch";
  config.score_threshold = 0.5f;
  std::unique_ptr<NpuModel> model;
  MW_ASSERT_OK(runtime->Load(config, &model));

  // 640x480 grey frame -> 320x320 tensor -> model -> normalised box.
  SourceImage src;
  src.format = FrameFormat::Nv12(640, 480);
  std::vector<uint8_t> frame(src.format.buffer_size, 128);
  src.data = frame.data();
  std::unique_ptr<ImageScaler> scaler;
  MW_ASSERT_OK(CreateImageScaler("cpu", &scaler));
  std::unique_ptr<BufferAllocator> allocator;
  MW_ASSERT_OK(CreateBufferAllocator("memfd", &allocator));
  TensorBuffer input;
  MW_ASSERT_OK(input.Allocate(allocator.get(), model->input_spec().bytes()));
  LetterboxInfo geometry;
  MW_ASSERT_OK(
      scaler->Convert(src, config, model->input_spec(), &input, &geometry));

  std::vector<OutputTensor> outputs;
  MW_ASSERT_OK(model->Run(input, &outputs));
  std::unique_ptr<Postprocessor> post;
  MW_ASSERT_OK(CreatePostprocessor(config, &post));
  std::vector<ResultItem> items;
  MW_ASSERT_OK(post->Process(outputs, geometry, &items));
  MW_ASSERT_TRUE(items.size() == 1);
  MW_EXPECT_EQ(items[0].label, std::string("face"));
  MW_EXPECT_NEAR(items[0].score, 0.9, 1e-5);
  MW_EXPECT_NEAR(items[0].x, 64.0 / 320.0, 1e-5);  // Model px -> 0..1.
  MW_EXPECT_NEAR(items[0].w, 192.0 / 320.0, 1e-5);
}

MW_TEST(CpuRuntime, SpeakerModelTakesFloatPcmWindow) {
  std::unique_ptr<NpuRuntime> runtime;
  if (!CreateRuntime(&runtime)) {
    MW_SKIP("libonnxruntime not available (set MW_ONNX_LIBRARY)");
  }
  ModelConfig config;
  config.name = "voiceprint";
  config.kind = "speaker";
  config.path = std::string(kData) + "speaker_stats.onnx";
  config.sample_rate = 16000;
  config.window_ms = 1500;
  std::unique_ptr<NpuModel> model;
  MW_ASSERT_OK(runtime->Load(config, &model));
  MW_EXPECT_EQ(model->input_spec().width, 24000u);

  std::unique_ptr<BufferAllocator> allocator;
  MW_ASSERT_OK(CreateBufferAllocator("memfd", &allocator));
  TensorBuffer input;
  MW_ASSERT_OK(input.Allocate(allocator.get(), model->input_spec().bytes()));
  float* pcm = reinterpret_cast<float*>(input.data());
  for (size_t i = 0; i < 24000; ++i) {
    pcm[i] = (i % 2 == 0) ? 0.5f : -0.25f;
  }
  std::vector<OutputTensor> outputs;
  MW_ASSERT_OK(model->Run(input, &outputs));
  MW_ASSERT_TRUE(outputs.size() == 1 && outputs[0].data.size() == 4);
  MW_EXPECT_NEAR(outputs[0].data[0], 0.125, 1e-5);  // mean
  MW_EXPECT_NEAR(outputs[0].data[1], 0.5, 1e-6);    // max
  MW_EXPECT_NEAR(outputs[0].data[2], -0.25, 1e-6);  // min
  MW_EXPECT_NEAR(outputs[0].data[3], 0.375, 1e-5);  // mean |x|
}

MW_TEST(NpuConfig, CpuRuntimeSettings) {
  NpuConfig config;
  MW_ASSERT_OK(
      LoadNpuConfig(MW_SOURCE_DIR "/config/dev/npu_onnx.json", &config));
  MW_EXPECT_EQ(config.runtime, std::string("cpu"));
  MW_EXPECT_OK(ValidateNpuConfig(config));
  MW_ASSERT_OK(ApplyNpuConfigOverride("cpu_threads=3", &config));
  MW_EXPECT_EQ(config.cpu_threads, 3u);
  MW_EXPECT_FALSE(ApplyNpuConfigOverride("cpu_threads=x", &config).ok());
  MW_ASSERT_OK(ApplyNpuConfigOverride("onnx_library=/opt/lib/x.so", &config));
  MW_EXPECT_EQ(config.onnx_library, std::string("/opt/lib/x.so"));
  config.onnx_library.clear();
  MW_EXPECT_FALSE(ValidateNpuConfig(config).ok());
  config = NpuConfig();
  MW_EXPECT_EQ(config.runtime, std::string("cpu"));  // New default.
}

}  // namespace mw
