# genio-mw — camera / NPU middleware for MediaTek Genio 720

C++11 (Google style), CMake >= 3.8. Builds natively for Ubuntu x86-64 or
cross-compiles from it to aarch64 (Genio 720, Cortex-A78); you choose. Three services talk over D-Bus; video pixels never do:
frames are shared as dma-buf file descriptors (zero copy after the camera
ingest step).

```
UVC camera ══(V4L2 EXPBUF / DMABUF, camera-native format)══> mw-media-manager ──(dma-buf fds, once)──> clients / mw-npu-manager
RT1061 mic ──>      │  FrameReady(slot) unicast, ReleaseFrame(slot)        │
                    └── audio ring (memfd) ─────────────────────────────>  │
                                              Result signal  <─────────────┘
                                       mw-robotic-bridge ──TCP──> Jetson Orin Nano
```

See `docs/ARCHITECTURE.md`, `docs/PROTOCOL.md`, `docs/PORTING.md`.

## Status — read this first

| Part | State |
|---|---|
| MediaManager, frame pool, client SDK, D-Bus, systemd, packaging | implemented, tested on a host |
| NpuManager scheduling, gating, VoiceSprint pipeline, speaker DB | implemented, tested |
| **`cpu` runtime: ONNX models on the Cortex-A cores via a `dlopen`ed `libonnxruntime.so`** | implemented; tested with ONNX Runtime 1.29 on x86-64 using small synthetic ONNX models (no real model of yours was available) |
| **`neuron` NPU backend (inference on the NPU)** | **NOT implemented** (stub fails at start-up). Models run on the **CPU** until it exists. See `docs/PORTING.md` |
| `fake` runtime | test double only (hermetic unit/e2e tests need no model or library) |
| HW JPEG decode / HW scaler / HW JPEG encode | **NOT implemented**; CPU (libjpeg, bilinear) fallbacks |
| **Native-format zero-copy capture** (`VIDIOC_EXPBUF` / `V4L2_MEMORY_DMABUF`, camera pixel format kept) | implemented; tested against a software V4L2 driver (synthetic device), **never run against a real camera, `/dev/dma_heap` or the Genio kernel** |
| **Preprocessing from camera format (MJPG/YUYV) to model input** | **NOT implemented** (deferred): the NPU manager's CPU scaler reads NV12 only, so with the default `pool_format=native` and a MJPG/YUYV camera the video models idle. Workaround: `video.pool_format=nv12` |
| Real UVC camera (V4L2) and ALSA microphone | written, **never run against hardware** |
| x64 Ubuntu build and run | built, unit tests and end-to-end test pass on x86-64 Ubuntu (synthetic camera/mic; a real webcam was not available) |
| arm64 cross build | libraries verified to compile for aarch64; executables not linked (no arm64 sysroot available when this was produced) |

Until the `neuron` backend exists, no model runs on the NPU: the `cpu`
runtime uses the A78 cores (and competes with the camera ingest for them).
Real models must follow the contract in `include/mw/npu/cpu_runtime.h` and
`npu_config.h`; in particular a detector must output N x 6 rows
(x0,y0,x1,y1,score,class) with NMS inside the exported graph — a raw YOLO head
(e.g. `[1,84,8400]`) needs a new `Postprocessor`.

## ONNX Runtime (cpu runtime)

Nothing to link at build time (only the vendored MIT C header,
`third_party/onnxruntime`). On the target install an `libonnxruntime.so`
(>= 1.14, aarch64) and point `npu.json` at it:
`"runtime": "cpu", "onnx_library": "/usr/lib/libonnxruntime.so", "cpu_threads": 2`
and per model `"path": "/usr/share/genio-mw/models/face.onnx"`. Optional per
model `"input_layout": "auto|nhwc|nchw"` (auto reads it from the ONNX shape).
Run the CPU tests with `-DMW_ONNX_LIBRARY=/path/libonnxruntime.so` (without it
they are skipped). `tools/make_test_models.py` regenerates `tests/data/*.onnx`.

## Layout

```
include/mw/<module>/*.h   public headers (common, ipc, media, sdk, npu, robotic)
src/<module>/*.cc         one static library per module
apps/                     mw-media-manager, mw-npu-manager, mw-robotic-bridge, mw-example-client
config/ config/x64/ config/dev/  Genio, Ubuntu x64 webcam, and synthetic (no hardware) configuration
dbus/                     interface XML, bus policy
systemd/ packaging/ yocto/  units, sysusers, udev, DEB, Yocto recipe
cmake/                    toolchain files (aarch64, x86_64), helpers
third_party/onnxruntime/  vendored ONNX Runtime C header (MIT), library is dlopen()ed
tests/                    unit/ (73 tests) and e2e/ (about 37 checks), data/ synthetic ONNX models
tools/                    build.sh (x64|arm64), lint.sh, make_test_models.py
```

## Choose the target: x64 or arm64

One source tree, two targets, selected with `MW_TARGET_ARCH` (or the helper script):

| | Ubuntu x86-64 | Genio 720 (arm64, Cortex-A78) |
|---|---|---|
| script | `tools/build.sh x64 --test` | `tools/build.sh arm64 --sysroot /path/to/sysroot` |
| cmake | `cmake -S . -B build/x64 -DMW_TARGET_ARCH=x64` | `cmake -S . -B build/arm64 -DMW_TARGET_ARCH=arm64 -DMW_SYSROOT=...` |
| compiler | host `g++` | `aarch64-linux-gnu-g++` (toolchain file chosen automatically) |
| sysroot | not needed | **needed** (libsystemd, libjpeg, libasound for arm64) |
| tests | unit + e2e run on the host | cross build only; run on the board |
| DEB | `amd64` (`--package`) | `arm64` |
| config | `config/x64/` (webcam, user bus) or `config/dev/` (synthetic) | `config/` (system bus, dma-heap) |

