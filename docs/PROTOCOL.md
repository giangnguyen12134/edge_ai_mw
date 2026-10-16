# Wire protocols

## D-Bus

Authoritative definitions: `dbus/org.genio.MediaManager1.xml`,
`dbus/org.genio.NpuManager1.xml` (also embedded for Introspect). Objects:
`/org/genio/MediaManager`, `/org/genio/NpuManager`. System bus access is
limited to group `genio-clients` (`dbus/org.genio.MiddleWare.conf`).

Client sequence (what `MediaClient` does):

1. `SubscribeVideo(s name, u queue_depth)` -> `(u id, s stream, u w, u h, u fourcc, u stride, u uv_offset, u buffer_size, ah fds)`.
   One fd per pool slot (index = slot number). Max 32 subscribers. `fourcc` is
   the camera's native format (`NV12`, `YUYV` or `MJPG`) unless the service
   runs with `video.pool_format=nv12`; validate it with
   `ValidateFrameFormat()` before mapping anything.
2. On signal `FrameReady(u id, u slot, u bytes_used, t seq, t ts_ns)`: read the
   frame from `fds[slot]`. NV12: Y plane at 0, interleaved UV at `uv_offset`,
   rows `stride` bytes. YUYV: packed, rows `stride` bytes (>= 2*width).
   MJPG: a JPEG of `bytes_used` bytes at offset 0 (`stride` is 0, `bytes_used`
   varies per frame). For CPU access around reads use `DMA_BUF_IOCTL_SYNC`
   (`DmaBufMapping` does this).

   The pool belongs to one camera session. On `StreamStateChanged(stream,
   "recovering")` drop the subscription (slots are void); when
   `"streaming"` arrives call `SubscribeVideo` again. `SubscribeVideo` fails
   with `Unavailable` while no session is running.
3. Always answer with `ReleaseFrame(u id, u slot)` (NO_REPLY_EXPECTED).
4. `Unsubscribe(id)` or disconnect releases everything.

`SubscribeAudio(s name)` -> `(s stream, u rate, u channels, u capacity_frames, h ring_fd)`.
Ring layout: `AudioRingHeader` (64 bytes: magic `0x5241574d`, version 1, rate,
channels, capacity, atomic u64 total frames written) then S16LE interleaved
samples; readers keep their own cursor (`AudioRingReader`).

NPU: `ListModels` `a(ssbd)`, `SetModelEnabled(sb)`, `EnrollSpeaker(s)`,
`RemoveSpeaker(s)`, `ListSpeakers` `as`, `GetStats` `a(st)`, signal
`Result(s model, s kind, t seq, t ts_ns, a(sddddd) items)` with items
`(label, score, x, y, w, h)`, box normalised 0..1, zeros when non-spatial.

## Robotic link (TCP to the Jetson, default 192.168.55.1)

Results, port 9100: one JSON object per line, e.g.
`{"type":"result","model":"object","kind":"detection","seq":42,"ts_ns":123,"items":[{"label":"cup","score":0.9100,"box":[x,y,w,h]}]}`
(`box` omitted for non-spatial results; NaN/Inf are sent as 0).

Video, port 9101: repeated packets, **little endian**:

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `MWV1` |
| 4 | 4 | JPEG length N |
| 8 | 8 | frame sequence |
| 16 | 8 | CLOCK_MONOTONIC ns (Genio clock, not comparable to Jetson's) |
| 24 | 2 | width |
| 26 | 2 | height |
| 28 | N | JPEG |

The Jetson-side receiver is not part of this repository.
