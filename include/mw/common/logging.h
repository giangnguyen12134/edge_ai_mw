// Copyright 2026 Genio Middleware Authors.
//
// Thread- and process-safe logging with selectable sinks, configured once per
// process (normally from the shared logging.json, see SetUpLogging()):
//
//   stderr  default; under systemd/journald lines carry a "<N>" priority.
//   file    one shared append-only file, e.g. /var/log/genio-mw/middleware.log.
//           Every line is one write(2) on an O_APPEND descriptor, so several
//           processes can share the file without interleaving. Lines carry
//           the program name and pid.
//   udp     RFC 5424 syslog datagrams to host:port. Never blocks; datagrams
//           that cannot be sent are dropped (see LogUdpDropped()).
//
// Messages are sanitised: control characters (newlines included) are escaped,
// so one MW_LOG statement is always exactly one log line and a peer-supplied
// string cannot forge extra lines.
//
// Usage: MW_LOG(Info) << "frame " << seq << " dropped";

#ifndef MW_COMMON_LOGGING_H_
#define MW_COMMON_LOGGING_H_

#include <stdint.h>

#include <sstream>
#include <string>

#include "mw/common/macros.h"
#include "mw/common/status.h"

namespace mw {

enum LogSeverity {
  kLogDebug = 0,
  kLogInfo = 1,
  kLogWarning = 2,
  kLogError = 3,
};

void SetMinLogSeverity(LogSeverity severity);
LogSeverity MinLogSeverity();

// Accepts "debug", "info", "warning", "error". Returns false if unknown.
bool SetMinLogSeverityFromString(const std::string& name);

struct LogConfig {
  LogConfig();
  bool to_stderr;
  std::string file_path;   // Absolute path; empty = no file sink.
  std::string udp_host;    // IP address (preferred) or name; empty = no UDP.
  uint32_t udp_port;       // 1..65535.
  uint32_t udp_facility;   // Syslog facility 0..23 (default 16 = local0).
  uint32_t udp_max_bytes;  // Datagram size limit, 256..1400.
};

// Reads a logging.json:
//   {"sinks": ["stderr", "file", "udp"],
//    "file": {"path": "/var/log/genio-mw/middleware.log"},
//    "udp":  {"host": "192.168.1.10", "port": 514, "facility": 16,
//             "max_bytes": 1200}}
// A missing "sinks" means stderr only.
Status LoadLogConfig(const std::string& path, LogConfig* config);
Status ValidateLogConfig(const LogConfig& config);

// Installs the sinks of |config| for this process. |program| tags file and
// UDP lines. A sink that cannot be set up (unwritable file, unresolvable
// host) is skipped, the others are installed, and the returned Status
// describes what failed: logging never takes a service down.
Status ConfigureLogging(const LogConfig& config, const std::string& program);

// Used by every service main(): picks the configuration (explicit path, else
// $MW_LOG_CONFIG, else /etc/genio-mw/logging.json when it exists, else
// stderr), loads and installs it. Returns false (after printing the reason to
// stderr) only when the configuration itself is invalid; sink failures are
// logged as a warning and the process continues.
bool SetUpLogging(const std::string& explicit_path, const std::string& program);

// Datagrams the UDP sink could not send since start.
uint64_t LogUdpDropped();

// Escapes control characters (and DEL) so |text| fits on one line.
std::string SanitizeLogText(const std::string& text);

class LogMessage {
 public:
  LogMessage(LogSeverity severity, const char* file, int line);
  ~LogMessage();

  std::ostream& stream() { return stream_; }

  MW_DISALLOW_COPY_AND_ASSIGN(LogMessage);

 private:
  LogSeverity severity_;
  const char* file_;
  int line_;
  std::ostringstream stream_;
};

}  // namespace mw

// The empty-if form keeps the macro safe inside un-braced if/else statements.
#define MW_LOG(severity)                               \
  if (::mw::kLog##severity < ::mw::MinLogSeverity()) { \
  } else /* NOLINT(readability/braces) */              \
    ::mw::LogMessage(::mw::kLog##severity, __FILE__, __LINE__).stream()

#endif  // MW_COMMON_LOGGING_H_
