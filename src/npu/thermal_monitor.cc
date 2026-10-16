// Copyright 2026 Genio Middleware Authors.

#include "mw/npu/thermal_monitor.h"

#include <stdio.h>

#include "mw/common/logging.h"
#include "mw/common/time_util.h"

namespace mw {

const double ThermalMonitor::kHysteresisC = 3.0;

ThermalMonitor::ThermalMonitor(const std::string& path, double throttle_temp_c,
                               double throttle_rate_scale)
    : path_(path),
      throttle_temp_c_(throttle_temp_c),
      throttle_rate_scale_(throttle_rate_scale),
      last_temp_c_(-1.0),
      throttled_(false),
      last_sample_ns_(0) {}

double ThermalMonitor::RateScale() {
  if (path_.empty()) {
    return 1.0;
  }
  const uint64_t now = MonotonicNs();
  if (last_sample_ns_ == 0 || now - last_sample_ns_ >= 1000000000ull) {
    last_sample_ns_ = now;
    FILE* f = fopen(path_.c_str(), "r");
    long milli_c = 0;
    if (f != nullptr && fscanf(f, "%ld", &milli_c) == 1) {
      last_temp_c_ = static_cast<double>(milli_c) / 1000.0;
      if (!throttled_ && last_temp_c_ >= throttle_temp_c_) {
        throttled_ = true;
        MW_LOG(Warning) << "SoC at " << last_temp_c_
                        << " C: throttling model rates to "
                        << throttle_rate_scale_ * 100 << "%";
      } else if (throttled_ && last_temp_c_ < throttle_temp_c_ - kHysteresisC) {
        throttled_ = false;
        MW_LOG(Info) << "SoC cooled to " << last_temp_c_
                     << " C: model rates restored";
      }
    } else {
      last_temp_c_ = -1.0;  // Sensor unreadable: do not throttle blindly.
    }
    if (f != nullptr) {
      fclose(f);
    }
  }
  return throttled_ ? throttle_rate_scale_ : 1.0;
}

}  // namespace mw