`MW_TARGET_ARCH=native` (default) builds for the machine you are on. The
reverse cross direction (arm64 host to x64) uses
`cmake/toolchain-x86_64-linux-gnu.cmake`; it is untested. Your own
`-DCMAKE_TOOLCHAIN_FILE` always takes precedence.

### x64 on Ubuntu

```
sudo apt install build-essential cmake pkg-config libsystemd-dev libjpeg-dev libasound2-dev dbus
tools/build.sh x64 --test -- -DMW_ONNX_LIBRARY=/path/to/x64/libonnxruntime.so
build/x64/apps/mw-media-manager --config config/x64/media.json &
build/x64/apps/mw-npu-manager   --config config/x64/npu.json &
build/x64/apps/mw-example-client --bus user --seconds 10
```
(`-DMW_ONNX_LIBRARY` is only needed to run the CPU-runtime tests.) Without a
camera use `config/dev/*.json` (synthetic camera, microphone and ONNX models).
Frames are shared through a `memfd` pool instead of a dma-heap; the
file-descriptor hand-off over D-Bus is identical.

### arm64 for the Genio 720 (cross compile on the x86-64 host)

Needs `gcc-aarch64-linux-gnu g++-aarch64-linux-gnu` and a **sysroot** of the
target with `libsystemd`, `libjpeg`, `libasound` headers and libraries (take it
from the Yocto SDK: `bitbake mw-middleware -c populate_sdk`, or an Ubuntu arm64
rootfs with the `-dev` packages).

```
tools/build.sh arm64 --sysroot /path/to/sysroot --package
```
Variables: `MW_SYSROOT`, `MW_CROSS_PREFIX` (default `aarch64-linux-gnu-`),
`MW_TARGET_FLAGS` (default `-march=armv8.2-a`; try `-mcpu=cortex-a78`).
Options (both targets): `MW_ENABLE_JPEG`, `MW_ENABLE_ALSA`, `MW_BUILD_ROBOTIC`,
`MW_BUILD_TESTS`, `MW_WERROR`, `MW_SANITIZE=address,undefined`.
Unit tests cross-build too but only run on the target; the e2e test is
skipped when cross compiling. Deploy an aarch64 `libonnxruntime.so` for the
`cpu` runtime.

## Logging

All services use one logging configuration (`/etc/genio-mw/logging.json`,
or `--log-config FILE`, or `$MW_LOG_CONFIG`) with any combination of sinks:

```
{"sinks": ["stderr", "file", "udp"],
 "file": {"path": "/var/log/genio-mw/middleware.log"},
 "udp":  {"host": "192.168.1.10", "port": 514, "facility": 16, "max_bytes": 1200}}
```
* `stderr` (default): journald under systemd, with priorities.
* `file`: one append-only file shared by all processes (every line is a single
  `write(2)` on an `O_APPEND` descriptor; lines carry `program[pid]`). Rotate
  with the shipped logrotate rule (`copytruncate`). Use an absolute path.
  Under systemd the services have `PrivateTmp=yes`, so `/tmp/middleware.log`
  is **not** shared between them there: use `/var/log/genio-mw/` (created by
  `LogsDirectory=`). `/tmp/middleware.log` is fine for development runs.
* `udp`: RFC 5424 syslog datagrams (`<PRI>1 time host app pid - - msg`), best
  effort: non-blocking, dropped when the server or network is down (counted),
  truncated to `max_bytes` (default 1200, stays below the MTU). It is
  unencrypted and unauthenticated; log lines can contain client and speaker
  names, so send only over a trusted link or to a local rsyslog/syslog-ng that
  forwards over TLS. Prefer an IP address: a host name is resolved once at start.
* A sink that cannot be opened (unwritable file, unresolvable host) is skipped
  with a warning; the service still starts (stderr is the fallback).
* Control characters in messages are escaped, so one message is one line.
* Not included: a log-collector server, TLS/TCP transport, retries or
  buffering of unsent datagrams.

## Configuration

JSON files (`config/*.json`), every value overridable:
`mw-media-manager --config F --set video.width=1920 --set video.pixel_format=MJPG`.
`--check-config` validates and exits. Camera format (`video.width/height/fps/pixel_format`
= NV12, YUYV or MJPG) is what clients receive: in the default
`video.pool_format = "native"` the camera fills the shared dma-bufs directly
(`video.capture_io`: `auto`, `expbuf` or `dmabuf`, see `docs/ARCHITECTURE.md`)
and no conversion happens. `video.pool_format = "nv12"` is the legacy mode
that converts every frame into an NV12 pool on the CPU. Clients map the fd or
import it (see `MediaClient` in `include/mw/sdk`; `frame.format()` tells the
pixel format, `frame.size()` the valid bytes).

## Style

`tools/lint.sh` (clang-format `.clang-format` + cpplint `CPPLINT.cfg`). The
documented cpplint exceptions are in `CPPLINT.cfg`.


## AI Models
conda create -n vmx_mw_env python=3.10

## Run
sudo ./build/x64/apps/mw-media-manager --config ./config/media.json
sudo ./build/x64/apps/mw-npu-manager --config ./config/npu.json
