// Copyright 2026 Genio Middleware Authors.
//
// Test runner. Usage: mw_tests [substring-filter]

#include <stdio.h>
#include <string.h>

#include "mw/common/logging.h"
#include "tests/test_framework.h"

namespace mwtest {
namespace {

bool g_current_failed = false;
int g_failure_count = 0;

}  // namespace

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

void RecordFailure(const char* file, int line, const std::string& message) {
  g_current_failed = true;
  ++g_failure_count;
  printf("    %s:%d: %s\n", file, line, message.c_str());
}

bool CurrentTestFailed() { return g_current_failed; }

}  // namespace mwtest

int main(int argc, char** argv) {
  // Keep test output readable: the code under test logs errors on purpose.
  mw::SetMinLogSeverity(mw::kLogError);
  const char* filter = argc > 1 ? argv[1] : "";
  int run = 0;
  int failed = 0;
  const std::vector<mwtest::TestCase>& tests = mwtest::Registry();
  for (size_t i = 0; i < tests.size(); ++i) {
    const std::string full = std::string(tests[i].suite) + "." + tests[i].name;
    if (strstr(full.c_str(), filter) == nullptr) {
      continue;
    }
    ++run;
    mwtest::g_current_failed = false;
    tests[i].function();
    if (mwtest::g_current_failed) {
      ++failed;
      printf("[ FAIL ] %s\n", full.c_str());
    } else {
      printf("[  ok  ] %s\n", full.c_str());
    }
  }
  printf("\n%d tests, %d failed\n", run, failed);
  return failed == 0 && run > 0 ? 0 : 1;
}
