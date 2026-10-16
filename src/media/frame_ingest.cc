// Copyright 2026 VMX Middleware Team.

#include "mw/media/frame_ingest.h"

#include <string.h>

#include "mw/media/jpeg_decoder.h"

namespace mw {
namespace {

Status CheckDestination(const CapturedFrame& src, const FrameFormat& dst) {
  if (dst.fourcc != kFourccNv12) {
    return InvalidArgumentError("pool format must be NV12");
  }
  if (src.width != dst.width || src.height != dst.height) {
    return InvalidArgumentError("source and pool dimensions differ");
  }
  if ((dst.width % 2) != 0 || (dst.height % 2) != 0) {
    return InvalidArgumentError("NV12 needs even dimensions");
  }
  return Status::Ok();
}

// NV12 camera output: row-by-row copy so differing strides are handled.
class Nv12Ingest : public FrameIngest {
 public:
  Status Convert(const CapturedFrame& src, const FrameFormat& dst_format,
                 uint8_t* dst) override {
    MW_RETURN_IF_ERROR(CheckDestination(src, dst_format));
    const uint32_t src_stride = src.stride != 0 ? src.stride : src.width;
    const size_t needed = static_cast<size_t>(src_stride) * src.height * 3 / 2;
    if (src.bytes_used < needed) {
      return InvalidArgumentError("short NV12 frame");
    }
    const uint32_t row_bytes = src.width;
    const uint8_t* src_y = src.data;
    const uint8_t* src_uv =
        src.data + static_cast<size_t>(src_stride) * src.height;
    uint8_t* dst_uv = dst + dst_format.uv_offset;
    for (uint32_t y = 0; y < src.height; ++y) {
      memcpy(dst + static_cast<size_t>(y) * dst_format.stride,
             src_y + static_cast<size_t>(y) * src_stride, row_bytes);
    }
    for (uint32_t y = 0; y < src.height / 2; ++y) {
      memcpy(dst_uv + static_cast<size_t>(y) * dst_format.stride,
             src_uv + static_cast<size_t>(y) * src_stride, row_bytes);
    }
    return Status::Ok();
  }

  std::string Describe() const override { return "NV12 copy (cpu)"; }
};

// YUYV (YUY2) 4:2:2 to NV12 4:2:0: chroma of two rows is averaged.
class YuyvIngest : public FrameIngest {
 public:
  Status Convert(const CapturedFrame& src, const FrameFormat& dst_format,
                 uint8_t* dst) override {
    MW_RETURN_IF_ERROR(CheckDestination(src, dst_format));
    const uint32_t src_stride = src.stride != 0 ? src.stride : src.width * 2;
    if (src.bytes_used < static_cast<size_t>(src_stride) * src.height) {
      return InvalidArgumentError("short YUYV frame");
    }
    uint8_t* dst_uv = dst + dst_format.uv_offset;
    for (uint32_t y = 0; y < src.height; y += 2) {
      const uint8_t* s0 = src.data + static_cast<size_t>(y) * src_stride;
      const uint8_t* s1 = s0 + src_stride;
      uint8_t* d0 = dst + static_cast<size_t>(y) * dst_format.stride;
      uint8_t* d1 = d0 + dst_format.stride;
      uint8_t* duv = dst_uv + static_cast<size_t>(y / 2) * dst_format.stride;
      for (uint32_t x = 0; x < src.width; x += 2) {
        const uint8_t* p0 = s0 + static_cast<size_t>(x) * 2;
        const uint8_t* p1 = s1 + static_cast<size_t>(x) * 2;
        d0[x] = p0[0];
        d0[x + 1] = p0[2];
        d1[x] = p1[0];
        d1[x + 1] = p1[2];
        duv[x] = static_cast<uint8_t>((p0[1] + p1[1] + 1) / 2);
        duv[x + 1] = static_cast<uint8_t>((p0[3] + p1[3] + 1) / 2);
      }
    }
    return Status::Ok();
  }

  std::string Describe() const override { return "YUYV to NV12 (cpu)"; }
};

class MjpegIngest : public FrameIngest {
 public:
  explicit MjpegIngest(std::unique_ptr<JpegDecoder> decoder)
      : decoder_(std::move(decoder)) {}

  Status Convert(const CapturedFrame& src, const FrameFormat& dst_format,
                 uint8_t* dst) override {
    MW_RETURN_IF_ERROR(CheckDestination(src, dst_format));
    return decoder_->DecodeToNv12(src.data, src.bytes_used, dst, dst_format);
  }

  std::string Describe() const override {
    return "MJPEG decode via " + decoder_->Describe();
  }

 private:
  std::unique_ptr<JpegDecoder> decoder_;
};

}  // namespace

Status CreateFrameIngest(uint32_t source_fourcc, const std::string& mode,
                         std::unique_ptr<FrameIngest>* out) {
  if (mode != "cpu" && mode != "auto") {
    return InvalidArgumentError("unknown ingest mode '" + mode + "'");
  }
  if (source_fourcc == kFourccNv12) {
    out->reset(new Nv12Ingest());
    return Status::Ok();
  }
  if (source_fourcc == kFourccYuyv) {
    out->reset(new YuyvIngest());
    return Status::Ok();
  }
  if (source_fourcc == kFourccMjpeg) {
    std::unique_ptr<JpegDecoder> decoder;
    MW_RETURN_IF_ERROR(CreateJpegDecoder("libjpeg", &decoder));
    out->reset(new MjpegIngest(std::move(decoder)));
    return Status::Ok();
  }
  return InvalidArgumentError("unsupported camera pixel format " +
                              FourccToString(source_fourcc));
}

}  // namespace mw
