// Copyright 2026 Genio Middleware Authors.

#include "mw/ipc/result_codec.h"

#include <systemd/sd-bus.h>

namespace mw {
namespace {

const size_t kMaxItems = 1024;

}  // namespace

int AppendInferenceResult(sd_bus_message* m, const InferenceResult& result) {
  int r = sd_bus_message_append(m, "sstt", result.model.c_str(),
                                result.kind.c_str(),
                                static_cast<uint64_t>(result.sequence),
                                static_cast<uint64_t>(result.timestamp_ns));
  if (r < 0) {
    return r;
  }
  r = sd_bus_message_open_container(m, 'a', "(sddddd)");
  if (r < 0) {
    return r;
  }
  for (size_t i = 0; i < result.items.size() && i < kMaxItems; ++i) {
    const ResultItem& item = result.items[i];
    r = sd_bus_message_append(m, "(sddddd)", item.label.c_str(), item.score,
                              item.x, item.y, item.w, item.h);
    if (r < 0) {
      return r;
    }
  }
  return sd_bus_message_close_container(m);
}

Status ReadInferenceResult(sd_bus_message* m, InferenceResult* result) {
  const char* model = nullptr;
  const char* kind = nullptr;
  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
  int r =
      sd_bus_message_read(m, "sstt", &model, &kind, &sequence, &timestamp_ns);
  if (r < 0) {
    return InvalidArgumentError("malformed Result header");
  }
  result->model = model != nullptr ? model : "";
  result->kind = kind != nullptr ? kind : "";
  result->sequence = sequence;
  result->timestamp_ns = timestamp_ns;
  result->items.clear();
  r = sd_bus_message_enter_container(m, 'a', "(sddddd)");
  if (r < 0) {
    return InvalidArgumentError("malformed Result items");
  }
  while (result->items.size() < kMaxItems) {
    const char* label = nullptr;
    double score = 0, x = 0, y = 0, w = 0, h = 0;
    r = sd_bus_message_read(m, "(sddddd)", &label, &score, &x, &y, &w, &h);
    if (r < 0) {
      return InvalidArgumentError("malformed Result item");
    }
    if (r == 0) {
      break;
    }
    result->items.push_back(
        ResultItem(label != nullptr ? label : "", score, x, y, w, h));
  }
  sd_bus_message_exit_container(m);
  return Status::Ok();
}

}  // namespace mw
