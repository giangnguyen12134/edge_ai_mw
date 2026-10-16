// Copyright 2026 Genio Middleware Authors.

#include "mw/robotic/robotic_config.h"

#include <stdlib.h>

#include "mw/common/json.h"
#include "mw/ipc/dbus_names.h"

namespace mw {
namespace {

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

uint32_t GetUint(const JsonValue& json, const char* key, uint32_t fallback) {
  return static_cast<uint32_t>(json.GetInt(key, static_cast<int>(fallback)));
}

}  // namespace

RoboticConfig::RoboticConfig()
    : bus("system"),
      log_level("info"),
      media_bus_name(dbus::kMediaBusName),
      npu_bus_name(dbus::kNpuBusName),
      jetson_host("192.168.55.1"),
      results_port(9100),
      video_port(9101),
      connect_timeout_ms(1000),
      send_timeout_ms(1000),
      reconnect_ms(2000),
      send_results(true),
      send_video(false),
      video_fps(15),
      jpeg_quality(80),
      encoder("libjpeg") {}

Status LoadRoboticConfig(const std::string& path, RoboticConfig* c) {
  JsonValue root;
  MW_RETURN_IF_ERROR(JsonValue::ParseFile(path, &root));
  if (!root.is_object()) {
    return InvalidArgumentError(path + ": top level must be an object");
  }
  c->bus = root.GetString("bus", c->bus);
  c->log_level = root.GetString("log_level", c->log_level);
  c->media_bus_name = root.GetString("media_bus_name", c->media_bus_name);
  c->npu_bus_name = root.GetString("npu_bus_name", c->npu_bus_name);
  c->send_results = root.GetBool("send_results", c->send_results);
  c->send_video = root.GetBool("send_video", c->send_video);
  const JsonValue* jetson = root.Find("jetson");
  if (jetson != nullptr) {
    c->jetson_host = jetson->GetString("host", c->jetson_host);
    c->results_port = GetUint(*jetson, "results_port", c->results_port);
    c->video_port = GetUint(*jetson, "video_port", c->video_port);
    c->connect_timeout_ms =
        GetUint(*jetson, "connect_timeout_ms", c->connect_timeout_ms);
    c->send_timeout_ms =
        GetUint(*jetson, "send_timeout_ms", c->send_timeout_ms);
    c->reconnect_ms = GetUint(*jetson, "reconnect_ms", c->reconnect_ms);
  }
  const JsonValue* video = root.Find("video");
  if (video != nullptr) {
    c->video_fps = GetUint(*video, "fps", c->video_fps);
    c->jpeg_quality = GetUint(*video, "jpeg_quality", c->jpeg_quality);
    c->encoder = video->GetString("encoder", c->encoder);
  }
  return Status::Ok();
}

Status ApplyRoboticConfigOverride(const std::string& assignment,
                                  RoboticConfig* c) {
  const size_t eq = assignment.find('=');
  if (eq == std::string::npos) {
    return InvalidArgumentError("override must look like key=value: '" +
                                assignment + "'");
  }
  const std::string key = assignment.substr(0, eq);
  const std::string value = assignment.substr(eq + 1);
  if (key == "bus") {
    c->bus = value;
  } else if (key == "log_level") {
    c->log_level = value;
  } else if (key == "jetson.host") {
    c->jetson_host = value;
  } else if (key == "jetson.results_port") {
    return ParseUint(value, &c->results_port);
  } else if (key == "jetson.video_port") {
    return ParseUint(value, &c->video_port);
  } else if (key == "send_results") {
    return ParseBool(value, &c->send_results);
  } else if (key == "send_video") {
    return ParseBool(value, &c->send_video);
  } else if (key == "video.fps") {
    return ParseUint(value, &c->video_fps);
  } else if (key == "video.jpeg_quality") {
    return ParseUint(value, &c->jpeg_quality);
  } else {
    return InvalidArgumentError("unknown config key '" + key + "'");
  }
  return Status::Ok();
}

Status ValidateRoboticConfig(const RoboticConfig& c) {
  if (c.bus != "system" && c.bus != "user") {
    return InvalidArgumentError("bus must be 'system' or 'user'");
  }
  if (c.jetson_host.empty()) {
    return InvalidArgumentError("jetson.host must not be empty");
  }
  if (!c.send_results && !c.send_video) {
    return InvalidArgumentError(
        "nothing to do: enable send_results and/or "
        "send_video");
  }
  if (c.results_port == 0 || c.results_port > 65535 || c.video_port == 0 ||
      c.video_port > 65535) {
    return InvalidArgumentError("ports must be 1..65535");
  }
  if (c.send_results && c.send_video && c.results_port == c.video_port) {
    return InvalidArgumentError("results_port and video_port must differ");
  }
  if (c.connect_timeout_ms < 100 || c.send_timeout_ms < 100 ||
      c.reconnect_ms < 100) {
    return InvalidArgumentError("timeouts must be >= 100 ms");
  }
  if (c.video_fps == 0 || c.video_fps > 60) {
    return InvalidArgumentError("video.fps must be 1..60");
  }
  if (c.jpeg_quality < 1 || c.jpeg_quality > 100) {
    return InvalidArgumentError("video.jpeg_quality must be 1..100");
  }
  return Status::Ok();
}

}  // namespace mw
