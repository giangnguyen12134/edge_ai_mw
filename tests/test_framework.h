// Copyright 2026 Genio Middleware Authors.
//
// Minimal unit test framework (no external dependency so the tests build on a
// bare Yocto/Ubuntu host).
//
//   MW_TEST(Suite, Name) { MW_EXPECT_EQ(1 + 1, 2); }
//
// Failures are recorded and the test continues; MW_ASSERT_* abort the test.

#ifndef TESTS_TEST_FRAMEWORK_H_  // NOLINT(build/header_guard)
#define TESTS_TEST_FRAMEWORK_H_

#include <math.h>
#include <stdio.h>

#include <sstream>
#include <string>
#include <vector>

namespace mwtest {

struct TestCase {
  const char* suite;
  const char* name;
  void (*function)();
};

std::vector<TestCase>& Registry();
void RecordFailure(const char* file, int line, const std::string& message);
bool CurrentTestFailed();

struct Registrar {
  Registrar(const char* suite, const char* name, void (*function)()) {
    TestCase test_case;
    test_case.suite = suite;
    test_case.name = name;
    test_case.function = function;
    Registry().push_back(test_case);
  }
};

template <typename A, typename B>
std::string Describe(const char* a_text, const char* b_text, const A& a,
                     const B& b) {
  std::ostringstream os;
  os << "expected " << a_text << " == " << b_text << ", got " << a << " vs "
     << b;
  return os.str();
}

}  // namespace mwtest

#define MW_TEST(suite, name)                             \
  static void Test_##suite##_##name();                   \
  static ::mwtest::Registrar registrar_##suite##_##name( \
      #suite, #name, &Test_##suite##_##name);            \
  static void Test_##suite##_##name()

#define MW_EXPECT_TRUE(cond)                                           \
  do {                                                                 \
    if (!(cond)) {                                                     \
      ::mwtest::RecordFailure(__FILE__, __LINE__,                      \
                              std::string("expected true: ") + #cond); \
    }                                                                  \
  } while (0)

#define MW_EXPECT_FALSE(cond) MW_EXPECT_TRUE(!(cond))

#define MW_EXPECT_EQ(a, b)                                             \
  do {                                                                 \
    const auto& mw_a = (a);                                            \
    const auto& mw_b = (b);                                            \
    if (!(mw_a == mw_b)) {                                             \
      ::mwtest::RecordFailure(__FILE__, __LINE__,                      \
                              ::mwtest::Describe(#a, #b, mw_a, mw_b)); \
    }                                                                  \
  } while (0)

#define MW_EXPECT_NEAR(a, b, tolerance)                                \
  do {                                                                 \
    const double mw_a = static_cast<double>(a);                        \
    const double mw_b = static_cast<double>(b);                        \
    if (!(fabs(mw_a - mw_b) <= (tolerance))) {                         \
      ::mwtest::RecordFailure(__FILE__, __LINE__,                      \
                              ::mwtest::Describe(#a, #b, mw_a, mw_b)); \
    }                                                                  \
  } while (0)

#define MW_EXPECT_OK(status)                                          \
  do {                                                                \
    const ::mw::Status mw_s = (status);                               \
    if (!mw_s.ok()) {                                                 \
      ::mwtest::RecordFailure(__FILE__, __LINE__,                     \
                              "expected OK, got " + mw_s.ToString()); \
    }                                                                 \
  } while (0)

#define MW_EXPECT_CODE(status, expected_code)                              \
  do {                                                                     \
    const ::mw::Status mw_s = (status);                                    \
    if (mw_s.code() != (expected_code)) {                                  \
      ::mwtest::RecordFailure(                                             \
          __FILE__, __LINE__,                                              \
          "expected " + std::string(::mw::StatusCodeName(expected_code)) + \
              ", got " + mw_s.ToString());                                 \
    }                                                                      \
  } while (0)

// Ends the current test early without failing it (missing optional
// dependency); the reason is printed.
#define MW_SKIP(reason)                  \
  do {                                   \
    printf("    SKIPPED: %s\n", reason); \
    return;                              \
  } while (0)

// Aborts the current test when |cond| is false.
#define MW_ASSERT_TRUE(cond)                                              \
  do {                                                                    \
    if (!(cond)) {                                                        \
      ::mwtest::RecordFailure(__FILE__, __LINE__,                         \
                              std::string("assertion failed: ") + #cond); \
      return;                                                             \
    }                                                                     \
  } while (0)

#define MW_ASSERT_OK(status)                                               \
  do {                                                                     \
    const ::mw::Status mw_s = (status);                                    \
    if (!mw_s.ok()) {                                                      \
      ::mwtest::RecordFailure(__FILE__, __LINE__,                          \
                              "assertion failed, got " + mw_s.ToString()); \
      return;                                                              \
    }                                                                      \
  } while (0)

#endif  // TESTS_TEST_FRAMEWORK_H_  // NOLINT(build/header_guard)
