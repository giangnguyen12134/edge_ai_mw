// Copyright 2026 Genio Middleware Authors.
//
// mw-robotic-bridge (optional): relays NPU results and/or camera video to the
// Jetson Orin Nano over USB networking.
//
//   mw-robotic-bridge [--config FILE] [--set key=value]... [--check-config]
//                  [--log-config FILE]

#include <stdio.h>

#include <algorithm>
#include <string>
#include <vector>

#include "mw/common/logging.h"
#include "mw/common/systemd_notify.h"
#include "mw/common/termination_watcher.h"
#include "mw/robotic/robotic_bridge.h"
#include "mw/robotic/robotic_config.h"

namespace {

void PrintUsage(const char* program) {
  fprintf(stderr,
          "Usage: %s [--config FILE] [--set key=value]... [--check-config]\n"
          "       [--log-config FILE]\n"
          "  --config FILE     JSON configuration\n"
          "  --set key=value   override, e.g. jetson.host=192.168.55.1 or\n"
          "                    send_video=true\n"
          "  --check-config    validate the configuration and exit\n"
          "  --log-config FILE logging sinks (default: $MW_LOG_CONFIG, then\n"
          "                    /etc/genio-mw/logging.json, then stderr)\n",
          program);
}

}  // namespace

int main(int argc, char** argv) {
  mw::TerminationWatcher termination;

  std::string config_path;
  std::string log_config_path;
  std::vector<std::string> overrides;
  bool check_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--set" && i + 1 < argc) {
      overrides.push_back(argv[++i]);
    } else if (arg == "--log-config" && i + 1 < argc) {
      log_config_path = argv[++i];
    } else if (arg == "--check-config") {
      check_only = true;
    } else {
      PrintUsage(argv[0]);
      return 2;
    }
  }
  if (!mw::SetUpLogging(log_config_path, "mw-robotic-bridge")) {
    return 2;
  }

  mw::RoboticConfig config;
  if (!config_path.empty()) {
    const mw::Status status = mw::LoadRoboticConfig(config_path, &config);
    if (!status.ok()) {
      MW_LOG(Error) << status.ToString();
      return 2;
    }
  }
  for (size_t i = 0; i < overrides.size(); ++i) {
    const mw::Status status =
        mw::ApplyRoboticConfigOverride(overrides[i], &config);
    if (!status.ok()) {
      MW_LOG(Error) << status.ToString();
      return 2;
    }
  }
  mw::SetMinLogSeverityFromString(config.log_level);
  const mw::Status valid = mw::ValidateRoboticConfig(config);
  if (!valid.ok()) {
    MW_LOG(Error) << "invalid configuration: " << valid.ToString();
    return 2;
  }
  if (check_only) {
    MW_LOG(Info) << "configuration OK";
    return 0;
  }

  mw::RoboticBridge bridge(config);
  const mw::Status started = bridge.Start();
  if (!started.ok()) {
    MW_LOG(Error) << "cannot start: " << started.ToString();
    return 1;
  }
  mw::NotifyReady("relaying");

  const uint64_t watchdog_us = mw::WatchdogIntervalUs();
  const int tick_ms = watchdog_us > 0
                          ? std::max(100, static_cast<int>(watchdog_us / 2000))
                          : 1000;
  while (!termination.Wait(tick_ms)) {
    if (bridge.Healthy()) {
      mw::NotifyWatchdog();
    } else {
      MW_LOG(Error) << "bridge thread stalled; watchdog ping withheld";
    }
  }
  mw::NotifyStopping();
  bridge.Stop();
  return 0;
}
