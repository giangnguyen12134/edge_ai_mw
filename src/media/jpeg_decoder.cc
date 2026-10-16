// Copyright 2026 VMX Middleware Team.

#include "mw/media/jpeg_decoder.h"

#ifdef MW_HAVE_JPEG
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <jpeglib.h>
}
#endif

namespace mw {

#ifdef MW_HAVE_JPEG
namespace {

// libjpeg reports fatal errors through longjmp. Only plain C data lives
// between setjmp and longjmp; |scratch| is freed by the error path.
struct ErrorManager {
  struct jpeg_error_mgr pub;
  jmp_buf jump;
  void* scratch;
  char message[JMSG_LENGTH_MAX];
};

void ErrorExit(j_common_ptr cinfo) {
  ErrorManager* err = reinterpret_cast<ErrorManager*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, err->message);
  longjmp(err->jump, 1);
}

void SilentMessage(j_common_ptr /*cinfo*/, int /*level*/) {}

// Returns 0 on success, 1 for a size mismatch. Fatal libjpeg errors longjmp
// out of this function.
int DecodeImpl(struct jpeg_decompress_struct* cinfo, ErrorManager* err,
               const uint8_t* jpeg, size_t size, uint8_t* dst,
               const FrameFormat& format) {
  jpeg_mem_src(cinfo, const_cast<unsigned char*>(jpeg),
               static_cast<unsigned long>(size));
  jpeg_read_header(cinfo, TRUE);
  if (cinfo->image_width != format.width ||
      cinfo->image_height != format.height) {
    return 1;
  }
  cinfo->out_color_space = JCS_YCbCr;
  jpeg_start_decompress(cinfo);

  const size_t row_bytes = static_cast<size_t>(format.width) * 3;
  err->scratch = malloc(row_bytes * 2);
  if (err->scratch == nullptr) {
    snprintf(err->message, sizeof(err->message), "out of memory");
    longjmp(err->jump, 1);
  }
  uint8_t* row0 = static_cast<uint8_t*>(err->scratch);
  uint8_t* row1 = row0 + row_bytes;
  uint8_t* uv_plane = dst + format.uv_offset;

  for (uint32_t y = 0; y + 1 < format.height; y += 2) {
    JSAMPROW rows[1];
    rows[0] = row0;
    jpeg_read_scanlines(cinfo, rows, 1);
    rows[0] = row1;
    jpeg_read_scanlines(cinfo, rows, 1);
    uint8_t* y0 = dst + static_cast<size_t>(y) * format.stride;
    uint8_t* y1 = y0 + format.stride;
    uint8_t* uv = uv_plane + static_cast<size_t>(y / 2) * format.stride;
    for (uint32_t x = 0; x < format.width; x += 2) {
      const uint8_t* a = row0 + static_cast<size_t>(x) * 3;
      const uint8_t* b = a + 3;
      const uint8_t* c = row1 + static_cast<size_t>(x) * 3;
      const uint8_t* d = c + 3;
      y0[x] = a[0];
      y0[x + 1] = b[0];
      y1[x] = c[0];
      y1[x + 1] = d[0];
      uv[x] = static_cast<uint8_t>((a[1] + b[1] + c[1] + d[1] + 2) / 4);
      uv[x + 1] = static_cast<uint8_t>((a[2] + b[2] + c[2] + d[2] + 2) / 4);
    }
  }
  jpeg_finish_decompress(cinfo);
  return 0;
}

class LibJpegDecoder : public JpegDecoder {
 public:
  Status DecodeToNv12(const uint8_t* jpeg, size_t size, uint8_t* dst,
                      const FrameFormat& format) override {
    if (size < 4) {
      return InvalidArgumentError("jpeg frame too small");
    }
    if ((format.width % 2) != 0 || (format.height % 2) != 0) {
      return InvalidArgumentError("jpeg decode needs even dimensions");
    }
    struct jpeg_decompress_struct cinfo;
    ErrorManager err;
    err.scratch = nullptr;
    err.message[0] = '\0';
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = ErrorExit;
    err.pub.emit_message = SilentMessage;
    if (setjmp(err.jump)) {
      free(err.scratch);
      jpeg_destroy_decompress(&cinfo);
      return InternalError(std::string("jpeg decode failed: ") + err.message);
    }
    jpeg_create_decompress(&cinfo);
    const int result = DecodeImpl(&cinfo, &err, jpeg, size, dst, format);
    free(err.scratch);
    jpeg_destroy_decompress(&cinfo);
    if (result != 0) {
      return InvalidArgumentError("jpeg size differs from configured size");
    }
    return Status::Ok();
  }

  std::string Describe() const override { return "libjpeg (software)"; }
};

}  // namespace
#endif  // MW_HAVE_JPEG

Status CreateJpegDecoder(const std::string& name,
                         std::unique_ptr<JpegDecoder>* out) {
  if (name == "libjpeg") {
#ifdef MW_HAVE_JPEG
    out->reset(new LibJpegDecoder());
    return Status::Ok();
#else
    (void)out;
    return UnimplementedError(
        "built without libjpeg; reconfigure with -DMW_ENABLE_JPEG=ON");
#endif
  }
  if (name == "v4l2_m2m") {
    return UnimplementedError(
        "the v4l2_m2m hardware JPEG decoder backend is not implemented yet; "
        "see docs/PORTING.md");
  }
  return InvalidArgumentError("unknown jpeg decoder '" + name + "'");
}

}  // namespace mw
