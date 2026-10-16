// Copyright 2026 Genio Middleware Authors.
//
// Thread-confined wrapper around sd-bus (libsystemd).
//
// sd-bus is not thread safe, so every sd-bus call made through this class is
// executed on one dedicated "bus thread". Other threads interact through
// Post(), CallMethod(), EmitSignal() and SendNoReply(), which are thread safe.
// All handlers run on the bus thread and must return quickly.
//
// File descriptors (D-Bus type 'h') are passed with AppendFd() / ReadFdDup().
// sd-bus duplicates the descriptor into the message, so the sender keeps
// ownership of its copy.

#ifndef MW_IPC_BUS_H_
#define MW_IPC_BUS_H_

#include <stdint.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mw/common/macros.h"
#include "mw/common/scoped_fd.h"
#include "mw/common/status.h"

struct sd_bus;
struct sd_bus_message;
struct sd_bus_slot;

namespace mw {

enum BusType {
  kBusSystem,
  kBusUser,
};

// Accepts "system" or "user".
Status ParseBusType(const std::string& name, BusType* type);

class Bus {
 public:
  // Called for every method call on a registered object. The handler MUST
  // send a reply (ReplyOk / ReplyStatus) unless the caller set the NoReply
  // flag. |member| is only valid during the call.
  typedef std::function<void(const char* member, sd_bus_message* call)>
      ObjectHandler;
  typedef std::function<void(sd_bus_message* message)> MessageHandler;
  // Appends arguments to |message|. Returns a negative errno on failure.
  typedef std::function<int(sd_bus_message* message)> FillFunction;
  typedef std::function<Status(sd_bus_message* reply)> ReplyReader;
  typedef std::function<void(const std::string& unique_name)> PeerGoneHandler;
  typedef std::function<void(bool present)> NameWatchHandler;

  Bus();
  ~Bus();

  MW_DISALLOW_COPY_AND_ASSIGN(Bus);

  // Opens the connection. If |well_known_name| is not empty it is requested
  // (fails if another process already owns it).
  Status Connect(BusType type, const std::string& well_known_name);

  // Starts the bus thread. Registrations made before Start() are applied
  // directly; afterwards they are marshalled to the bus thread.
  Status Start();
  void Stop();

  bool running() const { return running_; }
  // True if the daemon connection broke (the service should exit and let
  // systemd restart it).
  bool connection_lost() const { return connection_lost_; }
  const std::string& unique_name() const { return unique_name_; }

  // Registers |handler| for all calls to |interface| on |path| and answers
  // org.freedesktop.DBus.Introspectable with |introspection_xml|.
  Status AddObject(const std::string& path, const std::string& interface_name,
                   const std::string& introspection_xml, ObjectHandler handler);

  // Adds a match rule; |handler| runs for each matching message.
  Status AddMatch(const std::string& rule, MessageHandler handler);

  // Reports when the owner of |name| appears or vanishes.
  Status WatchName(const std::string& name, NameWatchHandler handler);

  // Reports the unique name of every peer that disconnects from the bus.
  Status WatchPeerExits(PeerGoneHandler handler);

  // Synchronous method call. |read_reply| runs on the bus thread while the
  // reply is alive (copy out anything you need). Must not be called from a
  // bus-thread handler of the same Bus unless unavoidable (it would block the
  // loop for the duration of the call).
  Status CallMethod(const std::string& destination, const std::string& path,
                    const std::string& interface_name,
                    const std::string& member, const FillFunction& fill,
                    const ReplyReader& read_reply, uint64_t timeout_us);

  // Fire-and-forget method call (sets the NoReply flag). Thread safe.
  void SendNoReply(const std::string& destination, const std::string& path,
                   const std::string& interface_name, const std::string& member,
                   const FillFunction& fill);

  // Emits a signal. An empty |destination| broadcasts; otherwise the signal is
  // unicast to that unique name. Thread safe.
  void EmitSignal(const std::string& destination, const std::string& path,
                  const std::string& interface_name, const std::string& member,
                  const FillFunction& fill);

  // Runs |task| on the bus thread. Returns false if the bus is stopped.
  bool Post(const std::function<void()>& task);
  bool InBusThread() const;

  // Reply helpers for use inside ObjectHandler.
  static void ReplyOk(sd_bus_message* call);
  static void ReplyStatus(sd_bus_message* call, const Status& status);

 private:
  Status Invoke(const std::function<Status()>& fn);
  Status CallLocked(const std::string& destination, const std::string& path,
                    const std::string& interface_name,
                    const std::string& member, const FillFunction& fill,
                    const ReplyReader& read_reply, uint64_t timeout_us);
  void Loop();
  void RunTasks();
  void Wake();

  sd_bus* bus_;
  std::string unique_name_;
  ScopedFd wake_fd_;
  std::thread thread_;
  std::thread::id thread_id_;
  std::atomic<bool> started_;
  std::atomic<bool> running_;
  std::atomic<bool> stop_requested_;
  std::atomic<bool> connection_lost_;

  std::mutex task_mutex_;
  std::deque<std::function<void()>> tasks_;
  bool accepting_tasks_;

  std::vector<sd_bus_slot*> slots_;
  std::vector<std::shared_ptr<void>> registrations_;
};

// Appends |fd| as a D-Bus unix fd ('h').
int AppendFd(sd_bus_message* message, int fd);

// Reads the next unix fd and stores a close-on-exec duplicate owned by |out|.
Status ReadFdDup(sd_bus_message* message, ScopedFd* out);

// Unique bus name of the message sender, or "" if unknown.
std::string SenderOf(sd_bus_message* message);

}  // namespace mw

#endif  // MW_IPC_BUS_H_
