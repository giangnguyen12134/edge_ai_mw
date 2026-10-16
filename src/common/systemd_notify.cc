// Copyright 2026 Genio Middleware Authors.

#include "mw/common/systemd_notify.h"

#include <systemd/sd-daemon.h>

namespace mw {

void NotifyReady(const std::string& status) {
  const std::string text = "READY=1\nSTATUS=" + status;
  sd_notify(0, text.c_str());
}

void NotifyStopping() { sd_notify(0, "STOPPING=1"); }

void NotifyWatchdog() { sd_notify(0, "WATCHDOG=1"); }

uint64_t WatchdogIntervalUs() {
  uint64_t usec = 0;
  if (sd_watchdog_enabled(0, &usec) > 0) {
    return usec;
  }
  return 0;
}

}  // namespace mw
