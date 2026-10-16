// Copyright 2026 Genio Middleware Authors.

#include "mw/npu/cpu_runtime.h"

#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#define DEBUG 0
#if DEBUG
#include <sys/stat.h>

#include <atomic>
#endif  // DEBUG

#include <memory>
#include <string>
#include <vector>

#include "mw/common/logging.h"
#include "onnxruntime_c_api.h"

namespace mw {
namespace {

#if DEBUG
constexpr int kMaxDumpFrames = 20;
constexpr char kDumpDir[] = "./dump_input";

inline uint8_t Clamp8(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

int NextDumpIndex() {
  static std::atomic<int> counter{0};
  if (counter.load() >= kMaxDumpFrames) {
    return -1;
  }
  const int idx = counter.fetch_add(1);
  if (idx >= kMaxDumpFrames) {
    return -1;
  }
  if (idx == 0) {
    mkdir(kDumpDir, 0755);
  }
  return idx;
}

bool WriteBytes(const std::string& path, const void* data, size_t size,
                const char* header = nullptr) {
  FILE* f = fopen(path.c_str(), "wb");
  if (f == nullptr) {
    MW_LOG(Warning) << "frame dump: cannot open " << path;
    return false;
  }
  bool ok = true;
  if (header != nullptr) {
    ok = fwrite(header, 1, strlen(header), f) == strlen(header);
  }
  if (ok && size > 0) {
    ok = fwrite(data, 1, size, f) == size;
  }
  fclose(f);
  if (!ok) {
    MW_LOG(Warning) << "frame dump: short write to " << path;
  }
  return ok;
}

std::string DumpPath(int idx, const std::string& suffix) {
  char name[64];
  snprintf(name, sizeof(name), "preprocess_%03d_", idx);
  return std::string(kDumpDir) + "/" + name + suffix;
}

void DumpFrame(const uint8_t* out_bytes, int w, int h, bool as_float, bool bgr,
               float mean, float scale, int idx) {
  const size_t pixels = static_cast<size_t>(w) * h;
  const size_t elems = pixels * 3;

  std::vector<uint8_t> rgb(elems);
  const float* f = reinterpret_cast<const float*>(out_bytes);
  for (size_t p = 0; p < pixels; ++p) {
    uint8_t c[3];
    for (int k = 0; k < 3; ++k) {
      if (as_float) {
        // Undo (v - mean) * scale.
        const float v = scale != 0.0f ? f[p * 3 + k] / scale + mean : 0.0f;
        c[k] = Clamp8(static_cast<int>(v + 0.5f));
      } else {
        c[k] = out_bytes[p * 3 + k];
      }
    }
    if (bgr) {
      std::swap(c[0], c[2]);
    }
    rgb[p * 3] = c[0];
    rgb[p * 3 + 1] = c[1];
    rgb[p * 3 + 2] = c[2];
  }

  char suffix[48];
  snprintf(suffix, sizeof(suffix), "%dx%d_rgb.raw", w, h);
  const std::string path = DumpPath(idx, suffix);
  if (WriteBytes(path, rgb.data(), rgb.size())) {
    MW_LOG(Info) << "frame dump: wrote " << path;
  }
}
#endif  // DEBUG

// Oldest and newest OrtApi versions we try. Every function used below exists
// since API 14 (ONNX Runtime 1.14); newer libraries still serve older tables.
const uint32_t kMinApiVersion = 14;

std::string OrtMessage(const OrtApi* api, OrtStatus* status) {
  const char* text = api->GetErrorMessage(status);
  std::string message = text != nullptr ? text : "unknown error";
  api->ReleaseStatus(status);
  return message;
}

// Converts an OrtStatus (owned, released here) to a Status.
Status Check(const OrtApi* api, OrtStatus* status, const std::string& what) {
  if (status == nullptr) {
    return Status::Ok();
  }
  return InternalError(what + ": " + OrtMessage(api, status));
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
  uint32_t exponent = (h >> 10) & 0x1Fu;
  uint32_t mantissa = h & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {  // Subnormal half: normalise.
      exponent = 127 - 15 + 1;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3FFu;
      bits = sign | (exponent << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);  // Inf / NaN.
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

// Everything that must outlive the runtime object: the loaded library and the
// environment. Models hold a reference, so destruction order does not matter.
// The library handle is intentionally never dlclose()d: ONNX Runtime keeps
// thread pools and static state that must not be unmapped under it.
struct OrtState {
  OrtState() : api(nullptr), env(nullptr) {}
  ~OrtState() {
    if (api != nullptr && env != nullptr) {
      api->ReleaseEnv(env);
    }
  }
  const OrtApi* api;
  OrtEnv* env;
  uint32_t threads = 0;
  std::string library;
  uint32_t api_version = 0;
};

enum Layout { kNhwc, kNchw };

class CpuModel : public NpuModel {
 public:
  CpuModel(std::shared_ptr<OrtState> state, const ModelConfig& config)
      : state_(state),
        config_(config),
        spec_(InputSpecFromConfig(config)),
        session_(nullptr),
        memory_info_(nullptr),
        model_float_(true),
        layout_(kNhwc) {}

  ~CpuModel() override {
    const OrtApi* api = state_->api;
    if (memory_info_ != nullptr) {
      api->ReleaseMemoryInfo(memory_info_);
    }
    if (session_ != nullptr) {
      api->ReleaseSession(session_);
    }
  }

  const InputSpec& input_spec() const override { return spec_; }

  Status Open() {
    const OrtApi* api = state_->api;
    OrtSessionOptions* options = nullptr;
    MW_RETURN_IF_ERROR(Check(api, api->CreateSessionOptions(&options),
                             "CreateSessionOptions"));
    Status status = ConfigureOptions(options);
    if (status.ok()) {
      status = Check(api,
                     api->CreateSession(state_->env, config_.path.c_str(),
                                        options, &session_),
                     "cannot load model '" + config_.path + "'");
    }
    api->ReleaseSessionOptions(options);
    MW_RETURN_IF_ERROR(status);
    MW_RETURN_IF_ERROR(InspectModel());
    return Check(api,
                 api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                          &memory_info_),
                 "CreateCpuMemoryInfo");
  }

  Status Run(const TensorBuffer& input,
             std::vector<OutputTensor>* outputs) override {
    const OrtApi* api = state_->api;
    if (input.size() < spec_.bytes()) {
      return InvalidArgumentError("input tensor too small");
    }
    // Pick the buffer ONNX Runtime reads: the shared tensor itself whenever
    // layout and type already match (no copy), otherwise a reused scratch.
    void* data = nullptr;
    size_t bytes = 0;
    PrepareInput(input, &data, &bytes);

    OrtValue* in_value = nullptr;
    MW_RETURN_IF_ERROR(
        Check(api,
              api->CreateTensorWithDataAsOrtValue(
                  memory_info_, data, bytes, shape_.data(), shape_.size(),
                  model_float_ ? ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
                               : ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8,
                  &in_value),
              "CreateTensorWithDataAsOrtValue"));

    std::vector<const char*> out_names;
    for (size_t i = 0; i < output_names_.size(); ++i) {
      out_names.push_back(output_names_[i].c_str());
    }
    std::vector<OrtValue*> out_values(output_names_.size(), nullptr);
    const char* in_names[1] = {input_name_.c_str()};
    const OrtValue* in_values[1] = {in_value};
    Status status =
        Check(api,
              api->Run(session_, nullptr, in_names, in_values, 1,
                       out_names.data(), out_names.size(), out_values.data()),
              "inference failed");
    api->ReleaseValue(in_value);
    if (status.ok()) {
      status = CollectOutputs(out_values, outputs);
    }
    for (size_t i = 0; i < out_values.size(); ++i) {
      if (out_values[i] != nullptr) {
        api->ReleaseValue(out_values[i]);
      }
    }
    return status;
  }

 private:
  Status ConfigureOptions(OrtSessionOptions* options) {
    const OrtApi* api = state_->api;
    if (state_->threads > 0) {
      MW_RETURN_IF_ERROR(Check(
          api,
          api->SetIntraOpNumThreads(options, static_cast<int>(state_->threads)),
          "SetIntraOpNumThreads"));
    }
    MW_RETURN_IF_ERROR(Check(api, api->SetInterOpNumThreads(options, 1),
                             "SetInterOpNumThreads"));
    MW_RETURN_IF_ERROR(
        Check(api, api->SetSessionExecutionMode(options, ORT_SEQUENTIAL),
              "SetSessionExecutionMode"));
    return Check(api,
                 api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL),
                 "SetSessionGraphOptimizationLevel");
  }

  // Reads the first dimensions/type of tensor |info| (non-owning).
  Status ReadShape(const OrtTensorTypeAndShapeInfo* info,
                   ONNXTensorElementDataType* type,
                   std::vector<int64_t>* dims) {
    const OrtApi* api = state_->api;
    MW_RETURN_IF_ERROR(Check(api, api->GetTensorElementType(info, type),
                             "GetTensorElementType"));
    size_t rank = 0;
    MW_RETURN_IF_ERROR(
        Check(api, api->GetDimensionsCount(info, &rank), "GetDimensionsCount"));
    dims->assign(rank, 0);
    if (rank > 0) {
      MW_RETURN_IF_ERROR(Check(
          api, api->GetDimensions(info, dims->data(), rank), "GetDimensions"));
    }
    return Status::Ok();
  }

  Status InspectModel() {
    const OrtApi* api = state_->api;
    OrtAllocator* allocator = nullptr;
    MW_RETURN_IF_ERROR(Check(api,
                             api->GetAllocatorWithDefaultOptions(&allocator),
                             "GetAllocatorWithDefaultOptions"));
    size_t inputs = 0;
    size_t outputs = 0;
    MW_RETURN_IF_ERROR(Check(api, api->SessionGetInputCount(session_, &inputs),
                             "input count"));
    MW_RETURN_IF_ERROR(Check(
        api, api->SessionGetOutputCount(session_, &outputs), "output count"));
    if (inputs != 1) {
      return InvalidArgumentError("model '" + config_.path + "' has " +
                                  std::to_string(inputs) +
                                  " inputs; exactly one is supported");
    }
    if (outputs == 0) {
      return InvalidArgumentError("model '" + config_.path + "' has no output");
    }
    char* name = nullptr;
    MW_RETURN_IF_ERROR(
        Check(api, api->SessionGetInputName(session_, 0, allocator, &name),
              "input name"));
    input_name_ = name;
    Check(api, api->AllocatorFree(allocator, name), "AllocatorFree");
    for (size_t i = 0; i < outputs; ++i) {
      MW_RETURN_IF_ERROR(
          Check(api, api->SessionGetOutputName(session_, i, allocator, &name),
                "output name"));
      output_names_.push_back(name);
      Check(api, api->AllocatorFree(allocator, name), "AllocatorFree");
    }

    OrtTypeInfo* type_info = nullptr;
    MW_RETURN_IF_ERROR(
        Check(api, api->SessionGetInputTypeInfo(session_, 0, &type_info),
              "type info"));
    const OrtTensorTypeAndShapeInfo* tensor_info = nullptr;
    Status status =
        Check(api, api->CastTypeInfoToTensorInfo(type_info, &tensor_info),
              "input is not a tensor");
    ONNXTensorElementDataType element_type =
        ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    std::vector<int64_t> dims;
    if (status.ok() && tensor_info == nullptr) {
      status = InvalidArgumentError("model input is not a tensor");
    }
    if (status.ok()) {
      status = ReadShape(tensor_info, &element_type, &dims);
    }
    api->ReleaseTypeInfo(type_info);
    MW_RETURN_IF_ERROR(status);

    if (element_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      model_float_ = true;
    } else if (element_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8) {
      model_float_ = false;
    } else {
      return InvalidArgumentError(
          "model input type is not float32 or uint8 (got ONNX element type " +
          std::to_string(static_cast<int>(element_type)) + ")");
    }
    if (!model_float_ && spec_.type == kTensorFloat32) {
      return InvalidArgumentError(
          "model expects uint8 input but the config feeds float32; set "
          "input_type to uint8");
    }
    return config_.is_video() ? PlanImageInput(dims) : PlanAudioInput(dims);
  }

  Status PlanImageInput(const std::vector<int64_t>& dims) {
    if (dims.size() != 4) {
      return InvalidArgumentError("image model input must have rank 4");
    }
    const int64_t w = spec_.width;
    const int64_t h = spec_.height;
    if (config_.input_layout == "nhwc") {
      layout_ = kNhwc;
    } else if (config_.input_layout == "nchw") {
      layout_ = kNchw;
    } else if (dims[3] == 3) {
      layout_ = kNhwc;
    } else if (dims[1] == 3) {
      layout_ = kNchw;
    } else {
      return InvalidArgumentError(
          "cannot detect the input layout (no dimension equal to 3); set "
          "input_layout");
    }
    const int64_t mh = layout_ == kNhwc ? dims[1] : dims[2];
    const int64_t mw = layout_ == kNhwc ? dims[2] : dims[3];
    const int64_t mc = layout_ == kNhwc ? dims[3] : dims[1];
    if ((dims[0] > 1) || (mc != 3 && mc > 0) || (mh > 0 && mh != h) ||
        (mw > 0 && mw != w)) {
      return InvalidArgumentError(
          "model input shape [" + std::to_string(dims[0]) + "," +
          std::to_string(dims[1]) + "," + std::to_string(dims[2]) + "," +
          std::to_string(dims[3]) + "] does not match configured " +
          std::to_string(spec_.width) + "x" + std::to_string(spec_.height) +
          " RGB input (batch must be 1 or dynamic)");
    }
    if (layout_ == kNhwc) {
      shape_ = {1, h, w, 3};
    } else {
      shape_ = {1, 3, h, w};
    }
    return Status::Ok();
  }

  Status PlanAudioInput(const std::vector<int64_t>& dims) {
    const int64_t samples = spec_.width;
    if (dims.size() == 2) {
      if ((dims[0] > 1) || (dims[1] > 0 && dims[1] != samples)) {
        return InvalidArgumentError("speaker model input must be [1, " +
                                    std::to_string(samples) +
                                    "] to match sample_rate * window_ms");
      }
      shape_ = {1, samples};
    } else if (dims.size() == 3) {
      if ((dims[0] > 1) || (dims[1] > 1) ||
          (dims[2] > 0 && dims[2] != samples)) {
        return InvalidArgumentError("speaker model input must be [1, 1, " +
                                    std::to_string(samples) +
                                    "] to match sample_rate * window_ms");
      }
      shape_ = {1, 1, samples};
    } else {
      return InvalidArgumentError("speaker model input must have rank 2 or 3");
    }
    if (!model_float_) {
      return InvalidArgumentError("speaker model input must be float32");
    }
    return Status::Ok();
  }

  void PrepareInput(const TensorBuffer& input, void** data, size_t* bytes) {
    const bool need_layout = config_.is_video() && layout_ == kNchw;
    const bool need_cast = model_float_ && spec_.type == kTensorUint8;
    if (!need_layout && !need_cast) {
      *data = input.data();
      *bytes = spec_.bytes();
      return;
    }
    const size_t h = spec_.height;
    const size_t w = spec_.width;
    const size_t count = w * h * 3;
    if (model_float_) {
      scratch_.resize(count * sizeof(float));
    } else {
      scratch_.resize(count);
    }
    const uint8_t* src8 = input.data();
    const float* srcf = reinterpret_cast<const float*>(input.data());
    float* dstf = reinterpret_cast<float*>(scratch_.data());
    uint8_t* dst8 = scratch_.data();
    const bool src_float = spec_.type == kTensorFloat32;
    for (size_t y = 0; y < h; ++y) {
      for (size_t x = 0; x < w; ++x) {
        for (size_t c = 0; c < 3; ++c) {
          const size_t s = (y * w + x) * 3 + c;
          const size_t d = need_layout ? (c * h + y) * w + x : s;
          if (model_float_) {
            dstf[d] = src_float ? srcf[s] : static_cast<float>(src8[s]);
          } else {
            dst8[d] = src8[s];
          }
        }
      }
    }
    *data = scratch_.data();
    *bytes = scratch_.size();
#if DEBUG
    const int dump_idx = NextDumpIndex();
    if (dump_idx >= 0) {
      DumpFrame((reinterpret_cast<uint8_t*>)*data, w, h, model_float_, true,
                config_.input_mean, config_.input_scale, dump_idx);
    }
#endif  // DEBUG
  }

  Status CollectOutputs(const std::vector<OrtValue*>& values,
                        std::vector<OutputTensor>* outputs) {
    const OrtApi* api = state_->api;
    outputs->clear();
    for (size_t i = 0; i < values.size(); ++i) {
      OrtTensorTypeAndShapeInfo* info = nullptr;
      MW_RETURN_IF_ERROR(Check(
          api, api->GetTensorTypeAndShape(values[i], &info), "output shape"));
      ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
      std::vector<int64_t> dims;
      size_t count = 0;
      Status status = ReadShape(info, &type, &dims);
      if (status.ok()) {
        status = Check(api, api->GetTensorShapeElementCount(info, &count),
                       "output element count");
      }
      api->ReleaseTensorTypeAndShapeInfo(info);
      MW_RETURN_IF_ERROR(status);

      void* raw = nullptr;
      MW_RETURN_IF_ERROR(Check(api, api->GetTensorMutableData(values[i], &raw),
                               "output data"));
      OutputTensor out;
      for (size_t d = 0; d < dims.size(); ++d) {
        out.dims.push_back(static_cast<int>(dims[d]));
      }
      out.data.resize(count);
      MW_RETURN_IF_ERROR(ConvertToFloat(type, raw, count, &out.data));
      outputs->push_back(out);
    }
    return Status::Ok();
  }

  static Status ConvertToFloat(ONNXTensorElementDataType type, const void* raw,
                               size_t count, std::vector<float>* out) {
    switch (type) {
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
        if (count > 0) {
          memcpy(out->data(), raw, count * sizeof(float));
        }
        return Status::Ok();
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: {
        const uint16_t* p = static_cast<const uint16_t*>(raw);
        for (size_t i = 0; i < count; ++i) {
          (*out)[i] = HalfToFloat(p[i]);
        }
        return Status::Ok();
      }
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE: {
        const double* p = static_cast<const double*>(raw);
        for (size_t i = 0; i < count; ++i) {
          (*out)[i] = static_cast<float>(p[i]);
        }
        return Status::Ok();
      }
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: {
        const int32_t* p = static_cast<const int32_t*>(raw);
        for (size_t i = 0; i < count; ++i) {
          (*out)[i] = static_cast<float>(p[i]);
        }
        return Status::Ok();
      }
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: {
        const int64_t* p = static_cast<const int64_t*>(raw);
        for (size_t i = 0; i < count; ++i) {
          (*out)[i] = static_cast<float>(p[i]);
        }
        return Status::Ok();
      }
      default:
        return InvalidArgumentError("unsupported output element type " +
                                    std::to_string(static_cast<int>(type)) +
                                    " (need float32/16/64 or int32/64)");
    }
  }

  std::shared_ptr<OrtState> state_;
  ModelConfig config_;
  InputSpec spec_;
  OrtSession* session_;
  OrtMemoryInfo* memory_info_;
  std::string input_name_;
  std::vector<std::string> output_names_;
  bool model_float_;
  Layout layout_;
  std::vector<int64_t> shape_;
  std::vector<uint8_t> scratch_;
};

class CpuRuntime : public NpuRuntime {
 public:
  explicit CpuRuntime(std::shared_ptr<OrtState> state) : state_(state) {}

  Status Load(const ModelConfig& config,
              std::unique_ptr<NpuModel>* model) override {
    std::unique_ptr<CpuModel> loaded(new CpuModel(state_, config));
    MW_RETURN_IF_ERROR(loaded->Open());
    model->reset(loaded.release());
    return Status::Ok();
  }

  std::string Describe() const override {
    std::string text = "cpu (ONNX Runtime API " +
                       std::to_string(state_->api_version) + " from " +
                       state_->library + ", ";
    text += state_->threads > 0 ? std::to_string(state_->threads) + " threads)"
                                : "default threads)";
    return text;
  }

 private:
  std::shared_ptr<OrtState> state_;
};

}  // namespace

Status CreateCpuRuntime(const NpuConfig& config,
                        std::unique_ptr<NpuRuntime>* out) {
  out->reset();
  void* handle = dlopen(config.onnx_library.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* reason = dlerror();
    return UnavailableError("cannot load ONNX Runtime '" + config.onnx_library +
                            "': " + (reason != nullptr ? reason : "unknown") +
                            " (set onnx_library to the full path of "
                            "libonnxruntime.so)");
  }
  typedef const OrtApiBase* (*GetApiBaseFn)(void);
  GetApiBaseFn get_base =
      reinterpret_cast<GetApiBaseFn>(dlsym(handle, "OrtGetApiBase"));
  if (get_base == nullptr) {
    return InvalidArgumentError(config.onnx_library +
                                " does not export OrtGetApiBase; is it really "
                                "ONNX Runtime?");
  }
  const OrtApiBase* base = get_base();
  std::shared_ptr<OrtState> state(new OrtState());
  for (uint32_t version = ORT_API_VERSION; version >= kMinApiVersion;
       --version) {
    state->api = base->GetApi(version);
    if (state->api != nullptr) {
      state->api_version = version;
      break;
    }
  }
  if (state->api == nullptr) {
    return UnavailableError(
        "ONNX Runtime " + std::string(base->GetVersionString()) +
        " is too old: the C API version " + std::to_string(kMinApiVersion) +
        " (ONNX Runtime 1.14) or newer is required");
  }
  state->library = config.onnx_library;
  state->threads = config.cpu_threads;
  MW_RETURN_IF_ERROR(Check(
      state->api,
      state->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "genio-mw", &state->env),
      "CreateEnv"));
  MW_LOG(Info) << "ONNX Runtime " << base->GetVersionString() << " loaded from "
               << config.onnx_library;
  out->reset(new CpuRuntime(state));
  return Status::Ok();
}

}  // namespace mw
