// Copyright 2026 Genio Middleware Authors.

#include "mw/ipc/bus.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <future>

#include "mw/common/logging.h"
#include "mw/common/time_util.h"
#include "mw/ipc/dbus_names.h"

namespace mw {
namespace {

const char kDbusDriverName[] = "org.freedesktop.DBus";
const char kDbusDriverPath[] = "/org/freedesktop/DBus";

struct ObjectRegistration {
  std::string interface_name;
  std::string introspection_xml;
  Bus::ObjectHandler handler;
};

struct MatchRegistration {
  Bus::MessageHandler handler;
};

int ObjectTrampoline(sd_bus_message* message, void* userdata,
                     sd_bus_error* /*ret_error*/) {
  ObjectRegistration* reg = static_cast<ObjectRegistration*>(userdata);
  if (sd_bus_message_is_method_call(
          message, "org.freedesktop.DBus.Introspectable", "Introspect")) {
    sd_bus_reply_method_return(message, "s", reg->introspection_xml.c_str());
    return 1;
  }
  if (sd_bus_message_is_method_call(message, reg->interface_name.c_str(),
                                    nullptr)) {
    reg->handler(sd_bus_message_get_member(message), message);
    return 1;
  }
  return 0;  // Let sd-bus answer with UnknownMethod/UnknownInterface.
}

int MatchTrampoline(sd_bus_message* message, void* userdata,
                    sd_bus_error* /*ret_error*/) {
  MatchRegistration* reg = static_cast<MatchRegistration*>(userdata);
  reg->handler(message);
  return 0;  // Keep dispatching to other matches.
}

bool StatusCodeFromName(const std::string& name, Status::Code* code) {
  for (int c = Status::kInvalidArgument; c <= Status::kInternal; ++c) {
    if (name == StatusCodeName(static_cast<Status::Code>(c))) {
      *code = static_cast<Status::Code>(c);
      return true;
    }
  }
  return false;
}

Status StatusFromBusError(const sd_bus_error& error, int result) {
  if (error.name == nullptr) {
    return ErrnoStatus("D-Bus call", -result);
  }
  const std::string name = error.name;
  const std::string message =
      name + (error.message != nullptr ? std::string(": ") + error.message
                                       : std::string());
  const std::string prefix = dbus::kErrorPrefix;
  if (name.compare(0, prefix.size(), prefix) == 0) {
    Status::Code code = Status::kInternal;
    if (StatusCodeFromName(name.substr(prefix.size()), &code)) {
      return Status(code, message);
    }
  }
  if (name == SD_BUS_ERROR_SERVICE_UNKNOWN ||
      name == SD_BUS_ERROR_NAME_HAS_NO_OWNER || name == SD_BUS_ERROR_NO_REPLY ||
      name == SD_BUS_ERROR_DISCONNECTED || name == SD_BUS_ERROR_TIMEOUT) {
    return UnavailableError(message);
  }
  if (name == SD_BUS_ERROR_ACCESS_DENIED) {
    return PermissionDeniedError(message);
  }
  return InternalError(message);
}

int64_t MonotonicUs() { return static_cast<int64_t>(MonotonicNs() / 1000); }

}  // namespace

Status ParseBusType(const std::string& name, BusType* type) {
  if (name == "system") {
    *type = kBusSystem;
  } else if (name == "user") {
    *type = kBusUser;
  } else {
    return InvalidArgumentError("bus must be 'system' or 'user', got '" + name +
                                "'");
  }
  return Status::Ok();
}

Bus::Bus()
    : bus_(nullptr),
      started_(false),
      running_(false),
      stop_requested_(false),
      connection_lost_(false),
      accepting_tasks_(false) {}

Bus::~Bus() {
  Stop();
  for (size_t i = 0; i < slots_.size(); ++i) {
    sd_bus_slot_unref(slots_[i]);
  }
  slots_.clear();
  registrations_.clear();
  if (bus_ != nullptr) {
    sd_bus_flush_close_unref(bus_);
    bus_ = nullptr;
  }
}

Status Bus::Connect(BusType type, const std::string& well_known_name) {
  const int r = (type == kBusSystem) ? sd_bus_open_system(&bus_)
                                     : sd_bus_open_user(&bus_);
  if (r < 0) {
    return ErrnoStatus(
        type == kBusSystem ? "connect to system bus" : "connect to user bus",
        -r);
  }
  const char* unique = nullptr;
  if (sd_bus_get_unique_name(bus_, &unique) >= 0 && unique != nullptr) {
    unique_name_ = unique;
  }
  wake_fd_.Reset(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
  if (!wake_fd_.valid()) {
    return ErrnoStatus("eventfd", errno);
  }
  if (!well_known_name.empty()) {
    const int rn = sd_bus_request_name(bus_, well_known_name.c_str(), 0);
    if (rn < 0) {
      return ErrnoStatus("request bus name " + well_known_name, -rn);
    }
  }
  return Status::Ok();
}

Status Bus::Start() {
  if (bus_ == nullptr) {
    return FailedPreconditionError("Bus::Start before Connect");
  }
  if (started_) {
    return FailedPreconditionError("Bus already started");
  }
  started_ = true;
  running_ = true;
  stop_requested_ = false;
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    accepting_tasks_ = true;
  }
  std::promise<void> go;
  const std::shared_future<void> go_future = go.get_future().share();
  thread_ = std::thread([this, go_future]() {
    go_future.wait();
    Loop();
  });
  thread_id_ = thread_.get_id();
  go.set_value();
  return Status::Ok();
}

void Bus::Stop() {
  if (!started_) {
    return;
  }
  stop_requested_ = true;
  Wake();
  if (thread_.joinable() && !InBusThread()) {
    thread_.join();
  }
}

void Bus::Wake() {
  if (!wake_fd_.valid()) {
    return;
  }
  const uint64_t one = 1;
  const ssize_t ignored = write(wake_fd_.get(), &one, sizeof(one));
  (void)ignored;
}

bool Bus::InBusThread() const {
  return std::this_thread::get_id() == thread_id_;
}

bool Bus::Post(const std::function<void()>& task) {
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    if (!accepting_tasks_) {
      return false;
    }
    tasks_.push_back(task);
  }
  Wake();
  return true;
}

