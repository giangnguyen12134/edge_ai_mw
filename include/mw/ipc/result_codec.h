// Copyright 2026 Genio Middleware Authors.
//
// (De)serialisation of InferenceResult as the body of the Result signal:
// signature "sstta(sddddd)".

#ifndef MW_IPC_RESULT_CODEC_H_
#define MW_IPC_RESULT_CODEC_H_

#include "mw/common/inference_result.h"
#include "mw/common/status.h"

struct sd_bus_message;

namespace mw {

// Returns a negative errno on failure (sd-bus convention).
int AppendInferenceResult(sd_bus_message* message,
                          const InferenceResult& result);

Status ReadInferenceResult(sd_bus_message* message, InferenceResult* result);

}  // namespace mw

#endif  // MW_IPC_RESULT_CODEC_H_
