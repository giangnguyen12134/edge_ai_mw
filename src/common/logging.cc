// Copyright 2026 Genio Middleware Authors.

#include "mw/common/logging.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <memory>

#include "mw/common/json.h"

namespace mw {
namespace {

const char kDefaultConfigPath[] = "/etc/genio-mw/logging.json";
const size_t kMaxUdpAppName = 48;

// What one process logs to. Immutable once published; replaced as a whole.
struct Sinks {
  Sinks()
      : to_stderr(true),
        file_fd(-1),
        udp_fd(-1),
        facility(16),
        max_udp_bytes(1200) {}
  ~Sinks() {
    if (file_fd >= 0) {
      close(file_fd);
    }
    if (udp_fd >= 0) {
      close(udp_fd);
    }
  }
  bool to_stderr;
  int file_fd;
  int udp_fd;
  uint32_t facility;
  uint32_t max_udp_bytes;
  std::string program;   // File/UDP tag, sanitised.
  std::string hostname;  // UDP header, sanitised.
};

std::shared_ptr<const Sinks>* SinksStorage() {
  static std::shared_ptr<const Sinks>* storage =
      new std::shared_ptr<const Sinks>(new Sinks());
  return storage;
}

std::shared_ptr<const Sinks> CurrentSinks() {
  return std::atomic_load(SinksStorage());
}

std::atomic<int>& MinSeverityStorage() {
  static std::atomic<int> severity(kLogInfo);
  return severity;
}

std::atomic<uint64_t>& UdpDropped() {
  static std::atomic<uint64_t> dropped(0);
  return dropped;
}

bool RunningUnderJournald() {
  static const bool under_journald = getenv("JOURNAL_STREAM") != nullptr;
  return under_journald;
}

const char* Basename(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash != nullptr ? slash + 1 : path;
}

char SeverityLetter(LogSeverity severity) {
  switch (severity) {
    case kLogDebug:
      return 'D';
    case kLogInfo:
      return 'I';
    case kLogWarning:
      return 'W';
    case kLogError:
      return 'E';
  }
  return '?';
}

// syslog priorities: 7 debug, 6 info, 4 warning, 3 error.
int SyslogPriority(LogSeverity severity) {
  switch (severity) {
    case kLogDebug:
      return 7;
    case kLogInfo:
      return 6;
    case kLogWarning:
      return 4;
    case kLogError:
      return 3;
  }
  return 6;
}

// Printable ASCII without spaces, as RFC 5424 requires for HOSTNAME/APP-NAME.
std::string HeaderToken(const std::string& text, size_t max_length) {
  std::string out;
  for (size_t i = 0; i < text.size() && out.size() < max_length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    out.push_back(c > 0x20 && c < 0x7f ? static_cast<char>(c) : '_');
  }
  return out.empty() ? "-" : out;
}

void WriteAll(int fd, const std::string& text) {
  size_t done = 0;
  while (done < text.size()) {
    const ssize_t n = write(fd, text.data() + done, text.size() - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;  // Disk full etc.: dropping a log line beats blocking.
    }
    done += static_cast<size_t>(n);
  }
}

std::string LocalTimestamp(const struct timespec& ts) {
  struct tm tm_buf;
  localtime_r(&ts.tv_sec, &tm_buf);
  char buf[48];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
  char out[128];
  char zone[8];
  strftime(zone, sizeof(zone), "%z", &tm_buf);
  snprintf(out, sizeof(out), "%s.%03d%s", buf,
           static_cast<int>(ts.tv_nsec / 1000000), zone);
  return out;
}

std::string UtcTimestamp(const struct timespec& ts) {
  struct tm tm_buf;
  gmtime_r(&ts.tv_sec, &tm_buf);
  char buf[48];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
  char out[128];
  snprintf(out, sizeof(out), "%s.%06dZ", buf,
           static_cast<int>(ts.tv_nsec / 1000));
  return out;
}

void SendUdp(const Sinks& sinks, LogSeverity severity,
             const struct timespec& ts, const std::string& where,
             const std::string& text) {
  // <PRI>1 TIMESTAMP HOSTNAME APP-NAME PROCID MSGID STRUCTURED-DATA MSG
  const int pri =
      static_cast<int>(sinks.facility) * 8 + SyslogPriority(severity);
  std::string datagram = "<" + std::to_string(pri) + ">1 " + UtcTimestamp(ts) +
                         " " + sinks.hostname + " " + sinks.program + " " +
                         std::to_string(getpid()) + " - - ";
  std::string body = where + "] " + text;
  if (datagram.size() + body.size() > sinks.max_udp_bytes) {
    const size_t room = sinks.max_udp_bytes > datagram.size() + 3
                            ? sinks.max_udp_bytes - datagram.size() - 3
                            : 0;
    body.resize(room);
    body += "...";
  }
  datagram += body;
  const ssize_t sent = send(sinks.udp_fd, datagram.data(), datagram.size(),
                            MSG_DONTWAIT | MSG_NOSIGNAL);
  if (sent < 0) {
    // Full socket buffer, no route, "connection refused" ICMP from a closed
    // port: all just drop the datagram.
    UdpDropped().fetch_add(1, std::memory_order_relaxed);
  }
}

Status OpenUdp(const LogConfig& config, int* fd) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo* list = nullptr;
  const int rc =
      getaddrinfo(config.udp_host.c_str(),
                  std::to_string(config.udp_port).c_str(), &hints, &list);
  if (rc != 0) {
    return UnavailableError("udp log host '" + config.udp_host +
                            "': " + gai_strerror(rc));
  }
  Status status =
      UnavailableError("no usable address for '" + config.udp_host + "'");
  for (struct addrinfo* a = list; a != nullptr; a = a->ai_next) {
    const int s =
        socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
               a->ai_protocol);
    if (s < 0) {
      status = ErrnoStatus("udp log socket", errno);
      continue;
    }
    if (connect(s, a->ai_addr, a->ai_addrlen) == 0) {
      *fd = s;
      status = Status::Ok();
      break;
    }
    status = ErrnoStatus("udp log connect", errno);
    close(s);
  }
  freeaddrinfo(list);
  return status;
}

}  // namespace

