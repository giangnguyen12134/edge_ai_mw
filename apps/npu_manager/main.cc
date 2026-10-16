// Copyright 2026 Genio Middleware Authors.
//
// mw-npu-manager: NPU owner, model scheduler and inference result publisher.
//
//   mw-npu-manager [--config FILE] [--set key=value]... [--check-config]
//                  [--log-config FILE]

#include <stdio.h>

#include <algorithm>
#include <string>
#include <vector>

#include "mw/common/logging.h"
#include "mw/common/systemd_notify.h"
#include "mw/common/termination_watcher.h"
#include "mw/npu/npu_config.h"
#include "mw/npu/npu_service.h"

namespace {

void PrintUsage(const char* program) {
  fprintf(stderr,
          "Usage: %s [--config FILE] [--set key=value]... [--check-config]\n"
          "       [--log-config FILE]\n"
          "  --config FILE     JSON configuration (models, scheduling)\n"
          "  --set key=value   override bus, runtime, allocator, log_level,\n"
          "                    media_bus_name or speaker_db_path\n"
          "  --check-config    validate the configuration and exit\n"
          "  --log-config FILE logging sinks (default: $MW_LOG_CONFIG, then\n"
          "                    /etc/vmx-mw/logging.json, then stderr)\n",
          program);
}

}  // namespace

int main(int argc, char** argv) {
  // Must come first: blocks SIGINT/SIGTERM in every thread created later.
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
  if (!mw::SetUpLogging(log_config_path, "mw-npu-manager")) {
    return 2;
  }

  mw::NpuConfig config;
  if (!config_path.empty()) {
    const mw::Status status = mw::LoadNpuConfig(config_path, &config);
    if (!status.ok()) {
      MW_LOG(Error) << status.ToString();
      return 2;
    }
  }
  for (size_t i = 0; i < overrides.size(); ++i) {
    const mw::Status status = mw::ApplyNpuConfigOverride(overrides[i], &config);
    if (!status.ok()) {
      MW_LOG(Error) << status.ToString();
      return 2;
    }
  }
  mw::SetMinLogSeverityFromString(config.log_level);
  const mw::Status valid = mw::ValidateNpuConfig(config);
  if (!valid.ok()) {
    MW_LOG(Error) << "invalid configuration: " << valid.ToString();
    return 2;
  }
  if (check_only) {
    MW_LOG(Info) << "configuration OK (" << config.models.size() << " models)";
    return 0;
  }

  mw::NpuService service(config);
  const mw::Status started = service.Start();
  if (!started.ok()) {
    MW_LOG(Error) << "cannot start: " << started.ToString();
    return 1;
  }
  mw::NotifyReady("running");

  const uint64_t watchdog_us = mw::WatchdogIntervalUs();
  const int tick_ms = watchdog_us > 0
                          ? std::max(100, static_cast<int>(watchdog_us / 2000))
                          : 1000;
  int exit_code = 0;
  while (!termination.Wait(tick_ms)) {
    if (service.bus_lost()) {
      MW_LOG(Error) << "lost the D-Bus connection; exiting for restart";
      exit_code = 1;
      break;
    }
    // Withholding the ping lets systemd restart a hung NPU or worker.
    if (service.Healthy()) {
      mw::NotifyWatchdog();
    } else {
      MW_LOG(Error) << "inference thread stalled; watchdog ping withheld";
    }
  }
  mw::NotifyStopping();
  service.Stop();
  return exit_code;
}
