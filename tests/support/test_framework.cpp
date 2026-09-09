// Model Router - minimal deterministic test framework.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "test_framework.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <cstdlib>
#else
#include <csignal>
#endif

namespace mrtest {

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(std::string suite, std::string name, std::function<void()> body) {
  cases_.push_back(TestCase{std::move(suite), std::move(name), std::move(body)});
}

void fail(const char* file, int line, const std::string& message) {
  std::ostringstream stream;
  stream << file << ":" << line << ": " << message;
  throw Failure(stream.str());
}

int run_all(int argc, char** argv) {
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--filter") == 0 && index + 1 < argc) {
      filter = argv[index + 1];
      ++index;
    }
  }

  std::vector<TestCase> cases = Registry::instance().cases();
  std::stable_sort(cases.begin(), cases.end(), [](const TestCase& lhs, const TestCase& rhs) {
    if (lhs.suite != rhs.suite) {
      return lhs.suite < rhs.suite;
    }
    return lhs.name < rhs.name;
  });

  std::size_t passed = 0;
  std::size_t failed = 0;
  std::size_t skipped = 0;
  std::vector<std::string> failures;

  for (const TestCase& test : cases) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      ++skipped;
      continue;
    }
    try {
      test.body();
      ++passed;
      std::printf("PASS %s\n", full.c_str());
    } catch (const Failure& failure) {
      ++failed;
      std::printf("FAIL %s\n  %s\n", full.c_str(), failure.what());
      failures.push_back(full + ": " + failure.what());
    } catch (const std::exception& error) {
      ++failed;
      std::printf("FAIL %s\n  unexpected exception: %s\n", full.c_str(), error.what());
      failures.push_back(full + ": unexpected exception: " + error.what());
    } catch (...) {
      ++failed;
      std::printf("FAIL %s\n  unexpected non-standard exception\n", full.c_str());
      failures.push_back(full + ": unexpected non-standard exception");
    }
    std::fflush(stdout);
  }

  std::printf("\n%zu passed, %zu failed, %zu skipped (of %zu)\n", passed, failed, skipped,
              cases.size());
  if (!failures.empty()) {
    std::printf("failures:\n");
    for (const std::string& failure : failures) {
      std::printf("  %s\n", failure.c_str());
    }
  }
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}

}  // namespace mrtest

int main(int argc, char** argv) {
#if defined(_WIN32)
  // A failing test must never raise a modal abort or crash dialog: the process
  // exits with a status instead of waiting for a human.
  (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#else
  (void)std::signal(SIGPIPE, SIG_IGN);
#endif
  return mrtest::run_all(argc, argv);
}
