// Model Router - reference child-process control.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "process.hpp"

#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <windows.h>

#include <cstdlib>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace model_router::reference {
namespace {

[[nodiscard]] std::string quote_argument(const std::string& value) {
  if (value.find_first_of(" \t\"") == std::string::npos) {
    return value;
  }
  std::string out = "\"";
  for (const char character : value) {
    if (character == '"') {
      out += "\\\"";
    } else {
      out.push_back(character);
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace

struct ChildProcess::Impl {
#if defined(_WIN32)
  PROCESS_INFORMATION information{};
  bool running{false};
#else
  pid_t pid{-1};
  bool running{false};
#endif
};

ChildProcess::~ChildProcess() {
  if (impl_ != nullptr && impl_->running) {
    std::string error;
    (void)terminate(1, &error);
  }
}

std::unique_ptr<ChildProcess> ChildProcess::spawn(const ChildProcessOptions& options,
                                                  std::string* error) {
  if (options.executable.empty()) {
    if (error != nullptr) {
      *error = "child executable path is empty";
    }
    return nullptr;
  }
  auto child = std::unique_ptr<ChildProcess>(new ChildProcess());
  child->impl_ = std::make_unique<Impl>();

#if defined(_WIN32)
  std::string command_line = quote_argument(options.executable);
  for (const std::string& argument : options.arguments) {
    command_line.push_back(' ');
    command_line += quote_argument(argument);
  }
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
  const char* working_directory =
      options.working_directory.empty() ? nullptr : options.working_directory.c_str();
  if (CreateProcessA(options.executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                     flags, nullptr, working_directory, &startup, &information) == 0) {
    if (error != nullptr) {
      *error = "CreateProcess failed with code " + std::to_string(GetLastError());
    }
    return nullptr;
  }
  CloseHandle(information.hThread);
  child->impl_->information = information;
  child->impl_->running = true;
#else
  const pid_t pid = ::fork();
  if (pid < 0) {
    if (error != nullptr) {
      *error = "fork failed";
    }
    return nullptr;
  }
  if (pid == 0) {
    if (!options.working_directory.empty()) {
      (void)::chdir(options.working_directory.c_str());
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(options.executable.c_str()));
    for (const std::string& argument : options.arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(options.executable.c_str(), argv.data());
    ::_exit(127);
  }
  child->impl_->pid = pid;
  child->impl_->running = true;
#endif
  return child;
}

bool ChildProcess::wait_for_exit(std::uint32_t* exit_code, std::string* error) {
  if (impl_ == nullptr) {
    if (error != nullptr) {
      *error = "child process is not initialized";
    }
    return false;
  }
#if defined(_WIN32)
  if (WaitForSingleObject(impl_->information.hProcess, INFINITE) != WAIT_OBJECT_0) {
    if (error != nullptr) {
      *error = "WaitForSingleObject failed with code " + std::to_string(GetLastError());
    }
    return false;
  }
  DWORD code = 0;
  if (GetExitCodeProcess(impl_->information.hProcess, &code) == 0) {
    if (error != nullptr) {
      *error = "GetExitCodeProcess failed with code " + std::to_string(GetLastError());
    }
    return false;
  }
  CloseHandle(impl_->information.hProcess);
  impl_->information.hProcess = nullptr;
  impl_->running = false;
  if (exit_code != nullptr) {
    *exit_code = static_cast<std::uint32_t>(code);
  }
  return true;
#else
  int status = 0;
  if (::waitpid(impl_->pid, &status, 0) < 0) {
    if (error != nullptr) {
      *error = "waitpid failed";
    }
    return false;
  }
  impl_->running = false;
  if (exit_code != nullptr) {
    *exit_code = WIFEXITED(status) ? static_cast<std::uint32_t>(WEXITSTATUS(status))
                                   : static_cast<std::uint32_t>(128 + WTERMSIG(status));
  }
  return true;
#endif
}

bool ChildProcess::terminate(std::uint32_t exit_code, std::string* error) {
  if (impl_ == nullptr) {
    if (error != nullptr) {
      *error = "child process is not initialized";
    }
    return false;
  }
  if (!impl_->running) {
    return true;
  }
#if defined(_WIN32)
  if (TerminateProcess(impl_->information.hProcess, static_cast<UINT>(exit_code)) == 0) {
    if (error != nullptr) {
      *error = "TerminateProcess failed with code " + std::to_string(GetLastError());
    }
    return false;
  }
  (void)WaitForSingleObject(impl_->information.hProcess, INFINITE);
  CloseHandle(impl_->information.hProcess);
  impl_->information.hProcess = nullptr;
  impl_->running = false;
  return true;
#else
  if (::kill(impl_->pid, SIGKILL) != 0) {
    if (error != nullptr) {
      *error = "kill failed";
    }
    return false;
  }
  int status = 0;
  (void)::waitpid(impl_->pid, &status, 0);
  impl_->running = false;
  return true;
#endif
}

std::uint32_t ChildProcess::process_id() const noexcept {
  if (impl_ == nullptr) {
    return 0;
  }
#if defined(_WIN32)
  return static_cast<std::uint32_t>(impl_->information.dwProcessId);
#else
  return static_cast<std::uint32_t>(impl_->pid);
#endif
}

bool ChildProcess::running() const noexcept { return impl_ != nullptr && impl_->running; }

bool read_stdin_line(std::string* line) {
  if (line == nullptr) {
    return false;
  }
  line->clear();
  int character = 0;
  bool any = false;
  while ((character = std::fgetc(stdin)) != EOF) {
    any = true;
    if (character == '\n') {
      return true;
    }
    if (character != '\r') {
      line->push_back(static_cast<char>(character));
    }
  }
  return any;
}

std::string current_executable_path() {
#if defined(_WIN32)
  char buffer[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (length == 0) {
    return {};
  }
  return std::string(buffer, buffer + length);
#else
  char buffer[4096] = {};
  const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length <= 0) {
    return {};
  }
  return std::string(buffer, buffer + length);
#endif
}

void suppress_failure_dialogs() {
#if defined(_WIN32)
  (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

std::string current_executable_directory() {
  const std::string path = current_executable_path();
  const std::size_t separator = path.find_last_of("\\/");
  if (separator == std::string::npos) {
    return {};
  }
  return path.substr(0, separator);
}

}  // namespace model_router::reference
