// Model Router - reference multiprocess proof harness.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// This harness runs REAL multiprocess execution on ONE physical host: a
// coordinator process and three backend worker processes, all independent
// operating-system processes, communicating over framed loopback TCP. It never
// claims physical multi-node routing.
//
// No sleep, no timeout, and no arbitrary wait is used anywhere. Readiness is
// established by prompt polling of the real coordinator.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "coordinator.hpp"
#include "model_router/distributed/transport.hpp"
#include "model_router/version.hpp"
#include "process.hpp"

namespace {

using model_router::OutcomeCode;
using model_router::distributed::Frame;
using model_router::distributed::MessageType;
using model_router::distributed::TransportOptions;
using model_router::reference::ChildProcess;
using model_router::reference::ChildProcessOptions;

int g_failures = 0;
int g_proof = 0;

void report(const char* name, bool ok, const std::string& detail = {}) {
  ++g_proof;
  if (ok) {
    std::printf("PROOF %02d %s OK %s\n", g_proof, name, detail.c_str());
  } else {
    ++g_failures;
    std::printf("PROOF %02d %s FAIL %s\n", g_proof, name, detail.c_str());
  }
  std::fflush(stdout);
}

[[nodiscard]] Frame make_frame(MessageType type, std::vector<std::uint8_t> payload) {
  Frame frame;
  frame.header.magic = model_router::distributed::frame_magic;
  frame.header.protocol_version = model_router::protocol_version;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.payload = std::move(payload);
  return frame;
}

/// A real client connection to the coordinator process.
class Client {
 public:
  [[nodiscard]] bool connect(const std::string& host, std::uint16_t port,
                             const model_router::ResourceLimits& limits, std::string* error) {
    transport_.limits = limits;
    connection_ = model_router::distributed::FrameConnector::connect(host, port, transport_, error);
    return connection_ != nullptr;
  }

  [[nodiscard]] bool send(MessageType type, std::vector<std::uint8_t> payload, std::string* error) {
    return connection_->send(make_frame(type, std::move(payload)), error);
  }

  [[nodiscard]] bool receive(Frame* frame, std::string* error) {
    return connection_->receive(frame, error);
  }

  void close() {
    if (connection_ != nullptr) {
      connection_->close();
      connection_.reset();
    }
  }

  [[nodiscard]] const model_router::ResourceLimits& limits() const noexcept {
    return transport_.limits;
  }

