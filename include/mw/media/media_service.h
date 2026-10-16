// Copyright 2026 Genio Middleware Authors.
//
// MediaManager: the only owner of the camera, the microphone and the shared
// DMA-BUF frame pool.
//
// Video has two modes (video.pool_format):
//   native  The camera driver fills the shared dma-bufs directly (VIDIOC_EXPBUF
//           or V4L2_MEMORY_DMABUF) and frames keep the camera's pixel format.
//           A buffer is requeued to the driver once every client released it.
//           The pool is tied to one camera session: every (re)open of the
//           camera creates a new pool ("epoch"), announced by
//           StreamStateChanged, and clients must subscribe again.
//   nv12    Legacy: capture, convert on the CPU into an NV12 pool, publish.
//
// Threads:
//   mw-bus      D-Bus handlers (owned by Bus).
//   mw-capture  camera loop.
//   mw-audio    microphone loop: read PCM -> shared ring.
//
// The camera loop never blocks on clients: slow subscribers miss frames, and
// when the pool is exhausted the frame is dropped and counted.

#ifndef MW_MEDIA_MEDIA_SERVICE_H_
#define MW_MEDIA_MEDIA_SERVICE_H_

#include <stdint.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "mw/common/audio_ring.h"
#include "mw/common/buffer_allocator.h"
#include "mw/common/macros.h"
#include "mw/common/scoped_fd.h"
#include "mw/common/status.h"
#include "mw/ipc/bus.h"
#include "mw/media/audio_source.h"
#include "mw/media/dmabuf_source.h"
#include "mw/media/frame_ingest.h"
#include "mw/media/frame_pool.h"
#include "mw/media/frame_source.h"
#include "mw/media/media_config.h"
#include "mw/media/video_hub.h"

namespace mw {

class MediaService {
 public:
  explicit MediaService(const MediaConfig& config);
  ~MediaService();

  MW_DISALLOW_COPY_AND_ASSIGN(MediaService);

  // Builds the pipeline, claims the bus name and starts the threads. A camera
  // that is not plugged in is NOT an error: the service keeps retrying.
  Status Start();
  void Stop();

  // False if a worker thread stopped making progress or the bus connection
  // broke. Feed this into the systemd watchdog.
  bool Healthy() const;
  bool bus_lost() const { return bus_.connection_lost(); }

 private:
  enum StreamState { kStarting = 0, kStreaming = 1, kRecovering = 2 };

  struct Counters {
    Counters();
    std::atomic<uint64_t> frames_captured;
    std::atomic<uint64_t> frames_published;
    std::atomic<uint64_t> frames_dropped_no_slot;
    std::atomic<uint64_t> ingest_errors;
    std::atomic<uint64_t> capture_errors;
    std::atomic<uint64_t> source_reopens;
    std::atomic<uint64_t> ingest_us_avg;
    std::atomic<uint64_t> audio_frames;
    std::atomic<uint64_t> audio_reopens;
  };

  // One frame pool with the hub that tracks its leases. Replaced as a whole.
  struct Epoch {
    FramePool pool;
    std::unique_ptr<VideoHub> hub;
  };

  std::shared_ptr<Epoch> CurrentEpoch() const;
  void SetEpoch(const std::shared_ptr<Epoch>& epoch);
  std::shared_ptr<Epoch> NewEpoch();

  void CaptureLoop();
  void ProcessFrame(Epoch* epoch, const CapturedFrame& raw);
  void NativeCaptureLoop();
  Status StartNativeSession(std::shared_ptr<Epoch>* epoch);
  Status RequeueFree(Epoch* epoch);
  void PublishNative(Epoch* epoch, const DequeuedBuffer& buffer);
  void AudioLoop();
  void SetState(StreamState state);
  void SleepInterruptibly(int milliseconds, std::atomic<uint64_t>* heartbeat);

  void OnMethodCall(const char* member, sd_bus_message* call);
  void HandleSubscribeVideo(sd_bus_message* call);
  void HandleSubscribeAudio(sd_bus_message* call);
  void HandleUnsubscribe(sd_bus_message* call);
  void HandleReleaseFrame(sd_bus_message* call);
  void HandleGetStats(sd_bus_message* call);
  void NotifyFrame(const VideoSubscriber& subscriber, const FrameInfo& frame);

  static const char* StateName(int state);

  MediaConfig config_;
  Bus bus_;
  std::unique_ptr<BufferAllocator> allocator_;
  bool native_;
  ScopedFd wake_fd_;  // Interrupts the native capture wait.
  mutable std::mutex epoch_mutex_;
  std::shared_ptr<Epoch> epoch_;
  std::unique_ptr<FrameSource> source_;       // Legacy mode.
  std::unique_ptr<FrameIngest> ingest_;       // Legacy mode.
  std::unique_ptr<DmaBufSource> dma_source_;  // Native mode.
  std::unique_ptr<AudioSource> audio_;
  AudioRingWriter ring_;

  std::thread capture_thread_;
  std::thread audio_thread_;
  std::atomic<bool> stop_;
  std::atomic<bool> started_;
  std::atomic<int> state_;
  std::atomic<uint64_t> capture_heartbeat_ns_;
  std::atomic<uint64_t> audio_heartbeat_ns_;
  std::atomic<uint64_t> sequence_;
  uint64_t start_ns_;
  Counters counters_;
};

}  // namespace mw

#endif  // MW_MEDIA_MEDIA_SERVICE_H_
