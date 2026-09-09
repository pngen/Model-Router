// Model Router - minimal deterministic test framework.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_TESTS_SUPPORT_TEST_FRAMEWORK_HPP
#define MODEL_ROUTER_TESTS_SUPPORT_TEST_FRAMEWORK_HPP

#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace mrtest {

/// Renders a value for a failure message. Enums print as their numeric value;
/// types without a stream operator print a placeholder instead of failing to
/// compile.
template <class T>
[[nodiscard]] std::string describe(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (requires(std::ostream& stream, const T& item) { stream << item; }) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return "<value>";
  }
}

/// One registered test case.
struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

/// Process-wide registry. Registration happens during static initialization.
class Registry {
 public:
  static Registry& instance();
  void add(std::string suite, std::string name, std::function<void()> body);
  [[nodiscard]] const std::vector<TestCase>& cases() const noexcept { return cases_; }

 private:
  std::vector<TestCase> cases_;
};

/// Registers a test case.
struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body) {
    Registry::instance().add(suite, name, std::move(body));
  }
};

/// Thrown when a check fails. The runner reports the message and continues with
/// the next test case.
class Failure : public std::exception {
 public:
  explicit Failure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

[[noreturn]] void fail(const char* file, int line, const std::string& message);

/// Runs every registered test. Returns the process exit code: 0 when all tests
/// passed, 1 otherwise.
int run_all(int argc, char** argv);

}  // namespace mrtest

#define MR_TEST(suite, name)                                                        \
  static void mr_test_##suite##_##name();                                           \
  static ::mrtest::Registrar mr_registrar_##suite##_##name(#suite, #name,           \
                                                           &mr_test_##suite##_##name); \
  static void mr_test_##suite##_##name()

#define MR_CHECK(condition)                                                          \
  do {                                                                               \
    if (!(condition)) {                                                              \
      ::mrtest::fail(__FILE__, __LINE__, "check failed: " #condition);               \
    }                                                                                \
  } while (false)

#define MR_CHECK_EQ(lhs, rhs)                                                        \
  do {                                                                               \
    const auto& mr_lhs = (lhs);                                                      \
    const auto& mr_rhs = (rhs);                                                      \
    if (!(mr_lhs == mr_rhs)) {                                                       \
      ::mrtest::fail(__FILE__, __LINE__,                                             \
                      std::string("check failed: " #lhs " == " #rhs " (") +          \
                          ::mrtest::describe(mr_lhs) + " vs " +                      \
                          ::mrtest::describe(mr_rhs) + ")");                         \
    }                                                                                \
  } while (false)

#define MR_CHECK_NE(lhs, rhs)                                                        \
  do {                                                                               \
    if ((lhs) == (rhs)) {                                                            \
      ::mrtest::fail(__FILE__, __LINE__, "check failed: " #lhs " != " #rhs);         \
    }                                                                                \
  } while (false)

#endif  // MODEL_ROUTER_TESTS_SUPPORT_TEST_FRAMEWORK_HPP
