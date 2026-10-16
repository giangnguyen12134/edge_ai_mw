// Copyright 2026 Genio Middleware Authors.

#include "mw/sdk/npu_client.h"

#include <systemd/sd-bus.h>

#include "mw/ipc/dbus_names.h"
#include "mw/ipc/result_codec.h"

namespace mw {
namespace {

const uint64_t kCallTimeoutUs = 5ull * 1000000ull;

}  // namespace

NpuClient::Options::Options()
    : bus("system"), npu_bus_name(dbus::kNpuBusName) {}

NpuClient::NpuClient(const Options& options)
    : options_(options), started_(false) {}

NpuClient::~NpuClient() { Stop(); }

Status NpuClient::Start(const ResultCallback& on_result) {
  if (started_) {
    return FailedPreconditionError("NpuClient already started");
  }
  on_result_ = on_result;
  BusType type;
  MW_RETURN_IF_ERROR(ParseBusType(options_.bus, &type));
  MW_RETURN_IF_ERROR(bus_.Connect(type, ""));
  const std::string rule = std::string("type='signal',sender='") +
                           options_.npu_bus_name + "',interface='" +
                           dbus::kNpuInterface + "',member='" +
                           dbus::kNpuResult + "'";
  MW_RETURN_IF_ERROR(bus_.AddMatch(rule, [this](sd_bus_message* m) {
    InferenceResult result;
    if (ReadInferenceResult(m, &result).ok() && on_result_) {
      on_result_(result);
    }
  }));
  MW_RETURN_IF_ERROR(bus_.Start());
  started_ = true;
  return Status::Ok();
}

void NpuClient::Stop() {
  if (started_) {
    bus_.Stop();
    started_ = false;
  }
}

Status NpuClient::Call(const std::string& member, const Bus::FillFunction& fill,
                       const Bus::ReplyReader& read) {
  if (!started_) {
    return FailedPreconditionError("NpuClient is not started");
  }
  return bus_.CallMethod(options_.npu_bus_name, dbus::kNpuObjectPath,
                         dbus::kNpuInterface, member, fill, read,
                         kCallTimeoutUs);
}

Status NpuClient::ListModels(std::vector<ModelInfo>* models) {
  models->clear();
  return Call(dbus::kNpuListModels, nullptr,
              [models](sd_bus_message* reply) -> Status {
                if (sd_bus_message_enter_container(reply, 'a', "(ssbd)") < 0) {
                  return InvalidArgumentError("malformed ListModels reply");
                }
                while (true) {
                  const char* name = nullptr;
                  const char* kind = nullptr;
                  int enabled = 0;
                  double fps = 0;
                  const int r = sd_bus_message_read(reply, "(ssbd)", &name,
                                                    &kind, &enabled, &fps);
                  if (r < 0) {
                    return InvalidArgumentError("malformed model entry");
                  }
                  if (r == 0) {
                    break;
                  }
                  ModelInfo info;
                  info.name = name;
                  info.kind = kind;
                  info.enabled = enabled != 0;
                  info.target_fps = fps;
                  models->push_back(info);
                }
                sd_bus_message_exit_container(reply);
                return Status::Ok();
              });
}

Status NpuClient::SetModelEnabled(const std::string& name, bool enabled) {
  return Call(
      dbus::kNpuSetModelEnabled,
      [&](sd_bus_message* m) {
        return sd_bus_message_append(m, "sb", name.c_str(), enabled ? 1 : 0);
      },
      nullptr);
}

Status NpuClient::EnrollSpeaker(const std::string& name) {
  return Call(
      dbus::kNpuEnrollSpeaker,
      [&](sd_bus_message* m) {
        return sd_bus_message_append(m, "s", name.c_str());
      },
      nullptr);
}

Status NpuClient::RemoveSpeaker(const std::string& name) {
  return Call(
      dbus::kNpuRemoveSpeaker,
      [&](sd_bus_message* m) {
        return sd_bus_message_append(m, "s", name.c_str());
      },
      nullptr);
}

Status NpuClient::ListSpeakers(std::vector<std::string>* names) {
  names->clear();
  return Call(dbus::kNpuListSpeakers, nullptr,
              [names](sd_bus_message* reply) -> Status {
                if (sd_bus_message_enter_container(reply, 'a', "s") < 0) {
                  return InvalidArgumentError("malformed ListSpeakers reply");
                }
                const char* name = nullptr;
                int r = 0;
                while ((r = sd_bus_message_read(reply, "s", &name)) > 0) {
                  names->push_back(name);
                }
                if (r < 0) {
                  return InvalidArgumentError("malformed speaker entry");
                }
                sd_bus_message_exit_container(reply);
                return Status::Ok();
              });
}

Status NpuClient::GetStats(
    std::vector<std::pair<std::string, uint64_t>>* stats) {
  stats->clear();
  return Call(
      dbus::kNpuGetStats, nullptr, [stats](sd_bus_message* reply) -> Status {
        if (sd_bus_message_enter_container(reply, 'a', "(st)") < 0) {
          return InvalidArgumentError("malformed GetStats reply");
        }
        while (true) {
          const char* key = nullptr;
          uint64_t value = 0;
          const int r = sd_bus_message_read(reply, "(st)", &key, &value);
          if (r < 0) {
            return InvalidArgumentError("malformed stat entry");
          }
          if (r == 0) {
            break;
          }
          stats->push_back(std::make_pair(std::string(key), value));
        }
        sd_bus_message_exit_container(reply);
        return Status::Ok();
      });
}

}  // namespace mw
