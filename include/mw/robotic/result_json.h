// Copyright 2026 Genio Middleware Authors.
//
// Wire format of results sent to the Jetson: one JSON object per line.
//
//   {"type":"result","model":"face","kind":"detection","seq":1234,
//    "ts_ns":987654321,"items":[{"label":"face","score":0.9100,
//    "box":[0.1000,0.2000,0.3000,0.4000]}]}
//
// "box" is [x, y, w, h] normalised to the camera frame, and is omitted for
// non-spatial results (gesture class, speaker identity).

#ifndef MW_ROBOTIC_RESULT_JSON_H_
#define MW_ROBOTIC_RESULT_JSON_H_

#include <string>

#include "mw/common/inference_result.h"

namespace mw {

// Serialises |result| as one line WITHOUT the trailing newline.
std::string InferenceResultToJson(const InferenceResult& result);

// Escapes |text| as a JSON string body (no surrounding quotes).
std::string JsonEscape(const std::string& text);

}  // namespace mw

#endif  // MW_ROBOTIC_RESULT_JSON_H_
