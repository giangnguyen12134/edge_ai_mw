// Copyright 2026 Genio Middleware Authors.

#include "mw/npu/image_scaler.h"

#include "mw/common/logging.h"

#define DEBUG 0
#if DEBUG
#include <sys/stat.h>

#include <atomic>
#include <memory>
#endif  // DEBUG

#include <string.h>

#include <algorithm>
#include <vector>

namespace mw {
namespace {

const uint8_t kPadValue = 114;  // Conventional letterbox grey.

inline uint8_t Clamp8(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// BT.601 limited range YUV -> RGB, 8-bit fixed point.
inline void YuvToRgb(int y, int u, int v, uint8_t* r, uint8_t* g, uint8_t* b) {
  const int c = y - 16;
  const int d = u - 128;
  const int e = v - 128;
  *r = Clamp8((298 * c + 409 * e + 128) >> 8);
  *g = Clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
  *b = Clamp8((298 * c + 516 * d + 128) >> 8);
}

#if DEBUG
constexpr int kMaxDumpFrames = 20;
constexpr char kDumpDir[] = "./dump_input";

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
  snprintf(name, sizeof(name), "frame_%03d_", idx);
  return std::string(kDumpDir) + "/" + name + suffix;
}

void DumpSourceFrame(const SourceImage& src, bool yuyv, int idx) {
  const size_t stride = src.format.stride;
  const size_t h = src.format.height;
  size_t size;
  if (yuyv) {
    size = stride * h;
  } else {
    // NV12: Y plane (uv_offset bytes) followed by UV plane, h/2 rows.
    size = static_cast<size_t>(src.format.uv_offset) + stride * ((h + 1) / 2);
  }
  char suffix[96];
  snprintf(suffix, sizeof(suffix), "src_%ux%u_stride%u_%s.yuv",
           static_cast<unsigned>(src.format.width),
           static_cast<unsigned>(src.format.height),
           static_cast<unsigned>(src.format.stride), yuyv ? "yuyv" : "nv12");
  const std::string path = DumpPath(idx, suffix);
  if (WriteBytes(path, src.data, size)) {
    MW_LOG(Info) << "frame dump: wrote " << path << " (" << size << " bytes, "
                 << "uv_offset=" << src.format.uv_offset << ")";
  }
}

void DumpOutputFrame(const uint8_t* out_bytes, int w, int h, bool as_float,
                     bool bgr, float mean, float scale, int idx) {
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
  snprintf(suffix, sizeof(suffix), "dst_%dx%d_rgb.raw", w, h);
  const std::string path = DumpPath(idx, suffix);
  if (WriteBytes(path, rgb.data(), rgb.size())) {
    MW_LOG(Info) << "frame dump: wrote " << path;
  }
}
#endif  // DEBUG

struct AxisMap {
  int i0;
  int i1;
  int w1;  // Weight of i1 in 0..256.
};

// Bilinear sample positions for |count| output samples drawn from |src_size|.
void BuildAxis(int count, int src_size, std::vector<AxisMap>* map) {
  map->resize(static_cast<size_t>(count));
  const float scale = static_cast<float>(src_size) / static_cast<float>(count);
  for (int i = 0; i < count; ++i) {
    float f = (static_cast<float>(i) + 0.5f) * scale - 0.5f;
    if (f < 0) {
      f = 0;
    }
    int i0 = static_cast<int>(f);
    if (i0 > src_size - 1) {
      i0 = src_size - 1;
    }
    const int i1 = std::min(i0 + 1, src_size - 1);
    AxisMap m;
    m.i0 = i0;
    m.i1 = i1;
    m.w1 = static_cast<int>((f - static_cast<float>(i0)) * 256.0f);
    (*map)[static_cast<size_t>(i)] = m;
  }
}

class CpuImageScaler : public ImageScaler {
 public:
  Status Convert(const SourceImage& src, const ModelConfig& config,
                 const InputSpec& spec, TensorBuffer* dst,
                 LetterboxInfo* geometry) override {
    if (src.data == nullptr) {
      return InvalidArgumentError("cpu scaler needs a CPU view of the frame");
    }
    const bool yuyv = src.format.fourcc == kFourccYuyv;
    if ((src.format.fourcc != kFourccNv12 && !yuyv) || src.format.width < 2 ||
        src.format.height < 2 || !ValidateFrameFormat(src.format).ok()) {
      return InvalidArgumentError(
          "cpu scaler needs a valid NV12 or YUYV frame");
    }
    if (dst->size() < spec.bytes() || spec.channels != 3) {
      return InvalidArgumentError("destination tensor too small");
    }
    const int in_w = static_cast<int>(spec.width);
    const int in_h = static_cast<int>(spec.height);
    const int src_w = static_cast<int>(src.format.width);
    const int src_h = static_cast<int>(src.format.height);

#if DEBUG
    const int dump_idx = NextDumpIndex();
    if (dump_idx >= 0) {
      DumpSourceFrame(src, yuyv, dump_idx);
    }
#endif
    int content_w = in_w;
    int content_h = in_h;
    int pad_x = 0;
    int pad_y = 0;
    if (config.resize_mode == "letterbox") {
      const float scale =
          std::min(static_cast<float>(in_w) / static_cast<float>(src_w),
                   static_cast<float>(in_h) / static_cast<float>(src_h));
      content_w = std::max(
          2, static_cast<int>(static_cast<float>(src_w) * scale + 0.5f));
      content_h = std::max(
          2, static_cast<int>(static_cast<float>(src_h) * scale + 0.5f));
      content_w = std::min(content_w, in_w);
      content_h = std::min(content_h, in_h);
      pad_x = (in_w - content_w) / 2;
      pad_y = (in_h - content_h) / 2;
    }
    geometry->pad_x = static_cast<float>(pad_x);
    geometry->pad_y = static_cast<float>(pad_y);
    geometry->content_w = static_cast<float>(content_w);
    geometry->content_h = static_cast<float>(content_h);
    geometry->input_w = static_cast<float>(in_w);
    geometry->input_h = static_cast<float>(in_h);

    x_map_.clear();
    y_map_.clear();
    BuildAxis(content_w, src_w, &x_map_);
    BuildAxis(content_h, src_h, &y_map_);

    const bool bgr = config.color_order == "bgr";
    const bool as_float = spec.type == kTensorFloat32;
    const float mean = config.input_mean;
    const float scale = config.input_scale;
    const int ro = bgr ? 2 : 0;
    const int bo = bgr ? 0 : 2;

    // NV12: Y plane, then interleaved UV at half vertical resolution.
    // YUYV: packed rows of Y0 U Y1 V, chroma shared by two pixels of a row.
    const uint8_t* y_plane = src.data;
    const uint8_t* uv_plane = src.data + src.format.uv_offset;
    const int stride = static_cast<int>(src.format.stride);
    const int luma_step = yuyv ? 2 : 1;  // Bytes between luma samples.

    dst->BeginCpuWrite();
    uint8_t* out_bytes = dst->data();
    // Fill padding first (letterbox only).
    if (pad_x != 0 || pad_y != 0 || content_w != in_w || content_h != in_h) {
      FillPad(out_bytes, in_w, in_h, as_float, mean, scale);
    }
    for (int y = 0; y < content_h; ++y) {
      const AxisMap& ym = y_map_[static_cast<size_t>(y)];
      const uint8_t* row0 = y_plane + static_cast<size_t>(ym.i0) * stride;
      const uint8_t* row1 = y_plane + static_cast<size_t>(ym.i1) * stride;
      const int near_y = ym.w1 < 128 ? ym.i0 : ym.i1;
      const uint8_t* chroma_row =
          yuyv ? y_plane + static_cast<size_t>(near_y) * stride
               : uv_plane + static_cast<size_t>(near_y / 2) * stride;
      const size_t out_row = static_cast<size_t>(y + pad_y) * in_w + pad_x;
      for (int x = 0; x < content_w; ++x) {
        const AxisMap& xm = x_map_[static_cast<size_t>(x)];
        const int l0 = xm.i0 * luma_step;
        const int l1 = xm.i1 * luma_step;
        const int top = row0[l0] * (256 - xm.w1) + row0[l1] * xm.w1;
        const int bottom = row1[l0] * (256 - xm.w1) + row1[l1] * xm.w1;
        const int luma = (top * (256 - ym.w1) + bottom * ym.w1) >> 16;
        const int near_x = (xm.w1 < 128 ? xm.i0 : xm.i1) / 2;
        const int u_at = yuyv ? near_x * 4 + 1 : near_x * 2;
        const int v_at = yuyv ? near_x * 4 + 3 : near_x * 2 + 1;
        uint8_t px[3];
        YuvToRgb(luma, chroma_row[u_at], chroma_row[v_at], &px[0], &px[1],
                 &px[2]);
        const uint8_t c0 = px[ro];
        const uint8_t c1 = px[1];
        const uint8_t c2 = px[bo];
        const size_t o = (out_row + static_cast<size_t>(x)) * 3;
        if (as_float) {
          float* f = reinterpret_cast<float*>(out_bytes) + o;
          f[0] = (static_cast<float>(c0) - mean) * scale;
          f[1] = (static_cast<float>(c1) - mean) * scale;
          f[2] = (static_cast<float>(c2) - mean) * scale;
        } else {
          out_bytes[o] = c0;
          out_bytes[o + 1] = c1;
          out_bytes[o + 2] = c2;
        }
      }
    }
#if DEBUG
    if (dump_idx >= 0) {
      DumpOutputFrame(out_bytes, in_w, in_h, as_float, bgr, mean, scale,
                      dump_idx);
    }
#endif
    dst->EndCpuWrite();
    return Status::Ok();
  }

  std::string Describe() const override {
    return "cpu (bilinear NV12/YUYV to RGB)";
  }

 private:
  static void FillPad(uint8_t* out, int w, int h, bool as_float, float mean,
                      float scale) {
    const size_t count = static_cast<size_t>(w) * h * 3;
    if (as_float) {
      float* f = reinterpret_cast<float*>(out);
      const float v = (static_cast<float>(kPadValue) - mean) * scale;
      for (size_t i = 0; i < count; ++i) {
        f[i] = v;
      }
    } else {
      memset(out, kPadValue, count);
    }
  }

  std::vector<AxisMap> x_map_;
  std::vector<AxisMap> y_map_;
};

}  // namespace

Status CreateImageScaler(const std::string& name,
                         std::unique_ptr<ImageScaler>* out) {
  if (name == "cpu") {
    out->reset(new CpuImageScaler());
    return Status::Ok();
  }
  return InvalidArgumentError("unknown image scaler '" + name + "'");
}

}  // namespace mw
