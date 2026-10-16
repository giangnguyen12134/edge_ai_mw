// Copyright 2026 Genio Middleware Authors.
//
// Blocking-with-timeout TCP client used to push data to the Jetson. Every call
// is bounded by a timeout so a dead link can never wedge the bridge threads.
// Not thread safe: use one instance per thread.

#ifndef MW_ROBOTIC_TCP_SENDER_H_
#define MW_ROBOTIC_TCP_SENDER_H_

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "mw/common/macros.h"
#include "mw/common/scoped_fd.h"
#include "mw/common/status.h"

namespace mw {

class TcpSender {
 public:
  TcpSender() {}

  MW_DISALLOW_COPY_AND_ASSIGN(TcpSender);

  Status Connect(const std::string& host, uint32_t port, int timeout_ms);
  bool connected() const { return fd_.valid(); }
  void Close() { fd_.Reset(); }

  // Sends all |size| bytes or fails (the connection is closed on failure).
  Status SendAll(const uint8_t* data, size_t size, int timeout_ms);

 private:
  ScopedFd fd_;
};

}  // namespace mw

#endif  // MW_ROBOTIC_TCP_SENDER_H_
