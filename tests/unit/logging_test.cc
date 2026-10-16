// Copyright 2026 Genio Middleware Authors.

#include "mw/common/logging.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <vector>

#include "tests/test_framework.h"

namespace mw {
namespace {

// Restores stderr-only logging when a test ends, whatever happens.
struct ResetLogging {
  ~ResetLogging() { ConfigureLogging(LogConfig(), "mw-tests"); }
};

class TempDir {
 public:
  TempDir() {
    char path[] = "/tmp/mw_log_XXXXXX";
    path_ = mkdtemp(path) != nullptr ? path : "";
  }
  ~TempDir() {
    for (size_t i = 0; i < files_.size(); ++i) {
      unlink(files_[i].c_str());
    }
    if (!path_.empty()) {
      rmdir(path_.c_str());
    }
  }
  std::string Path(const std::string& name) {
    files_.push_back(path_ + "/" + name);
    return files_.back();
  }
  bool ok() const { return !path_.empty(); }

 private:
  std::string path_;
  std::vector<std::string> files_;
};

std::string ReadFile(const std::string& path) {
  std::string out;
  FILE* f = fopen(path.c_str(), "r");
  if (f == nullptr) {
    return out;
  }
  char buffer[4096];
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
    out.append(buffer, n);
  }
  fclose(f);
  return out;
}

std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      lines.push_back(text.substr(start));  // Unterminated: caller notices.
      break;
    }
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

bool WriteText(const std::string& path, const std::string& text) {
  FILE* f = fopen(path.c_str(), "w");
  if (f == nullptr) {
    return false;
  }
  fwrite(text.data(), 1, text.size(), f);
  fclose(f);
  return true;
}

LogConfig FileOnly(const std::string& path) {
  LogConfig config;
  config.to_stderr = false;
  config.file_path = path;
  return config;
}

// UDP receiver on 127.0.0.1, ephemeral port, 1 s receive timeout.
int UdpReceiver(uint16_t* port) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (fd < 0 ||
      bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    return -1;
  }
  socklen_t len = sizeof(addr);
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  *port = ntohs(addr.sin_port);
  timeval timeout;
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  return fd;
}

std::string Receive(int fd) {
  char buffer[4096];
  const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
  return n > 0 ? std::string(buffer, static_cast<size_t>(n)) : "";
}

}  // namespace

MW_TEST(LogSanitize, EscapesLineBreaksAndControlCharacters) {
  MW_EXPECT_EQ(SanitizeLogText("a\nb"), std::string("a\\nb"));
  MW_EXPECT_EQ(SanitizeLogText("a\r\nb"), std::string("a\\r\\nb"));
  MW_EXPECT_EQ(SanitizeLogText(std::string("x\x01y\x7f", 4)),
               std::string("x\\x01y\\x7f"));
  MW_EXPECT_EQ(SanitizeLogText("tab\tstays"), std::string("tab\tstays"));
  MW_EXPECT_EQ(SanitizeLogText("plain text 123"),
               std::string("plain text 123"));
  // Bytes >= 0x80 (UTF-8) pass through untouched.
  MW_EXPECT_EQ(SanitizeLogText("caf\xc3\xa9"), std::string("caf\xc3\xa9"));
}

MW_TEST(LogConfig, LoadsAllSinks) {
  TempDir dir;
  MW_ASSERT_TRUE(dir.ok());
  const std::string path = dir.Path("logging.json");
  MW_ASSERT_TRUE(WriteText(
      path,
      "{\"sinks\":[\"stderr\",\"file\",\"udp\"],"
      "\"file\":{\"path\":\"/var/log/genio-mw/middleware.log\"},"
      "\"udp\":{\"host\":\"192.168.1.10\",\"port\":5140,\"facility\":17,"
      "\"max_bytes\":800}}"));
  LogConfig config;
  MW_ASSERT_OK(LoadLogConfig(path, &config));
  MW_EXPECT_TRUE(config.to_stderr);
  MW_EXPECT_EQ(config.file_path,
               std::string("/var/log/genio-mw/middleware.log"));
  MW_EXPECT_EQ(config.udp_host, std::string("192.168.1.10"));
  MW_EXPECT_EQ(config.udp_port, 5140u);
  MW_EXPECT_EQ(config.udp_facility, 17u);
  MW_EXPECT_EQ(config.udp_max_bytes, 800u);
}

MW_TEST(LogConfig, MissingSinksMeansStderr) {
  TempDir dir;
  const std::string path = dir.Path("logging.json");
  MW_ASSERT_TRUE(WriteText(path, "{}"));
  LogConfig config;
  MW_ASSERT_OK(LoadLogConfig(path, &config));
  MW_EXPECT_TRUE(config.to_stderr);
  MW_EXPECT_TRUE(config.file_path.empty());
  MW_EXPECT_TRUE(config.udp_host.empty());
}