void Bus::RunTasks() {
  std::deque<std::function<void()>> batch;
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    batch.swap(tasks_);
  }
  for (size_t i = 0; i < batch.size(); ++i) {
    batch[i]();
  }
}

void Bus::Loop() {
  SetCurrentThreadName("mw-bus");
  while (!stop_requested_) {
    RunTasks();
    const int r = sd_bus_process(bus_, nullptr);
    if (r < 0) {
      MW_LOG(Error) << "sd_bus_process failed: " << strerror(-r);
      connection_lost_ = true;
      break;
    }
    if (r > 0) {
      continue;  // More work may be pending.
    }
    struct pollfd fds[2];
    fds[0].fd = sd_bus_get_fd(bus_);
    fds[0].events = static_cast<short>(sd_bus_get_events(bus_));
    fds[0].revents = 0;
    fds[1].fd = wake_fd_.get();
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    int timeout_ms = -1;
    uint64_t timeout_us = 0;
    if (sd_bus_get_timeout(bus_, &timeout_us) >= 0 &&
        timeout_us != UINT64_MAX) {
      const int64_t delta = static_cast<int64_t>(timeout_us) - MonotonicUs();
      timeout_ms = delta <= 0 ? 0 : static_cast<int>((delta + 999) / 1000);
    }
    if (poll(fds, 2, timeout_ms) < 0 && errno != EINTR) {
      MW_LOG(Error) << "poll failed: " << strerror(errno);
      connection_lost_ = true;
      break;
    }
    if (fds[1].revents & POLLIN) {
      uint64_t value = 0;
      const ssize_t ignored = read(wake_fd_.get(), &value, sizeof(value));
      (void)ignored;
    }
  }
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    accepting_tasks_ = false;
  }
  RunTasks();  // Complete anything already queued so waiters are released.
  running_ = false;
}