 private:
  TransportOptions transport_{};
  std::unique_ptr<model_router::distributed::FramedConnection> connection_;
};

/// Polls the coordinator until it accepts a connection. Each attempt returns
/// promptly; nothing is slept on.
[[nodiscard]] bool wait_for_coordinator(const std::string& host, std::uint16_t port,
                                        const model_router::ResourceLimits& limits,
                                        const ChildProcess* child) {
  for (int attempt = 0; attempt < 20000; ++attempt) {
    if (child != nullptr && !child->running()) {
      return false;
    }
    Client probe;
    std::string error;
    if (probe.connect(host, port, limits, &error)) {
      probe.close();
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool wait_for_backend_count(Client* client, std::uint32_t expected,
                                          const ChildProcess* child) {
  for (int attempt = 0; attempt < 20000; ++attempt) {
    if (child != nullptr && !child->running()) {
      return false;
    }
    model_router::distributed::AdminRequest request;
    request.op = model_router::distributed::AdminOp::BACKEND_COUNT;
    std::vector<std::uint8_t> payload;
    std::string error;
    if (!model_router::distributed::encode_admin_request(request, client->limits(), &payload,
                                                         &error)) {
      return false;
    }
    if (!client->send(MessageType::ADMIN_REQUEST, std::move(payload), &error)) {
      return false;
    }
    Frame frame;
    if (!client->receive(&frame, &error)) {
      return false;
    }
    model_router::distributed::AdminResponse response;
    if (!model_router::distributed::decode_admin_response(frame.payload, client->limits(),
                                                          &response, &error)) {
      return false;
    }
    if (static_cast<std::uint32_t>(std::strtoul(response.detail.c_str(), nullptr, 10)) >=
        expected) {
      return true;
    }
  }
  return false;
}

/// Polls the coordinator until the given backend incarnation reports current
/// dynamic evidence. Each attempt returns promptly.
[[nodiscard]] bool wait_for_backend_evidence(Client* client, std::uint64_t backend_id,
                                             const ChildProcess* child);

[[nodiscard]] model_router::distributed::AdminResponse admin(
    Client* client, model_router::distributed::AdminOp op, std::uint64_t generation = 0,
    std::uint64_t value = 0, model_router::BackendId backend_id = {},
    model_router::ModelId model_id = {}, model_router::RouteDecisionId decision_id = {},
    std::string detail = {}) {
  model_router::distributed::AdminRequest request;
  request.op = op;
  request.generation = generation;
  request.value = value;
  request.backend_id = backend_id;
  request.model_id = model_id;
  request.decision_id = decision_id;
  request.detail = std::move(detail);
  model_router::distributed::AdminResponse response;
  response.code = OutcomeCode::INTERNAL_ERROR;
  response.detail = "admin request could not be sent";
  std::vector<std::uint8_t> payload;
  std::string error;
  if (!model_router::distributed::encode_admin_request(request, client->limits(), &payload,
                                                       &error)) {
    response.detail = error;
    return response;
  }
  if (!client->send(MessageType::ADMIN_REQUEST, std::move(payload), &error)) {
    response.detail = error;
    return response;
  }
  Frame frame;
  if (!client->receive(&frame, &error)) {
    response.detail = error;
    return response;
  }
  if (static_cast<MessageType>(frame.header.message_type) != MessageType::ADMIN_RESPONSE) {
    response.detail = "unexpected response type";
    return response;
  }
  if (!model_router::distributed::decode_admin_response(frame.payload, client->limits(), &response,
                                                        &error)) {
    response.detail = error;
  }
  return response;
}

bool wait_for_backend_evidence(Client* client, std::uint64_t backend_id,
                              const ChildProcess* child) {
  const std::string needle = "\"backend_id\":" + std::to_string(backend_id);
  for (int attempt = 0; attempt < 20000; ++attempt) {
    if (child != nullptr && !child->running()) {
      return false;
    }
    const auto response = admin(client, model_router::distributed::AdminOp::SNAPSHOT);
    const std::size_t at = response.json.find(needle);
    if (at != std::string::npos) {
      const std::string window = response.json.substr(at, 500);
      if (window.find("\"evidence_current\":true") != std::string::npos &&
          window.find("\"price_known\":true") != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] model_router::RouteRequest make_request(model_router::TenantId tenant,
                                                      std::uint64_t policy_generation,
                                                      std::string capability = {}) {
  model_router::RouteRequest request;
  request.request_id = model_router::allocate_id<model_router::RouteRequestTag>();
  request.request_generation = model_router::RouteRequestGeneration(1);
  request.tenant = tenant;
  request.name_space = model_router::NamespaceId(1);
  request.policy_id = model_router::PolicyId(1);
  request.policy_generation = model_router::PolicyGeneration(policy_generation);
  request.estimated_input_tokens = 1000;
  request.estimated_output_tokens = 500;
  request.requirements.required_input_modalities =
      model_router::ModalitySet(static_cast<std::uint32_t>(model_router::Modality::TEXT));
  request.requirements.required_output_modalities =
      model_router::ModalitySet(static_cast<std::uint32_t>(model_router::Modality::TEXT));
  request.requirements.retry_policy.max_fallbacks = 2;
  request.requirements.require_known_cost = true;
  if (!capability.empty()) {
    model_router::CapabilityRequirement requirement;
    requirement.key = model_router::CapabilityKey(std::move(capability));
    requirement.minimum_state = model_router::CapabilityState::DECLARED;
    request.requirements.required_capabilities.push_back(std::move(requirement));
  }
  return request;
}

[[nodiscard]] model_router::distributed::RouteResponseMessage route(
    Client* client, const model_router::RouteRequest& request) {
  model_router::distributed::RouteRequestMessage message;
  message.request = request;
  model_router::distributed::RouteResponseMessage response;
  response.code = OutcomeCode::INTERNAL_ERROR;
  response.detail = "route request could not be sent";
  std::vector<std::uint8_t> payload;
  std::string error;
  if (!model_router::distributed::encode_route_request(message, client->limits(), &payload,
                                                       &error)) {
    response.detail = error;
    return response;
  }
  if (!client->send(MessageType::ROUTE_REQUEST, std::move(payload), &error)) {
    response.detail = error;
    return response;
  }
  Frame frame;
  if (!client->receive(&frame, &error)) {
    response.detail = error;
    return response;
  }
  if (!model_router::distributed::decode_route_response(frame.payload, client->limits(), &response,
                                                        &error)) {
    response.detail = error;
  }
  return response;
}

[[nodiscard]] std::string read_file_text(const std::string& path) {
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, path.c_str(), "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "rb");
#endif
  if (file == nullptr) {
    return {};
  }
  std::string text;
  char buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text.append(buffer, read);
  }
  std::fclose(file);
  return text;
}

}  // namespace

int main(int argc, char** argv) {
  model_router::reference::suppress_failure_dialogs();
  std::string executable_directory = model_router::reference::current_executable_directory();
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::string(argv[index]) == "--bin-dir") {
      executable_directory = argv[index + 1];
    }
  }
#if defined(_WIN32)
  const std::string suffix = ".exe";
#else
  const std::string suffix;
#endif
  const std::string coordinator_binary = executable_directory + "/router_coordinator" + suffix;
  const std::string worker_binary = executable_directory + "/backend_worker" + suffix;
  const std::string state_path = executable_directory + "/reference_proof.state";
  const model_router::ResourceLimits limits;
  const model_router::reference::Catalog catalog = model_router::reference::make_catalog();
  const model_router::TenantId tenant(7);
  const std::string host = "127.0.0.1";

  // -----------------------------------------------------------------------
  // 1. Real multiprocess topology.
  // -----------------------------------------------------------------------
  ChildProcessOptions coordinator_options;
  coordinator_options.executable = coordinator_binary;
  coordinator_options.arguments = {"--port", "0"};
  std::string error;
  std::unique_ptr<ChildProcess> coordinator =
      ChildProcess::spawn(coordinator_options, &error);
  if (coordinator == nullptr) {
    report("topology.spawn_coordinator", false, error);
    return 1;
  }
  // The coordinator chooses an ephemeral port; discover it by scanning the
  // range it reports through the state file is not possible, so bind a fixed
  // port for the reference proof.
  (void)coordinator->terminate(1, &error);
  const std::uint16_t base_port = 17800;
  coordinator_options.arguments = {"--port", std::to_string(base_port)};
  coordinator = ChildProcess::spawn(coordinator_options, &error);
  if (coordinator == nullptr || !wait_for_coordinator(host, base_port, limits, coordinator.get())) {
    report("topology.spawn_coordinator", false, "coordinator did not become ready");
    return 1;
  }
  report("topology.spawn_coordinator", true,
         "pid=" + std::to_string(coordinator->process_id()) + " port=" + std::to_string(base_port));

  Client control;
  if (!control.connect(host, base_port, limits, &error)) {
    report("topology.control_connect", false, error);
    return 1;
  }
  report("topology.control_connect", true, "framed TCP loopback");

  // Install an open policy at generation 1.
  {
    const auto response = admin(&control, model_router::distributed::AdminOp::SET_POLICY_OPEN, 1);
    report("topology.open_policy", response.code == OutcomeCode::ACCEPTED, response.detail);
  }

  struct WorkerSpec {
    std::uint64_t backend_id;
    std::uint64_t boot;
    std::uint64_t cost_base;
    std::uint32_t latency_micros;
  };
  const std::vector<WorkerSpec> specs = {{1, 1, 100, 1000}, {2, 1, 400, 2000}, {3, 1, 900, 3000}};
  std::vector<std::unique_ptr<ChildProcess>> workers;
  const auto start_worker = [&](const WorkerSpec& spec) -> std::unique_ptr<ChildProcess> {
    ChildProcessOptions options;
    options.executable = worker_binary;
    options.arguments = {"--port", std::to_string(base_port), "--backend-id",
                         std::to_string(spec.backend_id), "--boot", std::to_string(spec.boot),
                         "--cost-base", std::to_string(spec.cost_base), "--latency-micros",
                         std::to_string(spec.latency_micros)};
    std::string spawn_error;
    std::unique_ptr<ChildProcess> child = ChildProcess::spawn(options, &spawn_error);
    if (child == nullptr) {
      report("topology.spawn_worker", false, spawn_error);
    }
    return child;
  };

  for (std::size_t index = 0; index < specs.size(); ++index) {
    workers.push_back(start_worker(specs[index]));
    if (workers.back() == nullptr) {
      return 1;
    }
    if (!wait_for_backend_count(&control, static_cast<std::uint32_t>(index + 1),
                                workers.back().get()) ||
        !wait_for_backend_evidence(&control, specs[index].backend_id, workers.back().get())) {
      report("topology.worker_registered", false,
             "backend " + std::to_string(specs[index].backend_id) +
                 " did not register and publish current evidence");
      return 1;
    }
  }
  report("topology.worker_registered", true, "3 independent backend processes registered");

  // -----------------------------------------------------------------------
  // 2. Capability routing over the real transport.
  // -----------------------------------------------------------------------
  std::string winner_backend;
  model_router::RouteDecision first_decision;
  {
    const auto response = route(&control, make_request(tenant, 1, "task/code"));
    const bool ok = response.code == OutcomeCode::ROUTED && response.has_decision;
    if (ok) {
      first_decision = response.decision;
      winner_backend = std::to_string(response.decision.authority.backend_id.value());
    }
    if (!ok) {
      const auto snapshot = admin(&control, model_router::distributed::AdminOp::SNAPSHOT);
      std::printf("  diagnostic snapshot: %s\n", snapshot.json.substr(0, 1200).c_str());
      std::printf("  diagnostic detail: %s\n", response.detail.substr(0, 1200).c_str());
    }
    report("routing.capability_winner", ok && response.decision.authority.backend_id.value() == 1,
           "winner_backend=" + winner_backend + " model=" +
               std::to_string(response.decision.authority.model_id.value()) + " code=" +
               std::string(model_router::to_string(response.code)));
  }

  // -----------------------------------------------------------------------
  // 3. Real dispatch handoff to the worker process.
  // -----------------------------------------------------------------------
  {
    const auto response =
        admin(&control, model_router::distributed::AdminOp::DISPATCH, 0, 0, {},
              {}, first_decision.decision_id);
    report("dispatch.real_handoff", response.code == OutcomeCode::DISPATCHED,
           "code=" + std::string(model_router::to_string(response.code)) + " " + response.detail);
  }

  // -----------------------------------------------------------------------
  // 4. Real backend death: kill the winning backend process.
  // -----------------------------------------------------------------------
  const std::uint32_t killed_pid = workers[0]->process_id();
  {
    std::string terminate_error;
    const bool killed = workers[0]->terminate(1, &terminate_error);
    // Poll the coordinator until it observes the loss through the real socket.
    bool observed = false;
    for (int attempt = 0; attempt < 20000; ++attempt) {
      const auto response =
          admin(&control, model_router::distributed::AdminOp::BACKEND_COUNT);
      if (static_cast<std::uint32_t>(std::strtoul(response.detail.c_str(), nullptr, 10)) == 2) {
        observed = true;
        break;
      }
    }
    report("death.real_process_kill", killed && observed,
           "pid=" + std::to_string(killed_pid) + " observed=" + (observed ? "1" : "0"));
  }

  // -----------------------------------------------------------------------
  // 5. Old route is no longer dispatchable, and stale replay is rejected.
  // -----------------------------------------------------------------------
  {
    const auto response =
        admin(&control, model_router::distributed::AdminOp::REVALIDATE, 0, 0, {},
              {}, first_decision.decision_id);
    const bool rejected = model_router::is_rejection(response.code);
    report("authority.old_route_not_dispatchable", rejected,
           "code=" + std::string(model_router::to_string(response.code)));
  }
  {
    // Replay the dead incarnation's registration and health through a fresh
    // connection. Both must be rejected before any canonical mutation.
    Client replay;
    std::string connect_error;
    if (!replay.connect(host, base_port, limits, &connect_error)) {
      report("authority.stale_replay_rejected", false, connect_error);
    } else {
      model_router::distributed::BackendRegisterMessage message;
      message.backend = model_router::reference::make_backend(
          catalog, model_router::BackendId(1), model_router::BackendGeneration(1),
          model_router::BackendBootId(1),
          model_router::BackendRegistrationGeneration(2), host + ":" + std::to_string(base_port),
          model_router::Provenance::SYNTHETIC, 1);
      std::vector<std::uint8_t> payload;
      std::string encode_error;
      model_router::distributed::BackendRegisterAckMessage ack;
      bool registered_ok = false;
      if (model_router::distributed::encode_backend_register(message, limits, &payload,
                                                             &encode_error) &&
          replay.send(MessageType::BACKEND_REGISTER, std::move(payload), &encode_error)) {
        Frame frame;
        if (replay.receive(&frame, &encode_error) &&
            model_router::distributed::decode_backend_register_ack(frame.payload, limits, &ack,
                                                                   &encode_error)) {
          registered_ok = ack.code == OutcomeCode::ACCEPTED;
        }
      }
      report("authority.stale_registration_rejected", !registered_ok,
             "code=" + std::string(model_router::to_string(ack.code)));

      model_router::HealthEvidence health;
      health.backend_id = model_router::BackendId(1);
      health.backend_boot = model_router::BackendBootId(1);
      health.generation = model_router::HealthGeneration(99);
      health.state = model_router::HealthState::HEALTHY;
      health.observed_at_unix_millis = 1;
      model_router::distributed::AdminResponse status;
      std::string encode_error2;
      if (model_router::distributed::encode_health(health, limits, &payload, &encode_error2) &&
          replay.send(MessageType::HEALTH_UPDATE, std::move(payload), &encode_error2)) {
        Frame frame;
        if (replay.receive(&frame, &encode_error2)) {
          (void)model_router::distributed::decode_admin_response(frame.payload, limits, &status,
                                                                 &encode_error2);
        }
      }
      report("authority.stale_health_rejected", status.code != OutcomeCode::ACCEPTED,
             "code=" + std::string(model_router::to_string(status.code)));
      replay.close();
    }
  }

  // -----------------------------------------------------------------------
  // 6. Fresh routing uses the deterministic remaining evidence.
  // -----------------------------------------------------------------------
  {
    const auto response = route(&control, make_request(tenant, 1, "task/code"));
    const bool ok = response.code == OutcomeCode::ROUTED &&
                    response.decision.authority.backend_id.value() == 2;
    report("routing.reroutes_to_surviving_backend", ok,
           "winner_backend=" + std::to_string(response.decision.authority.backend_id.value()));
  }

  // -----------------------------------------------------------------------
  // 7. Independent backend boot authority.
  // -----------------------------------------------------------------------
  {
    const auto response = admin(&control, model_router::distributed::AdminOp::SNAPSHOT);
    const bool backend_two_present = response.json.find("\"backend_id\":2") != std::string::npos;
    const bool backend_three_present =
        response.json.find("\"backend_id\":3") != std::string::npos;
    const bool backend_one_absent = response.json.find("\"backend_id\":1") == std::string::npos;
    report("lifecycle.independent_boot_authority",
           backend_two_present && backend_three_present && backend_one_absent,
           "survivors unaffected by the death of backend 1");
  }

  // -----------------------------------------------------------------------
  // 8. A-prime: restart backend 1 with a fresh incarnation.
  // -----------------------------------------------------------------------
  {
    WorkerSpec prime{1, 2, 100, 1000};
    workers[0] = start_worker(prime);
    const bool restarted =
        workers[0] != nullptr && wait_for_backend_count(&control, 3, workers[0].get()) &&
        wait_for_backend_evidence(&control, 1, workers[0].get());
    report("lifecycle.prime_registered", restarted, "fresh boot=2");
    const auto response = route(&control, make_request(tenant, 1, "task/code"));
    // The fresh incarnation must be eligible again. It is not required to win:
    // continuity stickiness legitimately prefers the incumbent backend.
    bool prime_eligible = false;
    for (const model_router::RankedCandidate& candidate : response.decision.explanation.ranking) {
      if (candidate.key.backend_id.value() == 1 && candidate.key.backend_boot.value() == 2) {
        prime_eligible = true;
      }
    }
    if (!prime_eligible) {
      std::printf("  diagnostic ranking=%zu code=%s detail=%s\n",
                  response.decision.explanation.ranking.size(),
                  std::string(model_router::to_string(response.code)).c_str(),
                  response.detail.substr(0, 400).c_str());
      for (const model_router::RankedCandidate& candidate :
           response.decision.explanation.ranking) {
        std::printf("    ranked backend=%llu boot=%llu score=%lld\n",
                    static_cast<unsigned long long>(candidate.key.backend_id.value()),
                    static_cast<unsigned long long>(candidate.key.backend_boot.value()),
                    static_cast<long long>(candidate.score));
      }
      for (const model_router::RouteRejection& rejection :
           response.decision.explanation.rejections) {
        std::printf("    rejected model=%llu backend=%llu boot=%llu code=%s %s\n",
                    static_cast<unsigned long long>(rejection.model_id),
                    static_cast<unsigned long long>(rejection.backend_id),
                    static_cast<unsigned long long>(rejection.backend_boot.value()),
                    std::string(model_router::to_string(rejection.code)).c_str(),
                    rejection.detail.c_str());
      }
    }
    report("lifecycle.prime_eligible", response.code == OutcomeCode::ROUTED && prime_eligible,
           "eligible=" + std::string(prime_eligible ? "1" : "0") + " winner_backend=" +
               std::to_string(response.decision.authority.backend_id.value()) + " boot=" +
               std::to_string(response.decision.authority.backend_boot.value()));
  }

  // -----------------------------------------------------------------------
  // 9. Policy change proof.
  // -----------------------------------------------------------------------
  {
    const auto before = route(&control, make_request(tenant, 1, "task/code"));
    const auto denied = admin(&control, model_router::distributed::AdminOp::SET_POLICY_DENY_BACKEND,
                              2, 0, model_router::BackendId(1));
    const auto stale = admin(&control, model_router::distributed::AdminOp::REVALIDATE, 0, 0, {},
                             {}, before.decision.decision_id);
    const auto after = route(&control, make_request(tenant, 2, "task/code"));
    const bool ok = denied.code == OutcomeCode::ACCEPTED &&
                    model_router::is_rejection(stale.code) &&
                    after.code == OutcomeCode::ROUTED &&
                    after.decision.authority.backend_id.value() != 1;
    report("authority.policy_change", ok,
           "old_code=" + std::string(model_router::to_string(stale.code)) +
               " new_backend=" + std::to_string(after.decision.authority.backend_id.value()));
    // Restore an open policy at a later generation.
    (void)admin(&control, model_router::distributed::AdminOp::SET_POLICY_OPEN, 3);
  }

  // -----------------------------------------------------------------------
  // 10. Cost change proof.
  // -----------------------------------------------------------------------
  {
    const auto before = route(&control, make_request(tenant, 3, "task/code"));
    // Price generation 2 makes backend 1 expensive and backend 2 cheapest.
    const auto price_one =
        admin(&control, model_router::distributed::AdminOp::ADVANCE_PRICE, 2, 9000,
              model_router::BackendId(1));
    const auto price_two =
        admin(&control, model_router::distributed::AdminOp::ADVANCE_PRICE, 2, 10,
              model_router::BackendId(2));
    const auto stale = admin(&control, model_router::distributed::AdminOp::REVALIDATE, 0, 0, {},
                             {}, before.decision.decision_id);
    const auto after = route(&control, make_request(tenant, 3, "task/code"));
    const bool ok = price_one.code == OutcomeCode::ACCEPTED &&
                    price_two.code == OutcomeCode::ACCEPTED &&
                    model_router::is_rejection(stale.code) &&
                    after.code == OutcomeCode::ROUTED &&
                    after.decision.authority.backend_id.value() == 2;
    report("authority.cost_change", ok,
           "old_code=" + std::string(model_router::to_string(stale.code)) +
               " new_backend=" + std::to_string(after.decision.authority.backend_id.value()));
  }

  // -----------------------------------------------------------------------
  // 11. Capability change proof.
  // -----------------------------------------------------------------------
  {
    // "execution/offline" is published by the backend worker itself, so revoking
    // the backend's claims genuinely removes it from the eligible set.
    const std::string capability(model_router::capability_keys::offline_execution);
    const auto before = route(&control, make_request(tenant, 3, capability));
    if (before.code != OutcomeCode::ROUTED) {
      std::printf("  diagnostic capability-before code=%s detail=%s\n",
                  std::string(model_router::to_string(before.code)).c_str(),
                  before.detail.substr(0, 600).c_str());
    }
    const auto revoked =
        admin(&control, model_router::distributed::AdminOp::REVOKE_CAPABILITY, 0, 0,
              model_router::BackendId(2));
    const auto stale = admin(&control, model_router::distributed::AdminOp::REVALIDATE, 0, 0, {},
                             {}, before.decision.decision_id);
    const auto after = route(&control, make_request(tenant, 3, capability));
    const bool excluded =
        after.code != OutcomeCode::ROUTED ||
        after.decision.authority.backend_id.value() != before.decision.authority.backend_id.value();
    report("authority.capability_change",
           revoked.code == OutcomeCode::ACCEPTED && model_router::is_rejection(stale.code) &&
               excluded,
           "old_code=" + std::string(model_router::to_string(stale.code)) + " revoke=" +
               std::string(model_router::to_string(revoked.code)) + " before_backend=" +
               std::to_string(before.decision.authority.backend_id.value()) + " after_backend=" +
               std::to_string(after.decision.authority.backend_id.value()));
  }

  // -----------------------------------------------------------------------
  // 12. No-eligible-candidate proof.
  // -----------------------------------------------------------------------
  {
    const auto response = route(&control, make_request(tenant, 3, "task/embedding"));
    const bool ok = response.code == OutcomeCode::NO_ELIGIBLE_CANDIDATE &&
                    response.detail.find("REJECT_CAPABILITY") != std::string::npos;
    report("routing.no_eligible_candidate", ok, "detail=" + response.detail.substr(0, 160));
  }

  // -----------------------------------------------------------------------
  // 13. Fallback proof.
  // -----------------------------------------------------------------------
  {
    const auto response = route(&control, make_request(tenant, 3, "task/code"));
    bool has_fallback = false;
    model_router::RouteDecision decision;
    if (response.code == OutcomeCode::ROUTED) {
      decision = response.decision;
      has_fallback = !decision.fallbacks.empty();
    }
    report("routing.fallback_plan", has_fallback,
           "fallbacks=" + std::to_string(decision.fallbacks.size()));
    if (has_fallback) {
      const auto rerouted = admin(&control, model_router::distributed::AdminOp::REROUTE, 0,
                                  static_cast<std::uint64_t>(
                                      model_router::FailureClass::BACKEND_UNAVAILABLE),
                                  {}, {}, decision.decision_id);
      const bool ok = rerouted.code == OutcomeCode::ROUTED;
      report("routing.fallback_selected", ok,
             "code=" + std::string(model_router::to_string(rerouted.code)) + " " +
                 rerouted.detail.substr(0, 80));
    }
  }

  // -----------------------------------------------------------------------
  // 14. Invariants hold in the live coordinator.
  // -----------------------------------------------------------------------
  {
    const auto response = admin(&control, model_router::distributed::AdminOp::INVARIANTS);
    const bool ok = response.json.find("\"ok\":true") != std::string::npos;
    report("hardening.invariants", ok, response.json.substr(0, 96));
  }

  // -----------------------------------------------------------------------
  // 15. Persistence and router restart proof.
  // -----------------------------------------------------------------------
  {
    const auto saved = admin(&control, model_router::distributed::AdminOp::SAVE, 0, 0, {}, {}, {},
                             state_path);
    const std::string before = read_file_text(state_path);
    const auto before_summary = admin(&control, model_router::distributed::AdminOp::SUMMARY);
    report("persistence.saved", saved.code == OutcomeCode::ACCEPTED && !before.empty(),
           "bytes=" + std::to_string(before.size()));

    // Kill the coordinator process for real, then start a fresh one that loads
    // the durable state.
    std::string terminate_error;
    (void)coordinator->terminate(1, &terminate_error);
    for (const std::unique_ptr<ChildProcess>& worker : workers) {
      if (worker != nullptr) {
        (void)worker->terminate(1, &terminate_error);
      }
    }
    control.close();

    ChildProcessOptions restart_options;
    restart_options.executable = coordinator_binary;
    restart_options.arguments = {"--port", std::to_string(base_port), "--state", state_path,
                                 "--load"};
    std::unique_ptr<ChildProcess> restarted =
        ChildProcess::spawn(restart_options, &terminate_error);
    const bool ready =
        restarted != nullptr && wait_for_coordinator(host, base_port, limits, restarted.get());
    report("recovery.coordinator_restarted", ready, "fresh process loaded durable state");
    if (ready) {
      Client recovered;
      std::string connect_error;
      if (!recovered.connect(host, base_port, limits, &connect_error)) {
        report("recovery.reconnect", false, connect_error);
      } else {
        const auto summary = admin(&recovered, model_router::distributed::AdminOp::SUMMARY);
        const auto stale = admin(&recovered, model_router::distributed::AdminOp::REVALIDATE, 0, 0,
                                 {}, {}, first_decision.decision_id);
        const bool epochs_advanced =
            summary.json.find("\"recovered\":true") != std::string::npos;
        report("recovery.epochs_advanced", epochs_advanced, summary.json.substr(0, 96));
        report("recovery.old_route_not_dispatchable", model_router::is_rejection(stale.code),
               "code=" + std::string(model_router::to_string(stale.code)));
        // Policy and dynamic evidence are deliberately not resurrected, so the
        // recovered router needs a current policy and freshly registered
        // backends before it can route again.
        const auto reinstated =
            admin(&recovered, model_router::distributed::AdminOp::SET_POLICY_OPEN, 1);
        const auto fresh = route(&recovered, make_request(tenant, 1, "task/code"));
        report("recovery.requires_fresh_registration",
               reinstated.code == OutcomeCode::ACCEPTED &&
                   fresh.code == OutcomeCode::NO_ELIGIBLE_CANDIDATE,
               "code=" + std::string(model_router::to_string(fresh.code)));
        recovered.close();
      }
      (void)restarted->terminate(1, &terminate_error);
    }
  }

  std::remove(state_path.c_str());
  std::printf("\nreference harness: %d proofs, %d failures\n", g_proof, g_failures);
  std::fflush(stdout);
  return g_failures == 0 ? 0 : 1;
}
