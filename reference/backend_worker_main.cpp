// Model Router - reference backend worker process.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// This worker is a real independent operating-system process that publishes
// boot-bound evidence and answers dispatch handoffs with a deterministic
// SYNTHETIC model step. It performs no model inference and makes no quality
// claim. When built with CUDA support it additionally proves a REAL local CUDA
// device probe bound to its current incarnation.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "model_router/distributed/transport.hpp"
#include "model_router/model_router.hpp"
#include "model_router/version.hpp"
#include "process.hpp"

#if defined(MODEL_ROUTER_HAS_CUDA)
#include "model_router/cuda/cuda_reference_probe.hpp"
#endif

namespace {

using model_router::distributed::Frame;
using model_router::distributed::MessageType;
using model_router::distributed::TransportOptions;

struct Options {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::uint64_t backend_id{1};
  std::uint64_t boot{1};
  std::uint64_t generation{1};
  std::uint64_t registration{1};
  std::uint64_t cost_base{100};
  std::uint32_t latency_micros{1000};
  std::uint32_t queue_micros{100};
  std::uint64_t price_generation{1};
  bool publish_cuda_capability{true};
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
    if (flag == "--host") {
      options->host = next();
    } else if (flag == "--port") {
      options->port = static_cast<std::uint16_t>(std::strtoul(next().c_str(), nullptr, 10));
    } else if (flag == "--backend-id") {
      options->backend_id = std::strtoull(next().c_str(), nullptr, 10);
    } else if (flag == "--boot") {
      options->boot = std::strtoull(next().c_str(), nullptr, 10);
    } else if (flag == "--generation") {
      options->generation = std::strtoull(next().c_str(), nullptr, 10);
    } else if (flag == "--registration") {
      options->registration = std::strtoull(next().c_str(), nullptr, 10);
    } else if (flag == "--cost-base") {
      options->cost_base = std::strtoull(next().c_str(), nullptr, 10);
    } else if (flag == "--latency-micros") {
      options->latency_micros =
          static_cast<std::uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
    } else if (flag == "--queue-micros") {
      options->queue_micros = static_cast<std::uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
    } else if (flag == "--price-generation") {
      options->price_generation = std::strtoull(next().c_str(), nullptr, 10);
    } else if (flag == "--no-cuda") {
      options->publish_cuda_capability = false;
    } else {
      return false;
    }
  }
  return options->port != 0;
}

[[nodiscard]] Frame make_frame(MessageType type, std::vector<std::uint8_t> payload) {
  Frame frame;
  frame.header.magic = model_router::distributed::frame_magic;
  frame.header.protocol_version = model_router::protocol_version;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.payload = std::move(payload);
  return frame;
}

/// Deterministic synthetic model step. It is a fixed arithmetic transform over
/// the dispatch identity; it is not inference and claims no model quality.
[[nodiscard]] std::uint64_t synthetic_step(const model_router::CandidateKey& key,
                                           std::uint32_t attempt) noexcept {
  std::uint64_t value = 0xCBF29CE484222325ull;
  const auto mix = [&value](std::uint64_t item) {
    value ^= item;
    value *= 0x100000001B3ull;
  };
  mix(key.model_id.value());
  mix(key.model_generation.value());
  mix(key.backend_id.value());
  mix(key.backend_boot.value());
  mix(key.endpoint_id.value());
  mix(attempt);
  return value;
}

}  // namespace