MW_TEST(LogConfig, RejectsBadConfigurations) {
  TempDir dir;
  const std::string path = dir.Path("logging.json");
  const char* bad[] = {
      "{\"sinks\":[\"syslog\"]}",                                // unknown sink
      "{\"sinks\":[\"file\"]}",                                  // no path
      "{\"sinks\":[\"file\"],\"file\":{\"path\":\"rel.log\"}}",  // relative
      "{\"sinks\":[\"udp\"]}",                         // no udp object
      "{\"sinks\":[\"udp\"],\"udp\":{\"port\":514}}",  // no host
      "{\"sinks\":[\"udp\"],\"udp\":{\"host\":\"h\",\"port\":0}}",
      "{\"sinks\":[\"udp\"],\"udp\":{\"host\":\"h\",\"port\":70000}}",
      "{\"sinks\":[\"udp\"],\"udp\":{\"host\":\"h\",\"port\":1,"
      "\"facility\":24}}",
      "{\"sinks\":[\"udp\"],\"udp\":{\"host\":\"h\",\"port\":1,"
      "\"max_bytes\":10}}",
      "{\"sinks\":[]}",          // no sink
      "{\"sinks\":\"stderr\"}",  // not an array
      "[1,2]",                   // not an object
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    MW_ASSERT_TRUE(WriteText(path, bad[i]));
    LogConfig config;
    if (LoadLogConfig(path, &config).ok()) {
      ::mwtest::RecordFailure(__FILE__, __LINE__,
                              std::string("accepted: ") + bad[i]);
    }
  }
  LogConfig config;
  MW_EXPECT_FALSE(LoadLogConfig("/nonexistent/logging.json", &config).ok());
}

MW_TEST(LogConfig, ShippedFilesAreValid) {
  const char* files[] = {MW_SOURCE_DIR "/config/logging.json",
                         MW_SOURCE_DIR "/config/dev/logging.json",
                         MW_SOURCE_DIR "/config/x64/logging.json"};
  for (size_t i = 0; i < 3; ++i) {
    LogConfig config;
    MW_EXPECT_OK(LoadLogConfig(files[i], &config));
  }
}

MW_TEST(LogFile, WritesOneTaggedLinePerMessage) {
  TempDir dir;
  MW_ASSERT_TRUE(dir.ok());
  const std::string path = dir.Path("middleware.log");
  ResetLogging reset;
  MW_ASSERT_OK(ConfigureLogging(FileOnly(path), "unit-test"));
  MW_LOG(Error) << "hello\nforged line";
  MW_LOG(Error) << "second";
  const std::vector<std::string> lines = Lines(ReadFile(path));
  MW_ASSERT_TRUE(lines.size() == 2);
  MW_EXPECT_TRUE(lines[0].find(" E unit-test[" + std::to_string(getpid()) +
                               "] logging_test.cc:") != std::string::npos);
  MW_EXPECT_TRUE(lines[0].find("] hello\\nforged line") != std::string::npos);
  MW_EXPECT_TRUE(lines[1].find("] second") != std::string::npos);
  // ISO timestamp with date at the start of the line.
  MW_EXPECT_TRUE(lines[0].size() > 20 && lines[0][4] == '-' &&
                 lines[0][10] == 'T');
}

MW_TEST(LogFile, AppendsToExistingFile) {
  TempDir dir;
  const std::string path = dir.Path("middleware.log");
  MW_ASSERT_TRUE(WriteText(path, "old line\n"));
  ResetLogging reset;
  MW_ASSERT_OK(ConfigureLogging(FileOnly(path), "unit-test"));
  MW_LOG(Error) << "new";
  const std::vector<std::string> lines = Lines(ReadFile(path));
  MW_ASSERT_TRUE(lines.size() == 2);
  MW_EXPECT_EQ(lines[0], std::string("old line"));
}

MW_TEST(LogFile, ThreadsDoNotInterleave) {
  TempDir dir;
  const std::string path = dir.Path("middleware.log");
  ResetLogging reset;
  MW_ASSERT_OK(ConfigureLogging(FileOnly(path), "unit-test"));
  const int kThreads = 4;
  const int kLines = 300;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.push_back(std::thread([t, kLines]() {
      const std::string padding(300, static_cast<char>('a' + t));
      for (int i = 0; i < kLines; ++i) {
        MW_LOG(Error) << "T" << t << " #" << i << " " << padding << " END";
      }
    }));
  }
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i].join();
  }
  const std::vector<std::string> lines = Lines(ReadFile(path));
  MW_EXPECT_EQ(lines.size(), static_cast<size_t>(kThreads * kLines));
  size_t intact = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    const size_t at = lines[i].find("] T");
    if (at != std::string::npos &&
        lines[i].compare(lines[i].size() - 4, 4, " END") == 0) {
      const char tag = lines[i][at + 3];
      const std::string padding(300, static_cast<char>('a' + (tag - '0')));
      if (lines[i].find(padding) != std::string::npos) {
        ++intact;
      }
    }
  }
  MW_EXPECT_EQ(intact, lines.size());
}

