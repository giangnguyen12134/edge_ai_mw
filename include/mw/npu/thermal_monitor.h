// Copyright 2026 Genio Middleware Authors.
//
// Reads a sysfs thermal zone and tells the scheduler to slow models down when
// the SoC runs hot. Hysteresis avoids flapping around the threshold.

#ifndef MW_NPU_THERMAL_MONITOR_H_
#define MW_NPU_THERMAL_MONITOR_H_

#include <stdint.h>

#include <string>

#include "mw/common/macros.h"

namespace mw {

class ThermalMonitor {
 public:
  // An empty |path| disables the monitor (scale is always 1.0).
  ThermalMonitor(const std::string& path, double throttle_temp_c,
                 double throttle_rate_scale);

  MW_DISALLOW_COPY_AND_ASSIGN(ThermalMonitor);

  // Rate multiplier in (0, 1]. Samples the sensor at most once per second.
  // Call from one thread only.
  double RateScale();

  // Last temperature read, or a negative value if unavailable.
  double last_temp_c() const { return last_temp_c_; }
  bool throttled() const { return throttled_; }

 private:
  static const double kHysteresisC;

  std::string path_;
  double throttle_temp_c_;
  double throttle_rate_scale_;
  double last_temp_c_;
  bool throttled_;
  uint64_t last_sample_ns_;
};

}  // namespace mw

#endif  // MW_NPU_THERMAL_MONITOR_H_