Status Bus::Invoke(const std::function<Status()>& fn) {
  if (!started_ || InBusThread()) {
    return fn();
  }
  std::shared_ptr<std::promise<Status>> promise(new std::promise<Status>());
  std::future<Status> future = promise->get_future();
  const bool posted = Post([promise, &fn]() { promise->set_value(fn()); });
  if (!posted) {
    return UnavailableError("bus is stopped");
  }
  return future.get();
}

Status Bus::AddObject(const std::string& path,
                      const std::string& interface_name,
                      const std::string& introspection_xml,
                      ObjectHandler handler) {
  return Invoke([&]() -> Status {
    std::shared_ptr<ObjectRegistration> reg(new ObjectRegistration());
    reg->interface_name = interface_name;
    reg->introspection_xml = introspection_xml;
    reg->handler = handler;
    sd_bus_slot* slot = nullptr;
    const int r = sd_bus_add_object(bus_, &slot, path.c_str(),
                                    &ObjectTrampoline, reg.get());
    if (r < 0) {
      return ErrnoStatus("register object " + path, -r);
    }
    slots_.push_back(slot);
    registrations_.push_back(reg);
    return Status::Ok();
  });
}

Status Bus::AddMatch(const std::string& rule, MessageHandler handler) {
  return Invoke([&]() -> Status {
    std::shared_ptr<MatchRegistration> reg(new MatchRegistration());
    reg->handler = handler;
    sd_bus_slot* slot = nullptr;
    const int r = sd_bus_add_match(bus_, &slot, rule.c_str(), &MatchTrampoline,
                                   reg.get());
    if (r < 0) {
      return ErrnoStatus("add match " + rule, -r);
    }
    slots_.push_back(slot);
    registrations_.push_back(reg);
    return Status::Ok();
  });
}

Status Bus::WatchName(const std::string& name, NameWatchHandler handler) {
  const std::string rule = std::string("type='signal',sender='") +
                           kDbusDriverName + "',path='" + kDbusDriverPath +
                           "',interface='" + kDbusDriverName +
                           "',member='NameOwnerChanged',arg0='" + name + "'";
  return AddMatch(rule, [handler](sd_bus_message* m) {
    const char* changed = nullptr;
    const char* old_owner = nullptr;
    const char* new_owner = nullptr;
    if (sd_bus_message_read(m, "sss", &changed, &old_owner, &new_owner) < 0) {
      return;
    }
    handler(new_owner != nullptr && new_owner[0] != '\0');
  });
}

Status Bus::WatchPeerExits(PeerGoneHandler handler) {
  const std::string rule = std::string("type='signal',sender='") +
                           kDbusDriverName + "',path='" + kDbusDriverPath +
                           "',interface='" + kDbusDriverName +
                           "',member='NameOwnerChanged'";
  return AddMatch(rule, [handler](sd_bus_message* m) {
    const char* name = nullptr;
    const char* old_owner = nullptr;
    const char* new_owner = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0) {
      return;
    }
    if (name != nullptr && name[0] == ':' && old_owner != nullptr &&
        old_owner[0] != '\0' &&
        (new_owner == nullptr || new_owner[0] == '\0')) {
      handler(name);
    }
  });
}

Status Bus::CallLocked(const std::string& destination, const std::string& path,
                       const std::string& interface_name,
                       const std::string& member, const FillFunction& fill,
                       const ReplyReader& read_reply, uint64_t timeout_us) {
  sd_bus_message* call = nullptr;
  int r = sd_bus_message_new_method_call(
      bus_, &call, destination.empty() ? nullptr : destination.c_str(),
      path.c_str(), interface_name.c_str(), member.c_str());
  if (r < 0) {
    return ErrnoStatus("create method call " + member, -r);
  }
  if (fill) {
    r = fill(call);
    if (r < 0) {
      sd_bus_message_unref(call);
      return ErrnoStatus("append arguments of " + member, -r);
    }
  }
  sd_bus_error error = {nullptr, nullptr, 0};
  sd_bus_message* reply = nullptr;
  r = sd_bus_call(bus_, call, timeout_us, &error, &reply);
  sd_bus_message_unref(call);
  if (r < 0) {
    const Status status = StatusFromBusError(error, r);
    sd_bus_error_free(&error);
    return status;
  }
  Status result;
  if (read_reply) {
    result = read_reply(reply);
  }
  sd_bus_message_unref(reply);
  return result;
}

