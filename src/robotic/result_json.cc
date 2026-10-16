// Copyright 2026 Genio Middleware Authors.

#include "mw/robotic/result_json.h"

#include <math.h>
#include <stdio.h>

namespace mw {
namespace {

// JSON has no NaN/Infinity; map them to 0 so the line always parses.
std::string Number(double value) {
  if (!(value == value) || isinf(value)) {
    value = 0.0;
  }
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.4f", value);
  return buffer;
}

}  // namespace

std::string JsonEscape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buffer[8];
          snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

std::string InferenceResultToJson(const InferenceResult& result) {
  std::string out =
      "{\"type\":\"result\",\"model\":\"" + JsonEscape(result.model) +
      "\",\"kind\":\"" + JsonEscape(result.kind) +
      "\",\"seq\":" + std::to_string(result.sequence) +
      ",\"ts_ns\":" + std::to_string(result.timestamp_ns) + ",\"items\":[";
  for (size_t i = 0; i < result.items.size(); ++i) {
    const ResultItem& item = result.items[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"label\":\"" + JsonEscape(item.label) +
           "\",\"score\":" + Number(item.score);
    if (item.w > 0.0 && item.h > 0.0) {
      out += ",\"box\":[" + Number(item.x) + "," + Number(item.y) + "," +
             Number(item.w) + "," + Number(item.h) + "]";
    }
    out += "}";
  }
  out += "]}";
  return out;
}

}  // namespace mw
