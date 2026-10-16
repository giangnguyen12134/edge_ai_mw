// Copyright 2026 VMX Middleware Team.

#include "mw/media/media_config.h"

#include <stdlib.h>

#include "mw/common/frame_types.h"
#include "mw/common/json.h"
#include "mw/common/logging.h"

namespace mw {
namespace {

const uint32_t kMaxPoolSlots = 16;  // Keeps the fd array under D-Bus limits.

Status ParseUint(const std::string& text, uint32_t* out) {
  char* end = nullptr;
  const unsigned long value = strtoul(text.c_str(), &end, 10);
  if (text.empty() || *end != '\0') {
    return InvalidArgumentError("not an unsigned integer: '" + text + "'");
  }
  *out = static_cast<uint32_t>(value);
  return Status::Ok();
}

Status ParseBool(const std::string& text, bool* out) {
  if (text == "true" || text == "1") {
    *out = true;
  } else if (text == "false" || text == "0") {
    *out = false;
  } else {
    return InvalidArgumentError("not a boolean: '" + text + "'");
  }
  return Status::Ok();
}

Status LoadVideo(const JsonValue& json, VideoStreamConfig* c) {
  c->id = json.GetString("id", c->id);
  c->source = json.GetString("source", c->source);
  c->device = json.GetString("device", c->device);
  c->width = static_cast<uint32_t>(json.GetInt("width", c->width));
  c->height = static_cast<uint32_t>(json.GetInt("height", c->height));
  c->fps = static_cast<uint32_t>(json.GetInt("fps", c->fps));
  c->v4l2_buffers =
      static_cast<uint32_t>(json.GetInt("v4l2_buffers", c->v4l2_buffers));
  c->pool_slots =
      static_cast<uint32_t>(json.GetInt("pool_slots", c->pool_slots));
  c->allocator = json.GetString("allocator", c->allocator);
  c->pool_format = json.GetString("pool_format", c->pool_format);
  c->capture_io = json.GetString("capture_io", c->capture_io);
  c->stall_timeout_ms = static_cast<uint32_t>(
      json.GetInt("stall_timeout_ms", c->stall_timeout_ms));
  c->test_unplug_after_frames = static_cast<uint32_t>(
      json.GetInt("test_unplug_after_frames", c->test_unplug_after_frames));
  const std::string format = json.GetString("pixel_format", "");
  if (!format.empty() && !ParsePixelFormat(format, &c->pixel_format)) {
    return InvalidArgumentError(
        "video.pixel_format must be NV12, YUYV or "
        "MJPEG, got '" +
        format + "'");
  }
  return Status::Ok();
}

void LoadAudio(const JsonValue& json, AudioStreamConfig* c) {
  c->enabled = json.GetBool("enabled", c->enabled);
  c->id = json.GetString("id", c->id);
  c->source = json.GetString("source", c->source);
  c->device = json.GetString("device", c->device);
  c->sample_rate =
      static_cast<uint32_t>(json.GetInt("sample_rate", c->sample_rate));
  c->channels = static_cast<uint32_t>(json.GetInt("channels", c->channels));
  c->ring_ms = static_cast<uint32_t>(json.GetInt("ring_ms", c->ring_ms));
  c->period_ms = static_cast<uint32_t>(json.GetInt("period_ms", c->period_ms));
}

}  // namespace

VideoStreamConfig::VideoStreamConfig()
    : id("camera0"),
      source("v4l2"),
      device("/dev/video0"),
      width(1280),
      height(720),
      fps(30),
      pixel_format(kFourccMjpeg),
      v4l2_buffers(4),
      pool_slots(8),
      pool_format("native"),
      capture_io("auto"),
      allocator("dma_heap:system"),
      stall_timeout_ms(3000),
      test_unplug_after_frames(0) {}

AudioStreamConfig::AudioStreamConfig()
    : enabled(true),
      id("mic0"),
      source("alsa"),
      device("hw:1,0"),
      sample_rate(16000),
      channels(1),
      ring_ms(3000),
      period_ms(20) {}

MediaConfig::MediaConfig() : bus("system"), log_level("info") {}

Status LoadMediaConfig(const std::string& path, MediaConfig* config) {
  JsonValue root;
  MW_RETURN_IF_ERROR(JsonValue::ParseFile(path, &root));
  if (!root.is_object()) {
    return InvalidArgumentError(path + ": top level must be an object");
  }
  config->bus = root.GetString("bus", config->bus);
  config->log_level = root.GetString("log_level", config->log_level);
  const JsonValue* video = root.Find("video");
  if (video != nullptr) {
    MW_RETURN_IF_ERROR(LoadVideo(*video, &config->video));
  }
  const JsonValue* audio = root.Find("audio");
  if (audio != nullptr) {
    LoadAudio(*audio, &config->audio);
  }
  return Status::Ok();
}

Status ApplyMediaConfigOverride(const std::string& assignment,
                                MediaConfig* config) {
  const size_t eq = assignment.find('=');
  if (eq == std::string::npos) {
    return InvalidArgumentError("override must look like key=value: '" +
                                assignment + "'");
  }
  const std::string key = assignment.substr(0, eq);
  const std::string value = assignment.substr(eq + 1);
  VideoStreamConfig* v = &config->video;
  AudioStreamConfig* a = &config->audio;
  if (key == "bus") {
    config->bus = value;
  } else if (key == "log_level") {
    config->log_level = value;
  } else if (key == "video.id") {
    v->id = value;
  } else if (key == "video.source") {
    v->source = value;
  } else if (key == "video.device") {
    v->device = value;
  } else if (key == "video.width") {
    return ParseUint(value, &v->width);
  } else if (key == "video.height") {
    return ParseUint(value, &v->height);
  } else if (key == "video.fps") {
    return ParseUint(value, &v->fps);
  } else if (key == "video.pixel_format") {
    if (!ParsePixelFormat(value, &v->pixel_format)) {
      return InvalidArgumentError("unknown pixel format '" + value + "'");
    }
  } else if (key == "video.v4l2_buffers") {
    return ParseUint(value, &v->v4l2_buffers);
  } else if (key == "video.pool_slots") {
    return ParseUint(value, &v->pool_slots);
  } else if (key == "video.allocator") {
    v->allocator = value;
  } else if (key == "video.pool_format") {
    v->pool_format = value;
  } else if (key == "video.capture_io") {
    v->capture_io = value;
  } else if (key == "video.stall_timeout_ms") {
    return ParseUint(value, &v->stall_timeout_ms);
  } else if (key == "video.test_unplug_after_frames") {
    return ParseUint(value, &v->test_unplug_after_frames);
  } else if (key == "audio.enabled") {
    return ParseBool(value, &a->enabled);
  } else if (key == "audio.source") {
    a->source = value;
  } else if (key == "audio.device") {
    a->device = value;
  } else if (key == "audio.sample_rate") {
    return ParseUint(value, &a->sample_rate);
  } else if (key == "audio.channels") {
    return ParseUint(value, &a->channels);
  } else if (key == "audio.ring_ms") {
    return ParseUint(value, &a->ring_ms);
  } else if (key == "audio.period_ms") {
    return ParseUint(value, &a->period_ms);
  } else {
    return InvalidArgumentError("unknown config key '" + key + "'");
  }
  return Status::Ok();
}

Status ValidateMediaConfig(const MediaConfig& c) {
  if (c.bus != "system" && c.bus != "user") {
    return InvalidArgumentError("bus must be 'system' or 'user'");
  }
  const VideoStreamConfig& v = c.video;
  if (v.source != "v4l2" && v.source != "test_pattern") {
    return InvalidArgumentError(
        "video.source must be 'v4l2' or 'test_pattern'");
  }
  if (v.pool_format != "native" && v.pool_format != "nv12") {
    return InvalidArgumentError("video.pool_format must be 'native' or 'nv12'");
  }
  if (v.capture_io != "auto" && v.capture_io != "expbuf" &&
      v.capture_io != "dmabuf") {
    return InvalidArgumentError(
        "video.capture_io must be 'auto', 'expbuf' or 'dmabuf'");
  }
  if (v.width < 64 || v.height < 64 || v.width > 7680 || v.height > 4320 ||
      (v.width % 2) != 0 || (v.height % 2) != 0) {
    return InvalidArgumentError(
        "video size must be even and within 64..7680 x "
        "64..4320");
  }
  if (v.fps == 0 || v.fps > 240) {
    return InvalidArgumentError("video.fps must be 1..240");
  }
  if (v.pool_slots < 3 || v.pool_slots > kMaxPoolSlots) {
    return InvalidArgumentError("video.pool_slots must be 3..16");
  }
  if (v.v4l2_buffers < 2 || v.v4l2_buffers > 32) {
    return InvalidArgumentError("video.v4l2_buffers must be 2..32");
  }
  if (v.stall_timeout_ms < 500) {
    return InvalidArgumentError("video.stall_timeout_ms must be >= 500");
  }
  const AudioStreamConfig& a = c.audio;
  if (a.enabled) {
    if (a.source != "alsa" && a.source != "sine") {
      return InvalidArgumentError("audio.source must be 'alsa' or 'sine'");
    }
    if (a.sample_rate < 8000 || a.sample_rate > 96000 || a.channels == 0 ||
        a.channels > 8) {
      return InvalidArgumentError("unsupported audio format");
    }
    if (a.period_ms < 5 || a.period_ms > 200 || a.ring_ms < 4 * a.period_ms) {
      return InvalidArgumentError(
          "audio.period_ms 5..200 and ring_ms >= "
          "4 * period_ms required");
    }
  }
  return Status::Ok();
}

}  // namespace mw