Status Bus::CallMethod(const std::string& destination, const std::string& path,
                       const std::string& interface_name,
                       const std::string& member, const FillFunction& fill,
                       const ReplyReader& read_reply, uint64_t timeout_us) {
  return Invoke([&]() -> Status {
    return CallLocked(destination, path, interface_name, member, fill,
                      read_reply, timeout_us);
  });
}

void Bus::SendNoReply(const std::string& destination, const std::string& path,
                      const std::string& interface_name,
                      const std::string& member, const FillFunction& fill) {
  Post([this, destination, path, interface_name, member, fill]() {
    sd_bus_message* call = nullptr;
    int r = sd_bus_message_new_method_call(
        bus_, &call, destination.empty() ? nullptr : destination.c_str(),
        path.c_str(), interface_name.c_str(), member.c_str());
    if (r < 0) {
      return;
    }
    sd_bus_message_set_expect_reply(call, 0);
    if (fill) {
      r = fill(call);
    }
    if (r >= 0) {
      r = sd_bus_send(bus_, call, nullptr);
    }
    if (r < 0) {
      MW_LOG(Debug) << "send " << member << " failed: " << strerror(-r);
    }
    sd_bus_message_unref(call);
  });
}

void Bus::EmitSignal(const std::string& destination, const std::string& path,
                     const std::string& interface_name,
                     const std::string& member, const FillFunction& fill) {
  Post([this, destination, path, interface_name, member, fill]() {
    sd_bus_message* signal = nullptr;
    int r = sd_bus_message_new_signal(bus_, &signal, path.c_str(),
                                      interface_name.c_str(), member.c_str());
    if (r < 0) {
      return;
    }
    if (!destination.empty()) {
      r = sd_bus_message_set_destination(signal, destination.c_str());
    }
    if (r >= 0 && fill) {
      r = fill(signal);
    }
    if (r >= 0) {
      r = sd_bus_send(bus_, signal, nullptr);
    }
    if (r < 0) {
      MW_LOG(Debug) << "emit " << member << " failed: " << strerror(-r);
    }
    sd_bus_message_unref(signal);
  });
}

void Bus::ReplyOk(sd_bus_message* call) {
  sd_bus_reply_method_return(call, nullptr);
}

void Bus::ReplyStatus(sd_bus_message* call, const Status& status) {
  if (status.ok()) {
    ReplyOk(call);
    return;
  }
  const std::string name =
      std::string(dbus::kErrorPrefix) + StatusCodeName(status.code());
  sd_bus_error error = {nullptr, nullptr, 0};
  sd_bus_error_set(&error, name.c_str(), status.message().c_str());
  sd_bus_reply_method_error(call, &error);
  sd_bus_error_free(&error);
}

int AppendFd(sd_bus_message* message, int fd) {
  return sd_bus_message_append_basic(message, SD_BUS_TYPE_UNIX_FD, &fd);
}

Status ReadFdDup(sd_bus_message* message, ScopedFd* out) {
  int fd = -1;
  const int r = sd_bus_message_read_basic(message, SD_BUS_TYPE_UNIX_FD, &fd);
  if (r < 0) {
    return ErrnoStatus("read unix fd", -r);
  }
  if (r == 0) {
    return InvalidArgumentError("message holds no more file descriptors");
  }
  ScopedFd duplicate = ScopedFd::DuplicateFrom(fd);
  if (!duplicate.valid()) {
    return ErrnoStatus("dup received fd", errno);
  }
  *out = std::move(duplicate);
  return Status::Ok();
}

std::string SenderOf(sd_bus_message* message) {
  const char* sender = sd_bus_message_get_sender(message);
  return sender != nullptr ? sender : std::string();
}

}  // namespace mw
