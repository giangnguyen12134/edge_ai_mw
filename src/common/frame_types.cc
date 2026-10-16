// Copyright 2026 Genio Middleware Authors.

#include "mw/common/frame_types.h"

#include <ctype.h>

namespace mw {
namespace {

const uint32_t kPageSize = 4096;

std::string Upper(const std::string& s) {
  std::string out = s;
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<char>(toupper(static_cast<unsigned char>(out[i])));
  }
  return out;
}

}  // namespace

bool ParsePixelFormat(const std::string& name, uint32_t* fourcc) {
  const std::string upper = Upper(name);
  if (upper == "NV12") {
    *fourcc = kFourccNv12;
  } else if (upper == "YUYV") {
    *fourcc = kFourccYuyv;
  } else if (upper == "MJPEG" || upper == "MJPG") {
    *fourcc = kFourccMjpeg;
  } else {
    return false;
  }
  return true;
}

std::string FourccToString(uint32_t fourcc) {
  std::string s(4, ' ');
  for (int i = 0; i < 4; ++i) {
    s[static_cast<size_t>(i)] = static_cast<char>((fourcc >> (8 * i)) & 0xFF);
  }
  return s;
}

FrameFormat FrameFormat::Nv12(uint32_t width, uint32_t height) {
  FrameFormat f;
  f.width = width;
  f.height = height;
  f.fourcc = kFourccNv12;
  f.stride = AlignUp(width, 16);
  f.uv_offset = f.stride * height;
  f.buffer_size =
      AlignUp(f.uv_offset + f.stride * ((height + 1) / 2), kPageSize);
  return f;
}

FrameFormat FrameFormat::FromCamera(uint32_t width, uint32_t height,
                                    uint32_t fourcc, uint32_t bytes_per_line,
                                    uint32_t size_image) {
  FrameFormat f;
  f.width = width;
  f.height = height;
  f.fourcc = fourcc;
  f.stride = bytes_per_line;
  f.uv_offset = fourcc == kFourccNv12 ? bytes_per_line * height : 0;
  f.buffer_size = AlignUp(size_image, kPageSize);
  return f;
}

Status ValidateFrameFormat(const FrameFormat& f) {
  if (f.width == 0 || f.height == 0 || f.buffer_size == 0) {
    return InvalidArgumentError("frame format has a zero dimension or size");
  }
  // 64-bit arithmetic: values may come from an untrusted peer.
  const uint64_t stride = f.stride;
  const uint64_t height = f.height;
  if (f.fourcc == kFourccNv12) {
    if (stride < f.width || f.uv_offset < stride * height ||
        static_cast<uint64_t>(f.uv_offset) + stride * ((height + 1) / 2) >
            f.buffer_size) {
      return InvalidArgumentError("inconsistent NV12 layout");
    }
  } else if (f.fourcc == kFourccYuyv) {
    if (stride < 2ull * f.width || stride * height > f.buffer_size) {
      return InvalidArgumentError("inconsistent YUYV layout");
    }
  } else if (f.fourcc != kFourccMjpeg) {
    return InvalidArgumentError("unsupported pixel format '" +
                                FourccToString(f.fourcc) + "'");
  }
  return Status::Ok();
}

}  // namespace mw
