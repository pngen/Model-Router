// Model Router - reference coordinator process.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// A real independent operating-system process hosting one ModelRouter. It owns
// route decisions and route authority; it never performs model inference.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "coordinator.hpp"
#include "model_router/version.hpp"
#include "process.hpp"

namespace {

struct Options {
  model_router::reference::CoordinatorOptions coordinator;
  bool quiet{false};
};

[[nodiscard]] bool parse_options(int argc, char** argv, Options* options) {
  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];
    const auto next = [&argc, &argv, &index]() -> std::string {
      if (index + 1 >= argc) {
        return {};
      }
      return argv[++index];
    };
    if (flag == "--port") {
      options->coordinator.port = static_cast<std::uint16_t>(std::strtoul(next().c_str(), nullptr, 10));
    } else if (flag == "--bind") {
      options->coordinator.bind_address = next();
    } else if (flag == "--state") {
      options->coordinator.state_path = next();
    } else if (flag == "--load") {
      options->coordinator.load_on_start = true;
    } else if (flag == "--save") {
      options->coordinator.save_on_shutdown = true;
    } else if (flag == "--router-generation") {
      options->coordinator.router_generation = model_router::RouterGeneration(
          std::strtoull(next().c_str(), nullptr, 10));
    } else if (flag == "--quiet") {
      options->quiet = true;
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  model_router::reference::suppress_failure_dialogs();
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::fprintf(stderr, "usage: router_coordinator [--port N] [--bind addr] [--state file] "
                         "[--load] [--save]\n");
    return 2;
  }

  model_router::reference::Coordinator coordinator(std::move(options.coordinator));
  std::string error;
  if (!coordinator.start(&error)) {
    std::fprintf(stderr, "coordinator start failed: %s\n", error.c_str());
    return 3;
  }

  const model_router::RouterSummary summary = coordinator.router().summary();
  std::printf("READY port=%u router=%llu router_epoch=%llu coordinator_epoch=%llu boot=%llu\n",
              static_cast<unsigned>(coordinator.port()),
              static_cast<unsigned long long>(summary.router_id.value()),
              static_cast<unsigned long long>(summary.router_epoch.value()),
              static_cast<unsigned long long>(summary.coordinator_epoch.value()),
              static_cast<unsigned long long>(summary.router_boot.value()));
  std::fflush(stdout);

  std::string line;
  while (model_router::reference::read_stdin_line(&line)) {
    if (line == "quit" || line == "shutdown") {
      break;
    }
    if (line == "summary") {
      std::printf("%s", coordinator.router().summary().to_text().c_str());
      std::fflush(stdout);
    } else if (line == "invariants") {
      std::printf("%s", coordinator.router().check_invariants().to_text().c_str());
      std::fflush(stdout);
    }
  }

  coordinator.stop();
  std::printf("STOPPED\n");
  std::fflush(stdout);
  return 0;
}
