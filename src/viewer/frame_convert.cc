// Copyright 2026 Genio Middleware Authors.

#include "mw/viewer/frame_convert.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

namespace mw {
namespace {

inline uint8_t Clamp8(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// BT.601 limited range, 8-bit fixed point; writes B, G, R, X.
inline void PutPixel(int y, int u, int v, uint8_t* out) {
  const int c = y - 16;
  const int d = u - 128;
  const int e = v - 128;
  out[0] = Clamp8((298 * c + 516 * d + 128) >> 8);
  out[1] = Clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
  out[2] = Clamp8((298 * c + 409 * e + 128) >> 8);
  out[3] = 255;
}

Status ConvertNv12(const uint8_t* data, const FrameFormat& f, BgraImage* out) {
  const uint8_t* uv_plane = data + f.uv_offset;
  for (uint32_t y = 0; y < f.height; ++y) {
    const uint8_t* luma = data + static_cast<size_t>(y) * f.stride;
    const uint8_t* chroma = uv_plane + static_cast<size_t>(y / 2) * f.stride;
    uint8_t* dst = out->pixels.data() + static_cast<size_t>(y) * out->stride();
    for (uint32_t x = 0; x < f.width; ++x) {
      const uint32_t c = (x / 2) * 2;
      PutPixel(luma[x], chroma[c], chroma[c + 1], dst + 4 * x);
    }
  }
  return Status::Ok();
}

Status ConvertYuyv(const uint8_t* data, const FrameFormat& f, BgraImage* out) {
  for (uint32_t y = 0; y < f.height; ++y) {
    const uint8_t* row = data + static_cast<size_t>(y) * f.stride;
    uint8_t* dst = out->pixels.data() + static_cast<size_t>(y) * out->stride();
    for (uint32_t x = 0; x < f.width; ++x) {
      const uint32_t pair = (x / 2) * 4;  // Y0 U Y1 V
      const int luma = row[2 * x];
      PutPixel(luma, row[pair + 1], row[pair + 3], dst + 4 * x);
    }
  }
  return Status::Ok();
}

Status ConvertMjpeg(const uint8_t* data, size_t bytes, BgraImage* out) {
  GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("jpeg", nullptr);
  if (loader == nullptr) {
    return UnavailableError("gdk-pixbuf has no JPEG loader");
  }
  GError* error = nullptr;
  const bool written = gdk_pixbuf_loader_write(loader, data, bytes, &error);
  const bool closed =
      gdk_pixbuf_loader_close(loader, written ? &error : nullptr);
  GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);  // Not owned.
  Status status = Status::Ok();
  if (!written || !closed || pixbuf == nullptr) {
    status = InvalidArgumentError(
        std::string("cannot decode MJPG frame: ") +
        (error != nullptr ? error->message : "no picture"));
  } else {
    const int w = gdk_pixbuf_get_width(pixbuf);
    const int h = gdk_pixbuf_get_height(pixbuf);
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const int stride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar* src = gdk_pixbuf_read_pixels(pixbuf);
    if (w <= 0 || h <= 0 || channels < 3) {
      status = InvalidArgumentError("unexpected MJPG pixel layout");
    } else {
      out->width = static_cast<uint32_t>(w);
      out->height = static_cast<uint32_t>(h);
      out->pixels.resize(static_cast<size_t>(w) * h * 4);
      for (int y = 0; y < h; ++y) {
        const guchar* s = src + static_cast<size_t>(y) * stride;
        uint8_t* d = out->pixels.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
          d[4 * x] = s[channels * x + 2];
          d[4 * x + 1] = s[channels * x + 1];
          d[4 * x + 2] = s[channels * x];
          d[4 * x + 3] = 255;
        }
      }
    }
  }
  if (error != nullptr) {
    g_error_free(error);
  }
  g_object_unref(loader);
  return status;
}

}  // namespace

Status ConvertFrameToBgra(const uint8_t* data, size_t bytes_used,
                          const FrameFormat& format, BgraImage* out) {
  if (data == nullptr) {
    return InvalidArgumentError("no CPU view of the frame");
  }
  MW_RETURN_IF_ERROR(ValidateFrameFormat(format));
  if (bytes_used > format.buffer_size) {
    return InvalidArgumentError("bytes_used exceeds the buffer");
  }
  if (format.fourcc == kFourccMjpeg) {
    return ConvertMjpeg(data, bytes_used, out);
  }
  out->width = format.width;
  out->height = format.height;
  out->pixels.resize(static_cast<size_t>(format.width) * format.height * 4);
  if (format.fourcc == kFourccNv12) {
    return ConvertNv12(data, format, out);
  }
  return ConvertYuyv(data, format, out);
}

}  // namespace mw
