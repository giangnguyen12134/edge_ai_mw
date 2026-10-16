// Copyright 2026 Genio Middleware Authors.

#include "mw/robotic/jpeg_encoder.h"

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

// libjpeg reports fatal errors through longjmp, so only plain C data is kept
// between setjmp and longjmp; the error path frees |scratch| and |output|.
struct ErrorManager {
  struct jpeg_error_mgr pub;
  jmp_buf jump;
  void* scratch;
  unsigned char* output;
  char message[JMSG_LENGTH_MAX];
};

void ErrorExit(j_common_ptr cinfo) {
  ErrorManager* err = reinterpret_cast<ErrorManager*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, err->message);
  longjmp(err->jump, 1);
}

void SilentMessage(j_common_ptr /*cinfo*/, int /*level*/) {}

void EncodeImpl(struct jpeg_compress_struct* cinfo, ErrorManager* err,
                const uint8_t* nv12, const FrameFormat& format, int quality,
                unsigned long* output_size) {
  jpeg_mem_dest(cinfo, &err->output, output_size);
  cinfo->image_width = format.width;
  cinfo->image_height = format.height;
  cinfo->input_components = 3;
  cinfo->in_color_space = JCS_YCbCr;
  jpeg_set_defaults(cinfo);
  jpeg_set_quality(cinfo, quality, TRUE);
  jpeg_start_compress(cinfo, TRUE);

  const size_t row_bytes = static_cast<size_t>(format.width) * 3;
  err->scratch = malloc(row_bytes);
  if (err->scratch == nullptr) {
    snprintf(err->message, sizeof(err->message), "out of memory");
    longjmp(err->jump, 1);
  }
  uint8_t* row = static_cast<uint8_t*>(err->scratch);
  const uint8_t* uv_plane = nv12 + format.uv_offset;
  for (uint32_t y = 0; y < format.height; ++y) {
    const uint8_t* luma = nv12 + static_cast<size_t>(y) * format.stride;
    const uint8_t* chroma =
        uv_plane + static_cast<size_t>(y / 2) * format.stride;
    for (uint32_t x = 0; x < format.width; ++x) {
      row[x * 3] = luma[x];
      row[x * 3 + 1] = chroma[(x / 2) * 2];
      row[x * 3 + 2] = chroma[(x / 2) * 2 + 1];
    }
    JSAMPROW rows[1];
    rows[0] = row;
    jpeg_write_scanlines(cinfo, rows, 1);
  }
  jpeg_finish_compress(cinfo);
}

class LibJpegEncoder : public JpegEncoder {
 public:
  Status EncodeNv12(const uint8_t* nv12, const FrameFormat& format, int quality,
                    std::vector<uint8_t>* out) override {
    if (format.fourcc != kFourccNv12 || format.width == 0 ||
        format.height == 0) {
      return InvalidArgumentError("encoder needs a valid NV12 format");
    }
    struct jpeg_compress_struct cinfo;
    ErrorManager err;
    err.scratch = nullptr;
    err.output = nullptr;
    err.message[0] = '\0';
    unsigned long output_size = 0;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = ErrorExit;
    err.pub.emit_message = SilentMessage;
    if (setjmp(err.jump)) {
      free(err.scratch);
      free(err.output);
      jpeg_destroy_compress(&cinfo);
      return InternalError(std::string("jpeg encode failed: ") + err.message);
    }
    jpeg_create_compress(&cinfo);
    EncodeImpl(&cinfo, &err, nv12, format, quality, &output_size);
    out->assign(err.output, err.output + output_size);
    free(err.scratch);
    free(err.output);
    jpeg_destroy_compress(&cinfo);
    return Status::Ok();
  }

  std::string Describe() const override { return "libjpeg (software)"; }
};

}  // namespace
#endif  // MW_HAVE_JPEG

Status CreateJpegEncoder(const std::string& name,
                         std::unique_ptr<JpegEncoder>* out) {
  if (name == "libjpeg") {
#ifdef MW_HAVE_JPEG
    out->reset(new LibJpegEncoder());
    return Status::Ok();
#else
    (void)out;
    return UnimplementedError(
        "built without libjpeg; reconfigure with -DMW_ENABLE_JPEG=ON");
#endif
  }
  if (name == "v4l2_m2m") {
    return UnimplementedError(
        "the v4l2_m2m hardware encoder backend is not implemented yet; see "
        "docs/PORTING.md");
  }
  return InvalidArgumentError("unknown jpeg encoder '" + name + "'");
}

}  // namespace mw