int main(int argc, char** argv) {
  model_router::reference::suppress_failure_dialogs();
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::fprintf(stderr, "usage: backend_worker --port N --backend-id N --boot N ...\n");
    return 2;
  }

  model_router::reference::ChildProcessOptions self;
  (void)self;

  TransportOptions transport;
  transport.bind_address = options.host;
  std::string error;
  std::unique_ptr<model_router::distributed::FramedConnection> connection =
      model_router::distributed::FrameConnector::connect(options.host, options.port, transport,
                                                         &error);
  if (connection == nullptr) {
    std::fprintf(stderr, "connect failed: %s\n", error.c_str());
    return 3;
  }

  const model_router::reference::Catalog catalog = model_router::reference::make_catalog();
  const model_router::BackendId backend_id(options.backend_id);
  const model_router::BackendBootId boot(options.boot);
  const model_router::BackendGeneration generation(options.generation);
  const model_router::BackendRegistrationGeneration registration(options.registration);
  const model_router::UnixMillis now = 1;

  // --- hello -------------------------------------------------------------
  {
    model_router::distributed::HelloMessage hello;
    hello.backend_id = backend_id;
    hello.backend_generation = generation;
    hello.backend_boot = boot;
    hello.product = std::string(model_router::product_string());
    hello.protocol_version = model_router::protocol_version;
    std::vector<std::uint8_t> payload;
    if (!model_router::distributed::encode_hello(hello, transport.limits, &payload, &error)) {
      std::fprintf(stderr, "encode hello failed: %s\n", error.c_str());
      return 3;
    }
    if (!connection->send(make_frame(MessageType::HELLO, std::move(payload)), &error)) {
      std::fprintf(stderr, "send hello failed: %s\n", error.c_str());
      return 3;
    }
    Frame response;
    if (!connection->receive(&response, &error) ||
        response.header.message_type != static_cast<std::uint16_t>(MessageType::HELLO_ACK)) {
      std::fprintf(stderr, "hello was not acknowledged: %s\n", error.c_str());
      return 3;
    }
  }

  // --- registration ------------------------------------------------------
  {
    model_router::BackendDescriptor backend = model_router::reference::make_backend(
        catalog, backend_id, generation, boot, registration,
        options.host + ":" + std::to_string(options.port),
        model_router::Provenance::SYNTHETIC, now);
    model_router::distributed::BackendRegisterMessage message;
    message.backend = std::move(backend);
    std::vector<std::uint8_t> payload;
    if (!model_router::distributed::encode_backend_register(message, transport.limits, &payload,
                                                            &error)) {
      std::fprintf(stderr, "encode register failed: %s\n", error.c_str());
      return 3;
    }
    if (!connection->send(make_frame(MessageType::BACKEND_REGISTER, std::move(payload)), &error)) {
      std::fprintf(stderr, "send register failed: %s\n", error.c_str());
      return 3;
    }
    Frame response;
    if (!connection->receive(&response, &error)) {
      std::fprintf(stderr, "registration was not acknowledged: %s\n", error.c_str());
      return 3;
    }
    model_router::distributed::BackendRegisterAckMessage ack;
    if (!model_router::distributed::decode_backend_register_ack(response.payload, transport.limits,
                                                                &ack, &error)) {
      std::fprintf(stderr, "registration acknowledgement was malformed: %s\n", error.c_str());
      return 3;
    }
    if (ack.code != model_router::OutcomeCode::ACCEPTED) {
      std::fprintf(stderr, "registration rejected: %s\n", ack.detail.c_str());
      return 4;
    }
  }

  // --- capability publication (boot-bound) -------------------------------
  {
    model_router::BackendDescriptor backend = model_router::reference::make_backend(
        catalog, backend_id, generation, boot, registration,
        options.host + ":" + std::to_string(options.port),
        model_router::Provenance::SYNTHETIC, now);
    model_router::distributed::CapabilityPublishMessage message;
    message.publication.backend_id = backend_id;
    message.publication.backend_generation = generation;
    message.publication.backend_boot = boot;
    message.publication.profile_id = backend.capability_profile_id;
    message.publication.generation = model_router::CapabilityGeneration(1);
    const auto claim = [&](std::string_view key, model_router::CapabilityState state) {
      model_router::CapabilityEvidence evidence;
      evidence.key = model_router::CapabilityKey(std::string(key));
      evidence.state = state;
      evidence.generation = message.publication.generation;
      evidence.profile_id = message.publication.profile_id;
      evidence.backend_boot = boot;
      evidence.observed_at_unix_millis = now;
      evidence.source = "reference-backend-worker";
      message.publication.claims.push_back(std::move(evidence));
    };
    claim(model_router::capability_keys::streaming, model_router::CapabilityState::VERIFIED);
    claim(model_router::capability_keys::local_execution, model_router::CapabilityState::VERIFIED);
    claim(model_router::capability_keys::offline_execution, model_router::CapabilityState::VERIFIED);
    claim(model_router::capability_keys::text_generation, model_router::CapabilityState::VERIFIED);

#if defined(MODEL_ROUTER_HAS_CUDA)
    if (options.publish_cuda_capability) {
      const model_router::cuda::DeviceProbe probe = model_router::cuda::run_device_probe(4096);
      if (probe.available && probe.parity_verified) {
        // REAL evidence: the claim is bound to this process incarnation and is
        // only published after a real device probe succeeded.
        claim(model_router::capability_keys::cuda_execution,
              model_router::CapabilityState::VERIFIED);
        std::printf("CUDA_PROBE device=\"%s\" cc=%u.%u parity=1 memory_restored=%d\n",
                    probe.device_name.c_str(), probe.compute_capability_major,
                    probe.compute_capability_minor, probe.memory_restored ? 1 : 0);
      } else {
        std::printf("CUDA_PROBE available=0 detail=%s\n", probe.detail.c_str());
      }
      std::fflush(stdout);
    }
#else
    (void)options.publish_cuda_capability;
#endif

    std::vector<std::uint8_t> payload;
    if (!model_router::distributed::encode_capability_publish(message, transport.limits, &payload,
                                                              &error)) {
      std::fprintf(stderr, "encode capabilities failed: %s\n", error.c_str());
      return 3;
    }
    if (!connection->send(make_frame(MessageType::CAPABILITY_PUBLISH, std::move(payload)),
                          &error)) {
      std::fprintf(stderr, "send capabilities failed: %s\n", error.c_str());
      return 3;
    }
    Frame response;
    if (!connection->receive(&response, &error)) {
      std::fprintf(stderr, "capability publication was not acknowledged: %s\n", error.c_str());
      return 3;
    }
    model_router::distributed::AdminResponse status;
    std::string decode_error;
    if (!model_router::distributed::decode_admin_response(response.payload, transport.limits,
                                                          &status, &decode_error) ||
        (status.code != model_router::OutcomeCode::ACCEPTED &&
         status.code != model_router::OutcomeCode::NO_CHANGE)) {
      std::fprintf(stderr, "capability publication rejected: %s %s\n",
                   std::string(model_router::to_string(status.code)).c_str(),
                   status.detail.c_str());
      return 3;
    }
  }

  // --- dynamic evidence, bound to this incarnation ------------------------
  // Every publication is acknowledged with a typed status. A rejection is
  // reported loudly instead of being silently ignored.
  const auto send_evidence = [&connection, &transport, &error](MessageType type,
                                                               std::vector<std::uint8_t> payload) {
    if (!connection->send(make_frame(type, std::move(payload)), &error)) {
      std::fprintf(stderr, "send %s failed: %s\n",
                   std::string(model_router::distributed::to_string(type)).c_str(),
                   error.c_str());
      return false;
    }
    Frame response;
    if (!connection->receive(&response, &error)) {
      std::fprintf(stderr, "no status for %s: %s\n",
                   std::string(model_router::distributed::to_string(type)).c_str(),
                   error.c_str());
      return false;
    }
    model_router::distributed::AdminResponse status;
    std::string decode_error;
    if (!model_router::distributed::decode_admin_response(response.payload, transport.limits,
                                                          &status, &decode_error)) {
      std::fprintf(stderr, "malformed status for %s: %s\n",
                   std::string(model_router::distributed::to_string(type)).c_str(),
                   decode_error.c_str());
      return false;
    }
    if (status.code != model_router::OutcomeCode::ACCEPTED &&
        status.code != model_router::OutcomeCode::NO_CHANGE) {
      std::fprintf(stderr, "publication %s rejected: %s %s\n",
                   std::string(model_router::distributed::to_string(type)).c_str(),
                   std::string(model_router::to_string(status.code)).c_str(),
                   status.detail.c_str());
      return false;
    }
    return true;
  };

  {
    model_router::HealthEvidence health;
    health.backend_id = backend_id;
    health.backend_boot = boot;
    health.backend_generation = generation;
    health.generation = model_router::HealthGeneration(1);
    health.state = model_router::HealthState::HEALTHY;
    health.observed_at_unix_millis = now;
    health.source = "reference-backend-worker";
    std::vector<std::uint8_t> payload;
    if (!model_router::distributed::encode_health(health, transport.limits, &payload, &error) ||
        !send_evidence(MessageType::HEALTH_UPDATE, std::move(payload))) {
      std::fprintf(stderr, "health publication failed: %s\n", error.c_str());
      return 3;
    }
  }
  {
    model_router::AvailabilityEvidence availability;
    availability.backend_id = backend_id;
    availability.backend_boot = boot;
    availability.backend_generation = generation;
    availability.generation = model_router::AvailabilityGeneration(1);
    availability.state = model_router::AvailabilityState::AVAILABLE;
    availability.observed_at_unix_millis = now;
    availability.source = "reference-backend-worker";
    std::vector<std::uint8_t> payload;
    if (!model_router::distributed::encode_availability(availability, transport.limits, &payload,
                                                        &error) ||
        !send_evidence(MessageType::AVAILABILITY_UPDATE, std::move(payload))) {
      std::fprintf(stderr, "availability publication failed: %s\n", error.c_str());
      return 3;
    }
  }
  {
    model_router::ReadinessEvidence readiness;
    readiness.backend_id = backend_id;
    readiness.backend_boot = boot;
    readiness.backend_generation = generation;
    readiness.generation = model_router::ReadinessGeneration(1);
    readiness.state = model_router::ReadinessState::READY;
    readiness.observed_at_unix_millis = now;
    readiness.source = "reference-backend-worker";
    std::vector<std::uint8_t> payload;
    if (!model_router::distributed::encode_readiness(readiness, transport.limits, &payload,
                                                     &error) ||
        !send_evidence(MessageType::READINESS_UPDATE, std::move(payload))) {
      std::fprintf(stderr, "readiness publication failed: %s\n", error.c_str());
      return 3;
    }
  }
  {
    model_router::ResidencyEvidence residency;
    residency.backend_id = backend_id;
    residency.backend_boot = boot;
    residency.backend_generation = generation;
    residency.generation = model_router::ResidencyGeneration(1);
    residency.state = model_router::ResidencyState::RESIDENT;
    residency.observed_at_unix_millis = now;
    residency.source = "reference-backend-worker";
    std::vector<std::uint8_t> payload;
    if (!model_router::distributed::encode_residency(residency, transport.limits, &payload,
                                                     &error) ||
        !send_evidence(MessageType::RESIDENCY_UPDATE, std::move(payload))) {
      std::fprintf(stderr, "residency publication failed: %s\n", error.c_str());
      return 3;
    }
  }
  {
    model_router::CapacityEvidence capacity;
    capacity.backend_id = backend_id;
    capacity.backend_boot = boot;
    capacity.backend_generation = generation;
    capacity.generation = model_router::CapacityGeneration(1);
    capacity.state = model_router::CapacityState::AVAILABLE;
    capacity.observed_at_unix_millis = now;
    capacity.source = "reference-backend-worker";
    model_router::CapacityDetail detail;
    detail.backend_id = backend_id;
    detail.backend_boot = boot;
    detail.available_slots = 16;
    detail.total_slots = 16;
    detail.observed_at_unix_millis = now;
    std::vector<std::uint8_t> payload;
    if (!model_router::distributed::encode_capacity(capacity, detail, transport.limits, &payload,
                                                    &error) ||
        !send_evidence(MessageType::CAPACITY_UPDATE, std::move(payload))) {
      std::fprintf(stderr, "capacity publication failed: %s\n", error.c_str());
      return 3;
    }
  }
  {
    model_router::LatencyEvidence latency;
    latency.backend_id = backend_id;
    latency.backend_boot = boot;
    latency.health_generation = model_router::HealthGeneration(1);
    latency.dispatch_micros = options.latency_micros;
    latency.queue_micros = options.queue_micros;
    latency.tail_micros = options.latency_micros;
    latency.completed_micros = options.latency_micros;
    latency.sample_count = 32;
    latency.observed_at_unix_millis = now;
    latency.source = "reference-backend-worker";
    std::vector<std::uint8_t> payload;
    if (!model_router::distributed::encode_latency(latency, transport.limits, &payload, &error) ||
        !send_evidence(MessageType::LATENCY_UPDATE, std::move(payload))) {
      std::fprintf(stderr, "latency publication failed: %s\n", error.c_str());
      return 3;
    }
  }
  {
    const model_router::PriceGeneration price_generation(options.price_generation);
    const std::int64_t base = static_cast<std::int64_t>(options.cost_base);
    const auto publish = [&](model_router::ModelId model_id, std::int64_t total) {
      const model_router::CostEvidence cost = model_router::reference::make_cost_evidence(
          backend_id, model_id, price_generation, total, total, total, now);
      std::vector<std::uint8_t> payload;
      if (!model_router::distributed::encode_cost(cost, transport.limits, &payload, &error) ||
          !send_evidence(MessageType::COST_UPDATE, std::move(payload))) {
        return false;
      }
      return true;
    };
    if (!publish(catalog.small_model, base) || !publish(catalog.general_model, base * 3) ||
        !publish(catalog.specialist_model, base * 5)) {
      std::fprintf(stderr, "cost publication failed: %s\n", error.c_str());
      return 3;
    }
  }

  std::printf("READY backend=%llu boot=%llu\n",
              static_cast<unsigned long long>(backend_id.value()),
              static_cast<unsigned long long>(boot.value()));
  std::fflush(stdout);

  // --- serve --------------------------------------------------------------
  for (;;) {
    Frame frame;
    if (!connection->receive(&frame, &error)) {
      break;
    }
    const auto type = static_cast<MessageType>(frame.header.message_type);
    if (type == MessageType::PING) {
      std::vector<std::uint8_t> empty;
      if (!connection->send(make_frame(MessageType::PONG, std::move(empty)), &error)) {
        break;
      }
      continue;
    }
    if (type == MessageType::BYE) {
      break;
    }
    if (type != MessageType::DISPATCH) {
      continue;
    }
    model_router::distributed::DispatchMessage dispatch;
    if (!model_router::distributed::decode_dispatch(frame.payload, transport.limits, &dispatch,
                                                    &error)) {
      continue;
    }
    model_router::distributed::DispatchResultMessage result;
    result.dispatch_id = dispatch.dispatch_id;
    result.decision_id = dispatch.decision_id;
    result.code = model_router::OutcomeCode::DISPATCHED;
    result.failure = model_router::FailureClass::UNKNOWN;
    result.latency_micros = options.latency_micros;
    const std::uint64_t checksum = synthetic_step(dispatch.target, dispatch.attempt);
    result.detail = "synthetic-step=" + std::to_string(checksum);
    std::vector<std::uint8_t> payload;
    if (!model_router::distributed::encode_dispatch_result(result, transport.limits, &payload,
                                                           &error)) {
      break;
    }
    if (!connection->send(make_frame(MessageType::DISPATCH_RESULT, std::move(payload)), &error)) {
      break;
    }
  }

  connection->close();
  std::printf("STOPPED backend=%llu boot=%llu\n",
              static_cast<unsigned long long>(backend_id.value()),
              static_cast<unsigned long long>(boot.value()));
  std::fflush(stdout);
  return 0;
}
