// Copyright 2026 Genio Middleware Authors.

#include "mw/robotic/tcp_sender.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "mw/common/time_util.h"

namespace mw {
namespace {

// Waits for |events| on |fd|. Returns 1 ready, 0 timeout, -1 error.
int WaitFor(int fd, short events, int timeout_ms) {
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = events;
  pfd.revents = 0;
  while (true) {
    const int r = poll(&pfd, 1, timeout_ms);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    return r;
  }
}

}  // namespace

Status TcpSender::Connect(const std::string& host, uint32_t port,
                          int timeout_ms) {
  Close();
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* list = nullptr;
  const std::string service = std::to_string(port);
  const int gai = getaddrinfo(host.c_str(), service.c_str(), &hints, &list);
  if (gai != 0) {
    return UnavailableError("cannot resolve " + host + ": " +
                            gai_strerror(gai));
  }
  Status last = UnavailableError("no address for " + host);
  for (struct addrinfo* ai = list; ai != nullptr; ai = ai->ai_next) {
    ScopedFd fd(socket(ai->ai_family,
                       ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                       ai->ai_protocol));
    if (!fd.valid()) {
      last = ErrnoStatus("socket", errno);
      continue;
    }
    int rc = connect(fd.get(), ai->ai_addr, ai->ai_addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
      const int ready = WaitFor(fd.get(), POLLOUT, timeout_ms);
      if (ready == 0) {
        last = TimeoutError("connect to " + host + ":" + service);
        continue;
      }
      int error = 0;
      socklen_t length = sizeof(error);
      if (ready < 0 ||
          getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &length) < 0) {
        last = ErrnoStatus("connect poll", errno);
        continue;
      }
      if (error != 0) {
        last = ErrnoStatus("connect to " + host + ":" + service, error);
        continue;
      }
    } else if (rc < 0) {
      last = ErrnoStatus("connect to " + host + ":" + service, errno);
      continue;
    }
    const int one = 1;
    setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd.get(), SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    fd_ = std::move(fd);
    freeaddrinfo(list);
    return Status::Ok();
  }
  freeaddrinfo(list);
  return last;
}

Status TcpSender::SendAll(const uint8_t* data, size_t size, int timeout_ms) {
  if (!fd_.valid()) {
    return FailedPreconditionError("not connected");
  }
  const uint64_t deadline_ns =
      MonotonicNs() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
  size_t sent = 0;
  while (sent < size) {
    const ssize_t n = send(fd_.get(), data + sent, size - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      const uint64_t now = MonotonicNs();
      if (now >= deadline_ns) {
        Close();
        return TimeoutError("send to Jetson timed out");
      }
      const int remaining_ms =
          static_cast<int>((deadline_ns - now) / 1000000ull) + 1;
      if (WaitFor(fd_.get(), POLLOUT, remaining_ms) <= 0) {
        Close();
        return TimeoutError("send to Jetson timed out");
      }
      continue;
    }
    const int error = n < 0 ? errno : EPIPE;
    Close();
    return ErrnoStatus("send to Jetson", error);
  }
  return Status::Ok();
}

}  // namespace mw
