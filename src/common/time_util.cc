// Copyright 2026 Genio Middleware Authors.

#include "mw/common/time_util.h"

#include <pthread.h>
#include <time.h>

namespace mw {

uint64_t MonotonicNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

void SetCurrentThreadName(const std::string& name) {
  pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}

}  // namespace mw
