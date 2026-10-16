// Copyright 2026 Genio Middleware Authors.
//
// NpuManager configuration (JSON). Each entry of "models" describes one model
// together with the pre- and post-processing it needs, so adding or swapping
// a model is a configuration change.
//
// Conventions the post-processors rely on (export your models accordingly):
//   detection       output tensor of N rows x 6 values:
//                   (box0, box1, box2, box3, score, class_id) with NMS already
//                   applied inside the exported graph. box_format selects
//                   "xyxy" (corners) or "xywh" (centre x, centre y, w, h);
//                   coords are model-input pixels unless
//                   coords_normalized is true (0..1 of the model input).
//   classification  output tensor of class scores (probabilities, or logits
//                   with apply_softmax=true).
//   speaker         output tensor holding one speaker embedding. The input is
//                   the utterance as float32 mono PCM in [-1, 1], padded or
//                   cut to window_ms. (A model that needs a mel/fbank front
//                   end requires an AudioFrontend, see docs/PORTING.md.)
// Image input is NHWC, 3 channels, uint8 or float32.

#ifndef MW_NPU_NPU_CONFIG_H_
#define MW_NPU_NPU_CONFIG_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "mw/common/status.h"

namespace mw {

struct ModelConfig {
  ModelConfig();

  std::string name;  // Unique, used in results and D-Bus calls.
  std::string kind;  // "detection", "classification" or "speaker".
  std::string path;  // Compiled model (.dla). Unused by the fake runtime.
  std::vector<std::string> labels;
  bool enabled;

  // Scheduling (video models).
  double target_fps;
  int priority;          // Higher runs first on a frame.
  std::string gated_by;  // Run only while this model has recent results.
  uint32_t gate_hold_ms;

  // Image input (video models).
  uint32_t input_width;
  uint32_t input_height;
  std::string input_layout;  // "auto", "nhwc" or "nchw" (cpu runtime).
  std::string color_order;   // "rgb" or "bgr".
  std::string resize_mode;   // "stretch" or "letterbox".
  std::string input_type;    // "uint8" or "float32".
  float input_mean;          // float32: (pixel - mean) * scale
  float input_scale;

  // Output decoding.
  std::string box_format;  // "xyxy" or "xywh".
  bool coords_normalized;
  float score_threshold;
  bool apply_softmax;

  // Audio input (speaker model).
  uint32_t sample_rate;
  uint32_t window_ms;  // Fixed model input length.
  uint32_t min_speech_ms;
  float vad_threshold_db;  // Frame RMS above this counts as speech.
  float match_threshold;   // Cosine similarity needed to name a speaker.

  bool is_video() const { return kind != "speaker"; }
};

struct NpuConfig {
  NpuConfig();

  std::string bus;  // "system" or "user".
  std::string log_level;
  std::string media_bus_name;
  std::string runtime;       // "cpu", "neuron" or "fake" (tests only).
  std::string onnx_library;  // cpu runtime: libonnxruntime path or soname.
  uint32_t cpu_threads;      // cpu runtime: intra-op threads, 0 = default.
  std::string allocator;     // Buffer source for tensor inputs.
  uint32_t media_queue_depth;
  std::string speaker_db_path;  // Empty keeps voice prints in memory only.
  std::string thermal_path;     // e.g. /sys/class/thermal/thermal_zone0/temp
  double throttle_temp_c;
  double throttle_rate_scale;  // Rate multiplier while throttled (0..1).
  std::vector<ModelConfig> models;
};

Status LoadNpuConfig(const std::string& path, NpuConfig* config);
Status ApplyNpuConfigOverride(const std::string& assignment, NpuConfig* config);
Status ValidateNpuConfig(const NpuConfig& config);

}  // namespace mw

#endif  // MW_NPU_NPU_CONFIG_H_
