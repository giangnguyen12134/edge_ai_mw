# Architecture

## Processes

| Process | Role | Bus name |
|---|---|---|
| `mw-media-manager` | owns camera + microphone, the camera fills a shared dma-buf pool in its native pixel format, serves subscriptions | `org.genio.MediaManager` |
| `mw-npu-manager` | the only NPU owner; subscribes like any client; runs all models; emits `Result` | `org.genio.NpuManager` |
| `mw-robotic-bridge` (optional) | subscribes to both, forwards results and JPEG video to the Jetson over TCP | none |
| application | links `mw_sdk` (`MediaClient`, `NpuClient`) | none |

Control plane = D-Bus (sd-bus). Data plane = dma-buf fds sent once, then
slot indices.

## Zero copy (native-format capture, the default)

`video.pool_format = "native"`. The pool *is* the camera's buffer set; no
pixel is copied by the middleware.

1. At every camera (re)open `MediaService` negotiates the camera's own format
   (`VIDIOC_S_FMT`: NV12, YUYV or MJPG, with the driver's `bytesperline` and
   `sizeimage`) and creates the buffers one of two ways (`video.capture_io`):
   * `expbuf` - `VIDIOC_REQBUFS(V4L2_MEMORY_MMAP)` then `VIDIOC_EXPBUF` per
     buffer: the driver allocates, we receive a dma-buf fd for each.
   * `dmabuf` - the configured allocator (dma-heap) allocates;
     `VIDIOC_REQBUFS(V4L2_MEMORY_DMABUF)` and `VIDIOC_QBUF` with `m.fd`
     import them into the driver. Use this when a downstream block (NPU,
     scaler, encoder) needs physically contiguous or heap-specific memory.
   * `auto` - `dmabuf` when the allocator is a real dma-heap, otherwise
     `expbuf`; if the driver refuses one (including a refusal that only shows
     at the first `QBUF`) the other is tried.
2. The buffer fds become the frame pool (`FramePool::InitFromFds`). A pool
   slot index equals the V4L2 buffer index.
3. `QBUF` gives a slot to the driver; `DQBUF` returns it filled together with
   `bytesused` (the JPEG size for MJPG). Ownership of a slot: driver ->
   service (publish) -> one reference per subscriber -> back to the free list
   when the last `ReleaseFrame` arrives, and the capture loop immediately
   `QBUF`s it again (an eventfd wakes the wait). Clients that hold frames
   too long only starve themselves: a camera without queued buffers drops
   frames, it never blocks, and is not mistaken for a stall.
4. Subscribers receive the pool fds once (`SubscribeVideo`, with the camera
   format) and per frame a unicast
   `FrameReady(id, slot, bytes_used, seq, ts)`. They map or import the fd.
5. **Camera sessions.** The pool belongs to one device session. When the
   camera is unplugged or stalls the service emits `StreamStateChanged
   "recovering"`, creates a *new* pool after reopening and emits `"streaming"`;
   `MediaClient` then subscribes again by itself. Old frames stay valid on the
   client (it keeps its own fd duplicates) but are never reported back.

What is *not* zero copy: a UVC camera delivers USB packets that `uvcvideo`
copies into the buffer with the CPU, inside the kernel. That one copy is
inherent to USB cameras; everything after it is zero copy.

Native frames are MJPG, YUYV or NV12 - whatever the camera offers. Consumers
that need another format need a preprocessing stage, which does **not exist
yet**: the NPU manager's CPU scaler reads NV12 only (it idles with one
warning on other formats), the Jetson bridge forwards MJPG as is, encodes
NV12 and refuses YUYV. `video.pool_format = "nv12"` keeps the previous
behaviour (one CPU conversion into an NV12 pool, via `FrameIngest`) as a
workaround until preprocessing exists.

The NPU path currently converts NV12 to the model tensor on the CPU
(`ImageScaler`, bilinear) into a `TensorBuffer`; with a HW scaler and a
dma-buf-capable NPU backend this also becomes zero copy.

## Threads

* MediaService: capture thread (ingest + publish), bus thread (`Bus`:
  sd-bus is thread-confined, other threads post tasks and wake it via eventfd),
  audio thread. A watchdog heartbeat feeds systemd `WatchdogSec`.
* NpuService: bus thread, one inference thread (single NPU owner), "latest frame
  wins". Per model `target_fps`, scaled down by `ThermalMonitor` when hot;
  `gated_by`/`gate_hold_ms` run e.g. gesture only while a face was seen, and an
  empty "cleared" result is emitted when a gate closes.

## VoiceSprint

Audio ring (single writer, multi reader, lock-free, memfd) -> 20 ms energy VAD,
300 ms hangover -> utterance padded/cut to `window_ms` as float32 PCM ->
embedding model -> L2 normalise -> cosine match against `SpeakerDb`
(`match_threshold`) -> `Result` kind `speaker`. `EnrollSpeaker(name)` stores
the next utterance (30 s timeout). Speaker names are personal data: the bus
policy denies signal reception by default (see `dbus/org.genio.MiddleWare.conf`).

## Modules (dependencies point down only)

```
robotic -> sdk -> ipc -> common        npu -> sdk -> ipc -> common
media -> ipc -> common
```
Inference backends: `cpu` (ONNX Runtime via dlopen), `neuron` (NPU, stub), `fake` (tests).
Hardware seams are interfaces with a factory: `FrameSource`, `AudioSource`,
`JpegDecoder`, `JpegEncoder`, `ImageScaler`, `NpuRuntime`, `BufferAllocator`.

## Failure behaviour

Camera stall -> device reopen (`stall_timeout_ms`), `StreamStateChanged`
`recovering`. MediaManager restart -> clients resubscribe automatically
(tested). RoboticBridge reconnects forever with a bounded drop-oldest queue.
Errors cross D-Bus as `org.genio.Error.<StatusCodeName>`.

## Logging

`MW_LOG` is thread-safe (per-message stream, one write per line) and, for the
file and stderr sinks, process-safe (one `write(2)` per line on `O_APPEND`).
Sinks (stderr, shared file, UDP syslog) are chosen per process from the shared
`logging.json`; see README "Logging". Not async-signal-safe: do not log from a
signal handler (the services use `signalfd`).
