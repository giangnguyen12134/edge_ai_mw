// Copyright 2026 Genio Middleware Authors.

#ifndef MW_COMMON_TIME_UTIL_H_
#define MW_COMMON_TIME_UTIL_H_

#include <stdint.h>

#include <string>

namespace mw {

// CLOCK_MONOTONIC in nanoseconds. All frame timestamps use this clock.
uint64_t MonotonicNs();

// Names the calling thread (truncated to 15 characters by the kernel).
void SetCurrentThreadName(const std::string& name);

}  // namespace mw

#endif  // MW_COMMON_TIME_UTIL_H_
