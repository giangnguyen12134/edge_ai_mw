// Copyright 2026 Genio Middleware Authors.
//
// NpuManager: the only owner of the NPU. All models are loaded here and run
// from ONE inference thread, which is what serialises access to the NPU.
//
// Threads:
//   mw-bus   D-Bus handlers (owned by Bus) and the Result signal emission.
//   mw-npu   the inference loop: video models on the latest camera frame,
//            then the VoiceSprint pipeline on newly arrived audio.
//   (the MediaClient SDK owns its own bus and subscription threads)
//
// Camera frames arrive as dma-buf leases from the MediaManager. Only the most
// recent frame is kept ("latest wins"): if inference is slower than the camera
// the older frames are returned to the pool immediately.
//
// Scheduling per frame: enabled models in priority order, each at most
// target_fps (scaled down while the SoC is hot), and a model with "gated_by"
// only runs while its gate model has recently produced results (for example
// gesture detection only while a face is present).

#ifndef MW_NPU_NPU_SERVICE_H_
#define MW_NPU_NPU_SERVICE_H_

#include <stdint.h>
#include <systemd/sd-bus.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mw/common/buffer_allocator.h"
#include "mw/common/inference_result.h"
#include "mw/common/macros.h"
#include "mw/common/status.h"
#include "mw/ipc/bus.h"
#include "mw/npu/image_scaler.h"
#include "mw/npu/npu_config.h"
#include "mw/npu/npu_runtime.h"
#include "mw/npu/postprocess.h"
#include "mw/npu/speaker_db.h"
#include "mw/npu/tensor_buffer.h"
#include "mw/npu/thermal_monitor.h"
#include "mw/sdk/media_client.h"

namespace mw {

class NpuService {
 public:
  explicit NpuService(const NpuConfig& config);
  ~NpuService();

  MW_DISALLOW_COPY_AND_ASSIGN(NpuService);

  // Loads the models, claims the bus name and starts the threads. The
  // MediaManager does not have to be running yet.
  Status Start();
  void Stop();

  // False if the inference thread stopped making progress. Feed into the
  // systemd watchdog.
  bool Healthy() const;
  bool bus_lost() const { return bus_.connection_lost(); }

 private:
  struct ModelState {
    explicit ModelState(const ModelConfig& model_config);

    ModelConfig config;
    InputSpec spec;
    std::unique_ptr<NpuModel> model;
    std::unique_ptr<Postprocessor> post;  // Video models only.
    TensorBuffer input;
    bool loaded;
    std::atomic<bool> enabled;

    // Inference thread only.
    bool have_run;
    uint64_t last_run_ns;
    bool have_sequence;
    uint64_t last_sequence;
    bool active;  // Produced a result since the gate last opened.

    // Read by other threads.
    std::atomic<uint64_t> last_hit_ns;  // Last result that had items.
    std::atomic<uint64_t> runs;
    std::atomic<uint64_t> errors;
    std::atomic<uint64_t> infer_us_total;
    std::atomic<uint64_t> latency_us_total;
  };

  Status LoadModels();
  ModelState* FindModel(const std::string& name) const;

  void OnFrame(MediaClient::Frame frame);
  void InferenceLoop();
  void RunVideoModels(const MediaClient::Frame& frame);
  void RunImageModel(ModelState* m, const MediaClient::Frame& frame,
                     const SourceImage& source);
  bool GateOpen(const ModelState& m, uint64_t now_ns) const;

  void RunSpeaker();
  void FeedAudio(const int16_t* samples, size_t frames, uint32_t channels);
  void FinishUtterance();
  void ProcessUtterance(const std::vector<float>& pcm);

  void Publish(const InferenceResult& result);
  void LogModelError(ModelState* m, const Status& status);

  void OnMethodCall(const char* member, sd_bus_message* call);
  void HandleListModels(sd_bus_message* call);
  void HandleSetModelEnabled(sd_bus_message* call);
  void HandleEnrollSpeaker(sd_bus_message* call);
  void HandleRemoveSpeaker(sd_bus_message* call);
  void HandleListSpeakers(sd_bus_message* call);
  void HandleGetStats(sd_bus_message* call);

  NpuConfig config_;
  Bus bus_;
  MediaClient media_;
  std::unique_ptr<BufferAllocator> allocator_;
  std::unique_ptr<NpuRuntime> runtime_;
  std::unique_ptr<ImageScaler> scaler_;
  SpeakerDb speakers_;
  ThermalMonitor thermal_;

  std::vector<std::unique_ptr<ModelState>> models_;  // Priority order.
  ModelState* speaker_model_;

  std::thread worker_;
  std::atomic<bool> stop_;
  std::atomic<bool> started_;
  std::atomic<uint64_t> heartbeat_ns_;

  std::mutex frame_mutex_;
  std::condition_variable frame_cv_;
  std::shared_ptr<MediaClient::Frame> latest_frame_;
  bool frame_dirty_;
  bool warned_unsupported_format_;  // Inference thread only.

  // Speaker pipeline state (inference thread only unless noted).
  std::shared_ptr<AudioRingReader> audio_ring_;
  bool audio_unusable_;
  uint64_t audio_cursor_;
  std::vector<float> block_;    // Partial 20 ms analysis block (mono).
  std::vector<float> segment_;  // Utterance being collected (mono).
  uint32_t trailing_silence_ms_;
  bool in_speech_;
  std::mutex enroll_mutex_;  // Guards the two fields below.
  std::string enroll_name_;
  uint64_t enroll_deadline_ns_;

  std::atomic<uint64_t> frames_received_;
  std::atomic<uint64_t> frames_replaced_;
  std::atomic<uint64_t> utterances_;
  std::atomic<uint64_t> audio_lost_frames_;
  std::atomic<uint64_t> results_published_;
  std::atomic<uint64_t> throttled_loops_;
  std::atomic<int64_t> temp_milli_c_;  // Snapshot of the thermal monitor.
  std::atomic<bool> thermal_throttled_;
  uint64_t start_ns_;
};

}  // namespace mw

#endif  // MW_NPU_NPU_SERVICE_H_
