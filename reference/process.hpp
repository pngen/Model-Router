// Model Router - reference child-process control. Reference architecture only.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_REFERENCE_PROCESS_HPP
#define MODEL_ROUTER_REFERENCE_PROCESS_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace model_router::reference {

/// Spawn parameters for a real independent operating-system process.
struct ChildProcessOptions {
  std::string executable;
  std::vector<std::string> arguments;
  std::string working_directory;
};

/// A real independent operating-system process. On Windows the child is created
/// with CREATE_NO_WINDOW so no console window is ever displayed.
class ChildProcess {
 public:
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] static std::unique_ptr<ChildProcess> spawn(const ChildProcessOptions& options,
                                                           std::string* error);

  /// Blocks until the process exits and reports its exit code. No timeout is
  /// used: a hang is a defect in the child, not something to wait out.
  [[nodiscard]] bool wait_for_exit(std::uint32_t* exit_code, std::string* error);

  /// Requests immediate operating-system termination. This is a real process
  /// kill, not a cooperative flag.
  [[nodiscard]] bool terminate(std::uint32_t exit_code, std::string* error);

  [[nodiscard]] std::uint32_t process_id() const noexcept;
  [[nodiscard]] bool running() const noexcept;

 private:
  ChildProcess() = default;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Reads one line from standard input of this process. Used by the reference
/// mains for a simple readiness handshake with the harness.
[[nodiscard]] bool read_stdin_line(std::string* line);

/// Path of the currently running executable.
[[nodiscard]] std::string current_executable_path();

/// Directory of the currently running executable.
[[nodiscard]] std::string current_executable_directory();

/// Suppresses modal crash and abort dialogs in this process so a failure is
/// reported as an exit status instead of a window waiting for a human.
void suppress_failure_dialogs();

}  // namespace model_router::reference

#endif  // MODEL_ROUTER_REFERENCE_PROCESS_HPP
