// Copyright 2026 Genio Middleware Authors.
//
// Turns SIGINT/SIGTERM into something a main loop can wait on. Construct it
// first thing in main(), before any thread is created, so the signal mask is
// inherited by all threads.

#ifndef MW_COMMON_TERMINATION_WATCHER_H_
#define MW_COMMON_TERMINATION_WATCHER_H_

#include "mw/common/macros.h"
#include "mw/common/scoped_fd.h"

namespace mw {

class TerminationWatcher {
 public:
  TerminationWatcher();
  ~TerminationWatcher();

  MW_DISALLOW_COPY_AND_ASSIGN(TerminationWatcher);

  // Waits up to |timeout_ms| (negative = forever). Returns true once a
  // termination signal has been received.
  bool Wait(int timeout_ms);

  bool requested() const { return requested_; }

 private:
  ScopedFd signal_fd_;
  bool requested_;
};

}  // namespace mw

#endif  // MW_COMMON_TERMINATION_WATCHER_H_
