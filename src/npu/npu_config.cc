// Copyright 2026 Genio Middleware Authors.

#include "mw/npu/npu_config.h"

#include <stdlib.h>

#include <set>

#include "mw/common/json.h"
#include "mw/ipc/dbus_names.h"

namespace mw {
namespace {

void LoadModel(const JsonValue& json, ModelConfig* m) {
  m->name = json.GetString("name", m->name);
  m->kind = json.GetString("kind", m->kind);
  m->path = json.GetString("path", m->path);
  m->enabled = json.GetBool("enabled", m->enabled);
  const JsonValue* labels = json.Find("labels");
  if (labels != nullptr && labels->is_array()) {
    m->labels.clear();
    for (size_t i = 0; i < labels->array().size(); ++i) {
      if (labels->array()[i].is_string()) {
        m->labels.push_back(labels->array()[i].string_value());
      }
    }
  }
  m->target_fps = json.GetDouble("target_fps", m->target_fps);
  m->priority = json.GetInt("priority", m->priority);
  m->gated_by = json.GetString("gated_by", m->gated_by);
  m->gate_hold_ms =
      static_cast<uint32_t>(json.GetInt("gate_hold_ms", m->gate_hold_ms));
  m->input_width =
      static_cast<uint32_t>(json.GetInt("input_width", m->input_width));
  m->input_height =
      static_cast<uint32_t>(json.GetInt("input_height", m->input_height));
  m->input_layout = json.GetString("input_layout", m->input_layout);
  m->color_order = json.GetString("color_order", m->color_order);
  m->resize_mode = json.GetString("resize_mode", m->resize_mode);
  m->input_type = json.GetString("input_type", m->input_type);
  m->input_mean =
      static_cast<float>(json.GetDouble("input_mean", m->input_mean));
  m->input_scale =
      static_cast<float>(json.GetDouble("input_scale", m->input_scale));
  m->box_format = json.GetString("box_format", m->box_format);
  m->coords_normalized =
      json.GetBool("coords_normalized", m->coords_normalized);
  m->score_threshold =
      static_cast<float>(json.GetDouble("score_threshold", m->score_threshold));
  m->apply_softmax = json.GetBool("apply_softmax", m->apply_softmax);
  m->sample_rate =
      static_cast<uint32_t>(json.GetInt("sample_rate", m->sample_rate));
  m->window_ms = static_cast<uint32_t>(json.GetInt("window_ms", m->window_ms));
  m->min_speech_ms =
      static_cast<uint32_t>(json.GetInt("min_speech_ms", m->min_speech_ms));
  m->vad_threshold_db = static_cast<float>(
      json.GetDouble("vad_threshold_db", m->vad_threshold_db));
  m->match_threshold =
      static_cast<float>(json.GetDouble("match_threshold", m->match_threshold));
}

bool OneOf(const std::string& value, const char* a, const char* b,
           const char* c = nullptr) {
  return value == a || value == b || (c != nullptr && value == c);
}

}  // namespace

ModelConfig::ModelConfig()
    : enabled(true),
      target_fps(10),
      priority(0),
      gate_hold_ms(1000),
      input_width(0),
      input_height(0),
      input_layout("auto"),
      color_order("rgb"),
      resize_mode("stretch"),
      input_type("uint8"),
      input_mean(0),
      input_scale(1),
      box_format("xyxy"),
      coords_normalized(false),
      score_threshold(0.5f),
      apply_softmax(false),
      sample_rate(16000),
      window_ms(1500),
      min_speech_ms(400),
      vad_threshold_db(-40.0f),
      match_threshold(0.6f) {}

NpuConfig::NpuConfig()
    : bus("system"),
      log_level("info"),
      media_bus_name(dbus::kMediaBusName),
      runtime("cpu"),
      onnx_library("libonnxruntime.so"),
      cpu_threads(0),
      allocator("dma_heap:system"),
      media_queue_depth(2),
      throttle_temp_c(85.0),
      throttle_rate_scale(0.5) {}

Status LoadNpuConfig(const std::string& path, NpuConfig* config) {
  JsonValue root;
  MW_RETURN_IF_ERROR(JsonValue::ParseFile(path, &root));
  if (!root.is_object()) {
    return InvalidArgumentError(path + ": top level must be an object");
  }
  config->bus = root.GetString("bus", config->bus);
  config->log_level = root.GetString("log_level", config->log_level);
  config->media_bus_name =
      root.GetString("media_bus_name", config->media_bus_name);
  config->runtime = root.GetString("runtime", config->runtime);
  config->onnx_library = root.GetString("onnx_library", config->onnx_library);
  config->cpu_threads = static_cast<uint32_t>(
      root.GetInt("cpu_threads", static_cast<int>(config->cpu_threads)));
  config->allocator = root.GetString("allocator", config->allocator);
  config->media_queue_depth = static_cast<uint32_t>(
      root.GetInt("media_queue_depth", config->media_queue_depth));
  config->speaker_db_path =
      root.GetString("speaker_db_path", config->speaker_db_path);
  config->thermal_path = root.GetString("thermal_path", config->thermal_path);
  config->throttle_temp_c =
      root.GetDouble("throttle_temp_c", config->throttle_temp_c);
  config->throttle_rate_scale =
      root.GetDouble("throttle_rate_scale", config->throttle_rate_scale);
  const JsonValue* models = root.Find("models");
  if (models != nullptr) {
    if (!models->is_array()) {
      return InvalidArgumentError(path + ": 'models' must be an array");
    }
    config->models.clear();
    for (size_t i = 0; i < models->array().size(); ++i) {
      if (!models->array()[i].is_object()) {
        return InvalidArgumentError(path + ": each model must be an object");
      }
      ModelConfig model;
      LoadModel(models->array()[i], &model);
      config->models.push_back(model);
    }
  }
  return Status::Ok();
}

Status ApplyNpuConfigOverride(const std::string& assignment,
                              NpuConfig* config) {
  const size_t eq = assignment.find('=');
  if (eq == std::string::npos) {
    return InvalidArgumentError("override must look like key=value: '" +
                                assignment + "'");
  }
  const std::string key = assignment.substr(0, eq);
  const std::string value = assignment.substr(eq + 1);
  if (key == "bus") {
    config->bus = value;
  } else if (key == "log_level") {
    config->log_level = value;
  } else if (key == "runtime") {
    config->runtime = value;
  } else if (key == "onnx_library") {
    config->onnx_library = value;
  } else if (key == "cpu_threads") {
    char* end = nullptr;
    const unsigned long threads = strtoul(value.c_str(), &end, 10);
    if (value.empty() || *end != '\0' || threads > 64) {
      return InvalidArgumentError("cpu_threads must be 0..64");
    }
    config->cpu_threads = static_cast<uint32_t>(threads);
  } else if (key == "allocator") {
    config->allocator = value;
  } else if (key == "media_bus_name") {
    config->media_bus_name = value;
  } else if (key == "speaker_db_path") {
    config->speaker_db_path = value;
  } else {
    return InvalidArgumentError("unknown config key '" + key + "'");
  }
  return Status::Ok();
}

Status ValidateNpuConfig(const NpuConfig& c) {
  if (c.bus != "system" && c.bus != "user") {
    return InvalidArgumentError("bus must be 'system' or 'user'");
  }
  if (c.runtime != "cpu" && c.runtime != "fake" && c.runtime != "neuron") {
    return InvalidArgumentError("runtime must be 'cpu', 'fake' or 'neuron'");
  }
  if (c.runtime == "cpu" && c.onnx_library.empty()) {
    return InvalidArgumentError("onnx_library must not be empty");
  }
  if (c.cpu_threads > 64) {
    return InvalidArgumentError("cpu_threads must be 0..64");
  }
  if (c.media_queue_depth == 0 || c.media_queue_depth > 8) {
    return InvalidArgumentError("media_queue_depth must be 1..8");
  }
  if (c.throttle_rate_scale <= 0.0 || c.throttle_rate_scale > 1.0) {
    return InvalidArgumentError("throttle_rate_scale must be in (0, 1]");
  }
  std::set<std::string> video_names;
  std::set<std::string> all_names;
  int speaker_models = 0;
  for (size_t i = 0; i < c.models.size(); ++i) {
    const ModelConfig& m = c.models[i];
    const std::string who = "model '" + m.name + "': ";
    if (m.name.empty() || m.name.size() > 32) {
      return InvalidArgumentError("model names must be 1..32 characters");
    }
    if (!all_names.insert(m.name).second) {
      return InvalidArgumentError("duplicate model name '" + m.name + "'");
    }
    if (!OneOf(m.kind, "detection", "classification", "speaker")) {
      return InvalidArgumentError(who +
                                  "kind must be detection, "
                                  "classification or speaker");
    }
    if (c.runtime != "fake" && m.path.empty()) {
      return InvalidArgumentError(who + "path is required");
    }
    if (m.is_video()) {
      video_names.insert(m.name);
      if (m.input_width < 8 || m.input_height < 8 || m.input_width > 4096 ||
          m.input_height > 4096) {
        return InvalidArgumentError(who + "input size must be 8..4096");
      }
      if (m.target_fps <= 0.0 || m.target_fps > 120.0) {
        return InvalidArgumentError(who + "target_fps must be in (0, 120]");
      }
      if (!OneOf(m.color_order, "rgb", "bgr") ||
          !OneOf(m.resize_mode, "stretch", "letterbox") ||
          !OneOf(m.input_type, "uint8", "float32") ||
          !OneOf(m.input_layout, "auto", "nhwc", "nchw") ||
          !OneOf(m.box_format, "xyxy", "xywh")) {
        return InvalidArgumentError(who +
                                    "bad color_order, resize_mode, "
                                    "input_type, input_layout or box_format");
      }
    } else {
      ++speaker_models;
      if (m.sample_rate < 8000 || m.window_ms < 500 || m.window_ms > 10000 ||
          m.min_speech_ms > m.window_ms) {
        return InvalidArgumentError(who + "bad audio parameters");
      }
    }
  }
  if (speaker_models > 1) {
    return InvalidArgumentError("at most one speaker model is supported");
  }
  for (size_t i = 0; i < c.models.size(); ++i) {
    const ModelConfig& m = c.models[i];
    if (!m.gated_by.empty()) {
      if (!m.is_video() || video_names.count(m.gated_by) == 0 ||
          m.gated_by == m.name) {
        return InvalidArgumentError("model '" + m.name +
                                    "': gated_by must "
                                    "name another video model");
      }
    }
  }
  return Status::Ok();
}

}  // namespace mw