MW_TEST(LogFile, ProcessesShareOneFileWithoutInterleaving) {
  TempDir dir;
  const std::string path = dir.Path("middleware.log");
  const int kProcesses = 4;
  const int kLines = 250;
  std::vector<pid_t> children;
  for (int p = 0; p < kProcesses; ++p) {
    const pid_t pid = fork();
    MW_ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
      // Child: its own descriptor on the same file, as separate services have.
      if (!ConfigureLogging(FileOnly(path), "proc" + std::to_string(p)).ok()) {
        _exit(1);
      }
      const std::string padding(1500, static_cast<char>('A' + p));
      for (int i = 0; i < kLines; ++i) {
        MW_LOG(Error) << "P" << p << " #" << i << " " << padding << " END";
      }
      _exit(0);
    }
    children.push_back(pid);
  }
  for (size_t i = 0; i < children.size(); ++i) {
    int status = 0;
    waitpid(children[i], &status, 0);
    MW_EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  const std::vector<std::string> lines = Lines(ReadFile(path));
  MW_EXPECT_EQ(lines.size(), static_cast<size_t>(kProcesses * kLines));
  std::vector<int> per_process(kProcesses, 0);
  for (size_t i = 0; i < lines.size(); ++i) {
    const size_t at = lines[i].find("] P");
    bool good = at != std::string::npos && lines[i].size() > 4 &&
                lines[i].compare(lines[i].size() - 4, 4, " END") == 0;
    if (good) {
      const int p = lines[i][at + 3] - '0';
      good =
          p >= 0 && p < kProcesses &&
          lines[i].find(std::string(1500, static_cast<char>('A' + p))) !=
              std::string::npos &&
          lines[i].find(" proc" + std::to_string(p) + "[") != std::string::npos;
      if (good) {
        ++per_process[static_cast<size_t>(p)];
      }
    }
    if (!good) {
      ::mwtest::RecordFailure(__FILE__, __LINE__, "damaged or foreign line");
      break;
    }
  }
  for (int p = 0; p < kProcesses; ++p) {
    MW_EXPECT_EQ(per_process[static_cast<size_t>(p)], kLines);
  }
}

MW_TEST(LogFile, UnwritablePathDegradesToStderr) {
  ResetLogging reset;
  LogConfig config = FileOnly("/nonexistent-dir/middleware.log");
  const Status status = ConfigureLogging(config, "unit-test");
  MW_EXPECT_CODE(status, Status::kUnavailable);
  MW_LOG(Error) << "this goes to stderr because no sink survived";
}

MW_TEST(LogUdp, SendsRfc5424Datagram) {
  uint16_t port = 0;
  const int receiver = UdpReceiver(&port);
  MW_ASSERT_TRUE(receiver >= 0);
  ResetLogging reset;
  LogConfig config;
  config.to_stderr = false;
  config.udp_host = "127.0.0.1";
  config.udp_port = port;
  MW_ASSERT_OK(ConfigureLogging(config, "unit test"));  // Space in the name.
  const LogSeverity saved = MinLogSeverity();
  SetMinLogSeverity(kLogWarning);  // The runner only lets errors through.
  MW_LOG(Warning) << "udp hello\nsecond";
  SetMinLogSeverity(saved);
  const std::string datagram = Receive(receiver);
  close(receiver);
  MW_ASSERT_TRUE(!datagram.empty());
  // local0 (16) * 8 + warning (4) = 132.
  MW_EXPECT_TRUE(datagram.compare(0, 7, "<132>1 ") == 0);
  const size_t z = datagram.find("Z ");
  MW_EXPECT_TRUE(z != std::string::npos && z == 7 + 26);  // 2026-..T..:..:...
  MW_EXPECT_TRUE(datagram.find(" unit_test " + std::to_string(getpid()) +
                               " - - logging_test.cc:") != std::string::npos);
  MW_EXPECT_TRUE(datagram.find("] udp hello\\nsecond") != std::string::npos);
  MW_EXPECT_TRUE(datagram.find('\n') == std::string::npos);
}

