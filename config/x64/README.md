# Ubuntu x86-64 (desktop / server) configuration

Runs the three services on the **user** session bus with a normal webcam and
the default ALSA microphone. No root, no udev rules, no dma-heap.

* `media.json`  UVC webcam `/dev/video0`, YUYV 640x480 (nearly all webcams;
  use `v4l2-ctl --list-formats-ext` to see others), `memfd` pool, ALSA `default`.
* `npu.json`    `cpu` runtime; model paths are relative to the working
  directory (`models/face_detect.onnx` ...): put your ONNX models there.
  `onnx_library` must resolve to an x86-64 libonnxruntime.so (a release tarball
  from the ONNX Runtime project, or the one inside the `onnxruntime` pip wheel).
* No camera at hand? Use `config/dev/*.json` (synthetic camera and microphone,
  synthetic ONNX models in `tests/data`).

Zero copy works the same way: the dma-buf API is replaced by a memfd pool, the
client maps the same pages, and the frame still travels by file descriptor.
A `memfd` buffer cannot be imported by GPU/NPU hardware (not needed on x64).

    mw-media-manager --config config/x64/media.json &
    mw-npu-manager   --config config/x64/npu.json   &
    mw-example-client --bus user --seconds 10
