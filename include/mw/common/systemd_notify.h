// Copyright 2026 Genio Middleware Authors.
//
// Thin wrappers over sd_notify for Type=notify services with a watchdog. All
// functions are harmless no-ops when not started by systemd.

#ifndef MW_COMMON_SYSTEMD_NOTIFY_H_
#define MW_COMMON_SYSTEMD_NOTIFY_H_

#include <stdint.h>

#include <string>

namespace mw {

void NotifyReady(const std::string& status);
void NotifyStopping();
void NotifyWatchdog();

// Watchdog period configured by WatchdogSec= in microseconds, or 0 when the
// watchdog is disabled.
uint64_t WatchdogIntervalUs();

}  // namespace mw

#endif  // MW_COMMON_SYSTEMD_NOTIFY_H_