LogConfig::LogConfig()
    : to_stderr(true), udp_port(0), udp_facility(16), udp_max_bytes(1200) {}

std::string SanitizeLogText(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if ((c < 0x20 && c != '\t') || c == 0x7f) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

void SetMinLogSeverity(LogSeverity severity) {
  MinSeverityStorage().store(static_cast<int>(severity));
}

LogSeverity MinLogSeverity() {
  return static_cast<LogSeverity>(MinSeverityStorage().load());
}

bool SetMinLogSeverityFromString(const std::string& name) {
  if (name == "debug") {
    SetMinLogSeverity(kLogDebug);
  } else if (name == "info") {
    SetMinLogSeverity(kLogInfo);
  } else if (name == "warning") {
    SetMinLogSeverity(kLogWarning);
  } else if (name == "error") {
    SetMinLogSeverity(kLogError);
  } else {
    return false;
  }
  return true;
}

uint64_t LogUdpDropped() { return UdpDropped().load(); }

Status ValidateLogConfig(const LogConfig& c) {
  if (!c.to_stderr && c.file_path.empty() && c.udp_host.empty()) {
    return InvalidArgumentError("logging needs at least one sink");
  }
  if (!c.file_path.empty() && c.file_path[0] != '/') {
    return InvalidArgumentError("file.path must be an absolute path");
  }
  if (!c.udp_host.empty()) {
    if (c.udp_port == 0 || c.udp_port > 65535) {
      return InvalidArgumentError("udp.port must be 1..65535");
    }
    if (c.udp_facility > 23) {
      return InvalidArgumentError("udp.facility must be 0..23");
    }
    if (c.udp_max_bytes < 256 || c.udp_max_bytes > 1400) {
      return InvalidArgumentError("udp.max_bytes must be 256..1400");
    }
  }
  return Status::Ok();
}

Status LoadLogConfig(const std::string& path, LogConfig* config) {
  JsonValue root;
  MW_RETURN_IF_ERROR(JsonValue::ParseFile(path, &root));
  if (!root.is_object()) {
    return InvalidArgumentError(path + ": top level must be an object");
  }
  LogConfig c;
  const JsonValue* sinks = root.Find("sinks");
  if (sinks != nullptr) {
    if (!sinks->is_array()) {
      return InvalidArgumentError(path + ": 'sinks' must be an array");
    }
    c.to_stderr = false;
    bool want_file = false;
    bool want_udp = false;
    for (size_t i = 0; i < sinks->array().size(); ++i) {
      const JsonValue& entry = sinks->array()[i];
      const std::string name = entry.is_string() ? entry.string_value() : "";
      if (name == "stderr") {
        c.to_stderr = true;
      } else if (name == "file") {
        want_file = true;
      } else if (name == "udp") {
        want_udp = true;
      } else {
        return InvalidArgumentError(path +
                                    ": unknown sink (use stderr, "
                                    "file or udp)");
      }
    }
    const JsonValue* file = root.Find("file");
    if (want_file) {
      c.file_path = file != nullptr ? file->GetString("path", "") : "";
      if (c.file_path.empty()) {
        return InvalidArgumentError(path + ": sink 'file' needs file.path");
      }
    }
    const JsonValue* udp = root.Find("udp");
    if (want_udp) {
      if (udp == nullptr) {
        return InvalidArgumentError(path + ": sink 'udp' needs a udp object");
      }
      c.udp_host = udp->GetString("host", "");
      c.udp_port = static_cast<uint32_t>(udp->GetInt("port", 0));
      c.udp_facility = static_cast<uint32_t>(udp->GetInt("facility", 16));
      c.udp_max_bytes = static_cast<uint32_t>(udp->GetInt("max_bytes", 1200));
      if (c.udp_host.empty()) {
        return InvalidArgumentError(path + ": udp.host must not be empty");
      }
    }
  }
  const Status valid = ValidateLogConfig(c);
  if (!valid.ok()) {
    return InvalidArgumentError(path + ": " + valid.message());
  }
  *config = c;
  return Status::Ok();
}

Status ConfigureLogging(const LogConfig& config, const std::string& program) {
  std::shared_ptr<Sinks> sinks(new Sinks());
  sinks->to_stderr = config.to_stderr;
  sinks->facility = config.udp_facility;
  sinks->max_udp_bytes = config.udp_max_bytes;
  sinks->program = HeaderToken(program, kMaxUdpAppName);
  char host[256];
  host[0] = '\0';
  gethostname(host, sizeof(host) - 1);
  host[sizeof(host) - 1] = '\0';
  sinks->hostname = HeaderToken(host, 255);

  std::string problems;
  if (!config.file_path.empty()) {
    // O_APPEND: the kernel positions every write at the current end of file
    // atomically, which is what makes one shared file safe across processes.
    const int fd = open(config.file_path.c_str(),
                        O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0640);
    if (fd < 0) {
      problems +=
          "log file '" + config.file_path + "': " + strerror(errno) + "; ";
    } else {
      sinks->file_fd = fd;
    }
  }
  if (!config.udp_host.empty()) {
    int fd = -1;
    const Status status = OpenUdp(config, &fd);
    if (status.ok()) {
      sinks->udp_fd = fd;
    } else {
      problems += status.message() + "; ";
    }
  }
  // Never end up with no sink at all: fall back to stderr.
  if (!sinks->to_stderr && sinks->file_fd < 0 && sinks->udp_fd < 0) {
    sinks->to_stderr = true;
  }
  std::shared_ptr<const Sinks> published = sinks;
  std::atomic_store(SinksStorage(), published);
  if (!problems.empty()) {
    return UnavailableError(problems);
  }
  return Status::Ok();
}

bool SetUpLogging(const std::string& explicit_path,
                  const std::string& program) {
  std::string path = explicit_path;
  if (path.empty()) {
    const char* env = getenv("MW_LOG_CONFIG");
    if (env != nullptr && env[0] != '\0') {
      path = env;
    } else if (access(kDefaultConfigPath, R_OK) == 0) {
      path = kDefaultConfigPath;
    }
  }
  LogConfig config;
  if (!path.empty()) {
    const Status status = LoadLogConfig(path, &config);
    if (!status.ok()) {
      fprintf(stderr, "%s: invalid logging configuration: %s\n",
              program.c_str(), status.ToString().c_str());
      return false;
    }
  }
  const Status status = ConfigureLogging(config, program);
  if (!status.ok()) {
    MW_LOG(Warning) << "logging degraded: " << status.message();
  }
  return true;
}

LogMessage::LogMessage(LogSeverity severity, const char* file, int line)
    : severity_(severity), file_(file), line_(line) {}

LogMessage::~LogMessage() {
  const std::shared_ptr<const Sinks> sinks = CurrentSinks();
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  const std::string text = SanitizeLogText(stream_.str());
  std::string where =
      std::string(Basename(file_)) + ":" + std::to_string(line_);

  if (sinks->to_stderr) {
    char prefix[64];
    if (RunningUnderJournald()) {
      snprintf(prefix, sizeof(prefix), "<%d>", SyslogPriority(severity_));
    } else {
      struct tm tm_buf;
      localtime_r(&ts.tv_sec, &tm_buf);
      char time_buf[32];
      strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm_buf);
      snprintf(prefix, sizeof(prefix), "%s.%03ld %c ", time_buf,
               ts.tv_nsec / 1000000, SeverityLetter(severity_));
    }
    const std::string line = std::string(prefix) + where + "] " + text + "\n";
    fwrite(line.data(), 1, line.size(), stderr);
  }
  if (sinks->file_fd >= 0) {
    // One line, one write(2).
    const std::string line = LocalTimestamp(ts) + " " +
                             SeverityLetter(severity_) + " " + sinks->program +
                             "[" + std::to_string(getpid()) + "] " + where +
                             "] " + text + "\n";
    WriteAll(sinks->file_fd, line);
  }
  if (sinks->udp_fd >= 0) {
    SendUdp(*sinks, severity_, ts, where, text);
  }
}

}  // namespace mw
