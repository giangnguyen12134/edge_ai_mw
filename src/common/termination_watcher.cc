// Copyright 2026 Genio Middleware Authors.

#include "mw/common/termination_watcher.h"

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace mw {

TerminationWatcher::TerminationWatcher() : requested_(false) {
  signal(SIGPIPE, SIG_IGN);
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigprocmask(SIG_BLOCK, &mask, nullptr);
  signal_fd_.Reset(signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK));
}

TerminationWatcher::~TerminationWatcher() {}

bool TerminationWatcher::Wait(int timeout_ms) {
  if (requested_) {
    return true;
  }
  struct pollfd pfd;
  pfd.fd = signal_fd_.get();
  pfd.events = POLLIN;
  pfd.revents = 0;
  const int r = poll(&pfd, 1, timeout_ms);
  if (r > 0 && (pfd.revents & POLLIN)) {
    struct signalfd_siginfo info;
    if (read(signal_fd_.get(), &info, sizeof(info)) ==
        static_cast<ssize_t>(sizeof(info))) {
      requested_ = true;
    }
  }
  return requested_;
}

}  // namespace mw