MW_TEST(LogUdp, FacilityAndSeverityMapToPriority) {
  uint16_t port = 0;
  const int receiver = UdpReceiver(&port);
  MW_ASSERT_TRUE(receiver >= 0);
  ResetLogging reset;
  LogConfig config;
  config.to_stderr = false;
  config.udp_host = "127.0.0.1";
  config.udp_port = port;
  config.udp_facility = 3;  // daemon
  MW_ASSERT_OK(ConfigureLogging(config, "unit-test"));
  MW_LOG(Error) << "boom";
  const std::string datagram = Receive(receiver);
  close(receiver);
  MW_EXPECT_TRUE(datagram.compare(0, 6, "<27>1 ") == 0);  // 3 * 8 + 3.
}

MW_TEST(LogUdp, OversizedMessagesAreTruncated) {
  uint16_t port = 0;
  const int receiver = UdpReceiver(&port);
  MW_ASSERT_TRUE(receiver >= 0);
  ResetLogging reset;
  LogConfig config;
  config.to_stderr = false;
  config.udp_host = "127.0.0.1";
  config.udp_port = port;
  config.udp_max_bytes = 512;
  MW_ASSERT_OK(ConfigureLogging(config, "unit-test"));
  MW_LOG(Error) << std::string(5000, 'x');
  const std::string datagram = Receive(receiver);
  close(receiver);
  MW_ASSERT_TRUE(!datagram.empty());
  MW_EXPECT_TRUE(datagram.size() <= 512);
  MW_EXPECT_TRUE(datagram.size() > 400);
  MW_EXPECT_EQ(datagram.substr(datagram.size() - 3), std::string("..."));
}

MW_TEST(LogUdp, DeadServerNeverBlocksOrCrashes) {
  uint16_t port = 0;
  const int receiver = UdpReceiver(&port);
  MW_ASSERT_TRUE(receiver >= 0);
  close(receiver);  // Nothing listens on the port any more.
  ResetLogging reset;
  LogConfig config;
  config.to_stderr = false;
  config.udp_host = "127.0.0.1";
  config.udp_port = port;
  MW_ASSERT_OK(ConfigureLogging(config, "unit-test"));
  const uint64_t before = LogUdpDropped();
  for (int i = 0; i < 100; ++i) {
    MW_LOG(Error) << "nobody is listening " << i;
    usleep(1000);
  }
  // The first datagram provokes an ICMP "port unreachable", after which the
  // connected socket reports ECONNREFUSED: those sends are counted as drops.
  MW_EXPECT_TRUE(LogUdpDropped() > before);
}

MW_TEST(LogUdp, UnresolvableHostDegradesWithoutThrowing) {
  ResetLogging reset;
  LogConfig config;
  config.to_stderr = false;
  config.udp_host = "300.300.300.300";
  config.udp_port = 514;
  MW_EXPECT_CODE(ConfigureLogging(config, "unit-test"), Status::kUnavailable);
  MW_LOG(Error) << "falls back to stderr";
}

MW_TEST(LogSetUp, ExplicitBadConfigIsRejected) {
  TempDir dir;
  const std::string path = dir.Path("logging.json");
  MW_ASSERT_TRUE(WriteText(path, "{\"sinks\":[\"nope\"]}"));
  ResetLogging reset;
  MW_EXPECT_FALSE(SetUpLogging(path, "unit-test"));
  MW_EXPECT_FALSE(SetUpLogging("/nonexistent/logging.json", "unit-test"));
}

MW_TEST(LogSetUp, ExplicitConfigAndEnvironmentVariable) {
  TempDir dir;
  const std::string log_path = dir.Path("middleware.log");
  const std::string config_path = dir.Path("logging.json");
  MW_ASSERT_TRUE(WriteText(
      config_path,
      "{\"sinks\":[\"file\"],\"file\":{\"path\":\"" + log_path + "\"}}"));
  ResetLogging reset;
  MW_ASSERT_TRUE(SetUpLogging(config_path, "unit-test"));
  MW_LOG(Error) << "via explicit path";
  MW_EXPECT_TRUE(ReadFile(log_path).find("via explicit path") !=
                 std::string::npos);

  ConfigureLogging(LogConfig(), "unit-test");
  MW_ASSERT_TRUE(setenv("MW_LOG_CONFIG", config_path.c_str(), 1) == 0);
  const bool ok = SetUpLogging("", "unit-test");
  unsetenv("MW_LOG_CONFIG");
  MW_ASSERT_TRUE(ok);
  MW_LOG(Error) << "via environment";
  MW_EXPECT_TRUE(ReadFile(log_path).find("via environment") !=
                 std::string::npos);
}

}  // namespace mw
