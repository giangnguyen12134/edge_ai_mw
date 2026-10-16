# Porting and integration points

## 0. CPU runtime with your own ONNX models (works today)

`"runtime": "cpu"` loads `onnx_library` with `dlopen` and runs each model's
`path` (.onnx) in its own ONNX Runtime session on the CPU. Checks at start-up
(clear errors): library/C-API version, one input only, float32/uint8 input,
rank-4 image shape matching `input_width/height` (static or dynamic H/W, NHWC
or NCHW), speaker input `[1,N]` or `[1,1,N]` with
N = `sample_rate * window_ms / 1000`. Outputs float32/16/64, int32/64 are
converted to float. For float models feed `input_type: "float32"` plus
`input_mean`/`input_scale` so the scaler normalises; a uint8 source into a
float graph is only cast. Not covered: audio front ends (mel/fbank), raw
detector heads without NMS, multi-input models, GPU/NNAPI providers.
Cross builds need no ONNX Runtime at all; deploy an aarch64
`libonnxruntime.so` next to the images.

## 1. Real NPU backend (not implemented; models run on the CPU until then)

`src/npu/neuron_npu_runtime.cc` currently returns `kUnimplemented`, so a config
with `"runtime": "neuron"` refuses to start. Implement `NpuRuntime`/`NpuModel`
against the NeuroPilot SDK matching your BSP (headers and libs are not in this
repo); the steps are in `include/mw/npu/neuron_npu_runtime.h`:
load `.dla`, query shapes/quantisation, bind `TensorBuffer::fd()` as input (or
copy once), run synchronously, de-quantise outputs to float.
Model contract (`include/mw/npu/npu_config.h`): detection output N x 6
(x0,y0,x1,y1,score,class) with NMS inside the graph; classification scores;
speaker model takes float32 PCM of `window_ms`. A speaker model that needs a
mel/fbank front end needs a new audio front-end stage (not present). Add the
runtime to `CreateNpuRuntime()` and `src/CMakeLists.txt`, link the SDK libs.
Grant the service access to the NPU device node (udev rule in
`packaging/70-genio-mw.rules` only covers `dma_heap`; add the Neuron node and
`SupplementaryGroups` in `systemd/mw-npu-manager.service`).
Validate with the e2e script by switching `config/dev/npu.json` to `neuron`
on target with real models.

## 2. Hardware JPEG decode / scale / encode (optional, CPU fallbacks exist)

* `JpegDecoder` name `v4l2_m2m` (MediaTek mtk-jpeg) writing into the pool
  dma-buf: needs the BSP node and plane layout.
* `ImageScaler`: add a hardware implementation next to `CpuImageScaler`
  (`SourceImage::dma_buf_fd` is already carried for it).
* `JpegEncoder`: hardware encoder for the Jetson link; libjpeg is used now.

## 3. Camera and microphone

V4L2 (`v4l2_frame_source.cc`) and ALSA (`alsa_audio_source.cc`) were never run
on hardware here. Check `v4l2-ctl --list-formats-ext`, set
`video.device`/`video.pixel_format`, and `audio.device` (`arecord -l`) for the
RT1061. Allocator: set `"allocator": "dma_heap:<name>"` (see `/dev/dma_heap/`)
on the board; `memfd` works everywhere but is not hardware-importable.

## 4. Ubuntu port

Nothing is Yocto-specific in the code. Build with the same CMake, install the
DEB (`cpack -G DEB`), which ships units, bus policy, sysusers and udev rules.
Needs `libsystemd` >= 245 (sd-bus), systemd as init, and a D-Bus system bus.
The Yocto recipe is `yocto/mw-middleware_0.1.0.bb`; it was not built with
bitbake.

## Bringing up native-format capture on the Genio 720

Nothing of this was run on hardware yet. Suggested order:

1. `v4l2-ctl -d /dev/video0 --list-formats-ext` - pick a format/size/fps and
   put it in `video.pixel_format/width/height/fps` (the service refuses to
   start a session when the driver offers something different).
2. `capture_io: "expbuf"` first: needs only the driver. Check the log line
   `camera session: ... io=expbuf`.
3. `capture_io: "dmabuf"` with `allocator: "dma_heap:<name>"` (see
   `/dev/dma_heap/`). uvcvideo (vb2-vmalloc) normally accepts imported
   dma-bufs. If your BSP's driver or heap refuses, `auto` falls back to
   `expbuf` and logs why.
4. Check `mw-example-client --stats`: `bad_frames` (short/corrupt frames the
   driver flagged), `queued_buffers`, `source_reopens`.
5. Importing these buffers in hardware (NPU/scaler/JPEG): vmalloc-backed
   buffers from `expbuf` are not physically contiguous; whether a block can
   import them depends on its IOMMU. If it cannot, use `dmabuf` with a
   contiguous heap (CMA).
