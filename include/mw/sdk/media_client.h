// Copyright 2026 Genio Middleware Authors.
//
// Client SDK for the MediaManager. Hides all D-Bus details:
//
//   mw::MediaClient::Options options;
//   options.client_name = "my-app";
//   mw::MediaClient client(options);
//   client.Start(
//       [](mw::MediaClient::Frame frame) {
//         // frame.dma_buf_fd() can be imported by the GPU/NPU/encoder, or
//         // frame.data() read after BeginCpuRead(). The slot is returned to
//         // the camera when |frame| is destroyed or Release()d.
//       },
//       nullptr);
//
// Frames arrive in the camera's NATIVE pixel format (frame.format().fourcc:
// NV12, YUYV or MJPG); the SDK does not convert. frame.size() is the number
// of valid bytes (the JPEG size for MJPG).
//
// The client reconnects automatically when MediaManager restarts or the
// camera recovers (the buffers change with every camera session; the
// connection callback reports false then true again). Frames from a previous
// connection stay valid (their mapping is kept alive) but are not reported
// back to the new service instance.
//
// THREADING: the frame callback runs on the SDK's bus thread. Return quickly;
// move the Frame elsewhere if you need to keep it. Holding frames longer than
// the queue depth lets the service skip frames for you (never the camera).

#ifndef MW_SDK_MEDIA_CLIENT_H_
#define MW_SDK_MEDIA_CLIENT_H_

#include <stddef.h>
#include <stdint.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mw/common/audio_ring.h"
#include "mw/common/frame_types.h"
#include "mw/common/macros.h"
#include "mw/common/status.h"
#include "mw/ipc/bus.h"

namespace mw {

class MediaClient {
 public:
  struct Options {
    Options();
    std::string bus;             // "system" or "user".
    std::string media_bus_name;  // MediaManager well-known name.
    std::string client_name;     // Shown in the service log.
    uint32_t queue_depth;        // Frames you may hold at once.
    bool map_frames;             // Provide CPU access via Frame::data().
    bool want_audio;             // Also attach the shared audio ring.
  };

  struct VideoInfo {
    VideoInfo() : slot_count(0) {}
    std::string stream_id;
    FrameFormat format;
    uint32_t slot_count;
  };

  struct Session;  // Implementation detail.

  // RAII lease on one pool slot.
  class Frame {
   public:
    Frame();
    Frame(Frame&& other);
    Frame& operator=(Frame&& other);
    ~Frame();

    MW_DISALLOW_COPY_AND_ASSIGN(Frame);

    bool valid() const { return session_ != nullptr; }
    const FrameInfo& info() const { return info_; }
    const FrameFormat& format() const;

    // Valid payload bytes in the buffer (info().bytes_used).
    size_t size() const { return info_.bytes_used; }

    // Descriptor of the whole slot's dma-buf (do not close it).
    int dma_buf_fd() const;

    // Read-only CPU view (layout per format()), or nullptr when the client was
    // created with map_frames=false. Bracket reads with BeginCpuRead() and
    // EndCpuRead() for cache coherency.
    const uint8_t* data() const;
    void BeginCpuRead() const;
    void EndCpuRead() const;

    // Returns the slot early. Safe to call more than once.
    void Release();

   private:
    friend class MediaClient;
    std::shared_ptr<Session> session_;
    FrameInfo info_;
  };

  typedef std::function<void(Frame frame)> FrameCallback;
  typedef std::function<void(bool connected, const VideoInfo& info)>
      ConnectionCallback;

  explicit MediaClient(const Options& options);
  ~MediaClient();

  MW_DISALLOW_COPY_AND_ASSIGN(MediaClient);

  // Connects to the bus and starts the (re)subscription thread. Returns once
  // the bus connection is up; the MediaManager does not have to be running
  // yet. |on_connection| may be empty.
  Status Start(const FrameCallback& on_frame,
               const ConnectionCallback& on_connection);
  void Stop();

  bool connected() const;

  // Latest audio ring (only when Options::want_audio), or nullptr.
  std::shared_ptr<AudioRingReader> audio_ring() const;

  // Fetches the service counters.
  Status GetStats(std::vector<std::pair<std::string, uint64_t>>* stats);

 private:
  void ManagerLoop();
  Status SubscribeVideo(std::shared_ptr<Session>* out);
  Status SubscribeAudio(std::shared_ptr<AudioRingReader>* out);
  void OnFrameReady(sd_bus_message* message);
  void OnStreamState(sd_bus_message* message);
  void RequestResubscribe();

  Options options_;
  Bus bus_;
  FrameCallback on_frame_;
  ConnectionCallback on_connection_;
  std::thread manager_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_;
  bool needs_subscribe_;
  std::shared_ptr<Session> session_;
  std::shared_ptr<AudioRingReader> audio_ring_;
  bool started_;
};

}  // namespace mw

#endif  // MW_SDK_MEDIA_CLIENT_H_
