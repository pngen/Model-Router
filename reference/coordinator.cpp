// Model Router - reference coordinator.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "coordinator.hpp"

#include <algorithm>
#include <utility>

#include "model_router/version.hpp"

namespace model_router::reference {
namespace {

using distributed::Frame;
using distributed::MessageType;

[[nodiscard]] Frame make_frame(MessageType type, std::vector<std::uint8_t> payload) {
  Frame frame;
  frame.header.magic = distributed::frame_magic;
  frame.header.protocol_version = protocol_version;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.payload = std::move(payload);
  return frame;
}

template <class EncodeFn>
[[nodiscard]] bool reply(const std::shared_ptr<distributed::FramedConnection>& connection,
                         MessageType type, const ResourceLimits& limits, EncodeFn encode) {
  std::vector<std::uint8_t> payload;
  std::string error;
  if (!encode(&payload, &error)) {
    distributed::ErrorMessage message;
    message.code = OutcomeCode::REJECT_INVALID;
    message.detail = error;
    std::vector<std::uint8_t> error_payload;
    if (!distributed::encode_error(message, limits, &error_payload, &error)) {
      return false;
    }
    return connection->send(make_frame(MessageType::ERROR_MESSAGE, std::move(error_payload)),
                            &error);
  }
  return connection->send(make_frame(type, std::move(payload)), &error);
}

/// Sends a control-plane status for a mutation that has no dedicated response
/// message. The status frame carries the typed outcome code and detail.
[[nodiscard]] bool reply_status(const std::shared_ptr<distributed::FramedConnection>& connection,
                                const ResourceLimits& limits, OutcomeCode code,
                                const std::string& detail) {
  distributed::AdminResponse response;
  response.code = code;
  response.detail = detail;
  std::vector<std::uint8_t> payload;
  std::string error;
  if (!distributed::encode_admin_response(response, limits, &payload, &error)) {
    return false;
  }
  return connection->send(make_frame(MessageType::ADMIN_RESPONSE, std::move(payload)), &error);
}

[[nodiscard]] bool reply_error(const std::shared_ptr<distributed::FramedConnection>& connection,
                               const ResourceLimits& limits, OutcomeCode code,
                               const std::string& detail) {
  distributed::ErrorMessage message;
  message.code = code;
  message.detail = detail;
  std::vector<std::uint8_t> payload;
  std::string error;
  if (!distributed::encode_error(message, limits, &payload, &error)) {
    return false;
  }
  return connection->send(make_frame(MessageType::ERROR_MESSAGE, std::move(payload)), &error);
}

[[nodiscard]] CapabilityState capability_state_from(std::uint64_t value) noexcept {
  const auto state = static_cast<CapabilityState>(value);
  if (static_cast<std::uint8_t>(state) >= static_cast<std::uint8_t>(CapabilityState::kCount)) {
    return CapabilityState::REVOKED;
  }
  return state;
}

}  // namespace

Coordinator::Coordinator(CoordinatorOptions options)
    : options_(std::move(options)), catalog_(reference::make_catalog()) {}

Coordinator::~Coordinator() { stop(); }

bool Coordinator::start(std::string* error) {
  ModelRouterOptions router_options;
  router_options.router_generation = options_.router_generation;
  router_options.limits = options_.limits;
  router_options.providers.dispatcher = std::make_shared<Dispatcher>(this);
  router_options.persistence_path = options_.state_path;
  router_options.load_on_start = options_.load_on_start;
  router_options.save_on_shutdown = options_.save_on_shutdown;
  router_ = std::make_unique<ModelRouter>(std::move(router_options));

  const MutationResult started = router_->start();
  if (!started.accepted()) {
    if (error != nullptr) {
      *error = started.explanation.message();
    }
    return false;
  }

  // The coordinator owns the reference model catalog: backends publish which
  // models they serve, but semantic model identity is registered here.
  {
    ProviderDescriptor provider;
    provider.provider_id = catalog_.provider;
    provider.generation = catalog_.provider_generation;
    provider.display_name = "reference-provider";
    provider.default_trust_domain = TrustDomain::LOCAL;
    provider.third_party = false;
    const MutationResult registered_provider = router_->register_provider(provider);
    if (!registered_provider.accepted()) {
      if (error != nullptr) {
        *error = registered_provider.explanation.message();
      }
      return false;
    }
    for (const ModelDescriptor& model : reference::make_models(catalog_, 1)) {
      const MutationResult registered = router_->register_model(model);
      if (!registered.accepted()) {
        if (error != nullptr) {
          *error = registered.explanation.message();
        }
        return false;
      }
    }
  }

  distributed::TransportOptions transport;
  transport.bind_address = options_.bind_address;
  transport.port = options_.port;
  transport.limits = options_.limits;
  transport.max_send_queue_frames = options_.limits.max_send_queue_frames;
  if (!listener_.start(transport, error)) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = error == nullptr ? std::string("listener failed") : *error;
    return false;
  }
  accept_thread_ = std::thread(&Coordinator::accept_loop, this);
  return true;
}

void Coordinator::stop() {
  if (stopping_.exchange(true)) {
    return;
  }
  listener_.stop();
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  std::vector<std::shared_ptr<BackendLink>> links;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [backend_id, link] : links_) {
      (void)backend_id;
      links.push_back(link);
    }
  }
  for (const std::shared_ptr<BackendLink>& link : links) {
    if (link->connection != nullptr) {
      link->connection->close();
    }
    std::lock_guard<std::mutex> lock(link->mutex);
    link->closed = true;
    link->condition.notify_all();
  }
  std::vector<std::thread> sessions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions = std::move(sessions_);
  }
  for (std::thread& session : sessions) {
    if (session.joinable()) {
      session.join();
    }
  }
  if (router_ != nullptr) {
    (void)router_->shutdown();
    router_.reset();
  }
}

std::uint16_t Coordinator::port() const noexcept { return listener_.bound_port(); }

std::string Coordinator::last_error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

std::uint64_t Coordinator::sessions_served() const noexcept { return sessions_served_.load(); }
std::uint64_t Coordinator::frames_handled() const noexcept { return frames_handled_.load(); }

std::uint32_t Coordinator::live_links() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<std::uint32_t>(links_.size());
}

void Coordinator::accept_loop() {
  while (!stopping_.load()) {
    std::string error;
    std::unique_ptr<distributed::FramedConnection> connection = listener_.accept(&error);
    if (connection == nullptr) {
      if (stopping_.load()) {
        return;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_error_.empty()) {
          last_error_ = error;
        }
      }
      continue;
    }
    std::shared_ptr<distributed::FramedConnection> shared(std::move(connection));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (sessions_.size() >= options_.limits.max_session_threads) {
        shared->close();
        continue;
      }
      sessions_.emplace_back(&Coordinator::serve, this, shared);
    }
    sessions_served_.fetch_add(1);
  }
}

void Coordinator::serve(std::shared_ptr<distributed::FramedConnection> connection) {
  std::shared_ptr<BackendLink> link;
  bool keep_going = true;
  while (keep_going) {
    distributed::Frame frame;
    std::string error;
    if (!connection->receive(&frame, &error)) {
      break;
    }
    frames_handled_.fetch_add(1);
    // Resolve the link lazily: registration is what creates it.
    if (!handle_frame(connection, frame, &keep_going)) {
      break;
    }
    if (link == nullptr) {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& [backend_id, candidate] : links_) {
        (void)backend_id;
        if (candidate->connection == connection) {
          link = candidate;
          break;
        }
      }
    }
  }
  connection->close();

  // A closed session means the backend incarnation is gone. Fence its boot so
  // that no stale evidence from it can ever become authoritative again, and
  // wake any dispatch waiting on this link.
  if (link != nullptr) {
    {
      std::lock_guard<std::mutex> lock(link->mutex);
      link->closed = true;
      link->condition.notify_all();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      links_.erase(link->backend_id);
    }
    if (router_ != nullptr) {
      (void)router_->fence_backend_boot(link->backend_id, link->backend_generation,
                                        link->backend_boot, OutcomeCode::FENCED);
    }
  }
}

bool Coordinator::handle_frame(const std::shared_ptr<distributed::FramedConnection>& connection,
                               const distributed::Frame& frame, bool* keep_going) {
  const ResourceLimits& limits = options_.limits;
  const auto type = static_cast<MessageType>(frame.header.message_type);

  switch (type) {
    case MessageType::HELLO: {
      distributed::HelloMessage hello;
      std::string error;
      if (!distributed::decode_hello(frame.payload, limits, &hello, &error)) {
        return reply_error(connection, limits, OutcomeCode::REJECT_INVALID, error);
      }
      distributed::HelloAckMessage ack;
      ack.router_id = router_->router_id();
      ack.router_epoch = router_->router_epoch();
      ack.coordinator_epoch = router_->coordinator_epoch();
      ack.code = OutcomeCode::ACCEPTED;
      ack.detail = std::string(product_string());
      return reply(connection, MessageType::HELLO_ACK, limits,
                   [&ack, &limits](std::vector<std::uint8_t>* payload, std::string* encode_error) {
                     return distributed::encode_hello_ack(ack, limits, payload, encode_error);
                   });
    }
    case MessageType::BACKEND_REGISTER: {
      distributed::BackendRegisterMessage message;
      std::string error;
      if (!distributed::decode_backend_register(frame.payload, limits, &message, &error)) {
        return reply_error(connection, limits, OutcomeCode::REJECT_INVALID, error);
      }
      const MutationResult result = router_->register_backend(message.backend);
      distributed::BackendRegisterAckMessage ack;
      ack.backend_id = message.backend.backend_id;
      ack.backend_boot = message.backend.backend_boot;
      ack.code = result.code;
      ack.detail = result.explanation.message();
      if (result.accepted()) {
        auto link = std::make_shared<BackendLink>();
        link->backend_id = message.backend.backend_id;
        link->backend_generation = message.backend.backend_generation;
        link->backend_boot = message.backend.backend_boot;
        link->connection = connection;
        std::lock_guard<std::mutex> lock(mutex_);
        links_[message.backend.backend_id] = std::move(link);
      }
      return reply(connection, MessageType::BACKEND_REGISTER_ACK, limits,
                   [&ack, &limits](std::vector<std::uint8_t>* payload, std::string* encode_error) {
                     return distributed::encode_backend_register_ack(ack, limits, payload,
                                                                     encode_error);
                   });
    }
    case MessageType::CAPABILITY_PUBLISH: {
      distributed::CapabilityPublishMessage message;
      std::string error;
      if (!distributed::decode_capability_publish(frame.payload, limits, &message, &error)) {
        return reply_error(connection, limits, OutcomeCode::REJECT_INVALID, error);
      }
      const MutationResult result = router_->publish_capabilities(message.publication);
      return reply_status(connection, limits, result.code, result.explanation.message());
    }
    case MessageType::HEALTH_UPDATE: {
      HealthEvidence evidence;
      std::string error;
      if (!distributed::decode_health(frame.payload, limits, &evidence, &error)) {
        return reply_error(connection, limits, OutcomeCode::REJECT_INVALID, error);
      }
      const MutationResult result = router_->update_health(std::move(evidence));
      return reply_status(connection, limits, result.code, result.explanation.message());
    }
    case MessageType::AVAILABILITY_UPDATE: {
      AvailabilityEvidence evidence;
      std::string error;
      if (!distributed::decode_availability(frame.payload, limits, &evidence, &error)) {
        return reply_error(connection, limits, OutcomeCode::REJECT_INVALID, error);
      }
      const MutationResult result = router_->update_availability(std::move(evidence));
      return reply_status(connection, limits, result.code, result.explanation.message());
    }
    case MessageType::READINESS_UPDATE: {
      ReadinessEvidence evidence;
      std::string error;
      if (!distributed::decode_readiness(frame.payload, limits, &evidence, &error)) {
        return reply_error(connection, limits, OutcomeCode::REJECT_INVALID, error);
      }
      const MutationResult result = router_->update_readiness(std::move(evidence));
      return reply_status(connection, limits, result.code, result.explanation.message());
    }
    case MessageType::RESIDENCY_UPDATE: {
      ResidencyEvidence evidence;
      std::string error;
      if (!distributed::decode_residency(frame.payload, limits, &evidence, &error)) {
        return reply_error(connection, limits, OutcomeCode::REJECT_INVALID, error);
      }
      const MutationResult result = router_->update_residency(std::move(evidence));
      return reply_status(connection, limits, result.code, result.explanation.message());
    }
    case MessageType::CAPACITY_UPDATE: {
      CapacityEvidence evidence;
      CapacityDetail detail;
      std::string error;
      if (!distributed::decode_capacity(frame.payload, limits, &evidence, &detail, &error)) {
        return reply_error(connection, limits, OutcomeCode::REJECT_INVALID, error);
      }
      const MutationResult result = router_->update_capacity(std::move(evidence), std::move(detail));
      return reply_status(connection, limits, result.code, result.explanation.message());
    }
    case MessageType::LATENCY_UPDATE: {
      LatencyEvidence evidence;
      std::string error;
      if (!distributed::decode_latency(frame.payload, limits, &evidence, &error)) {
        return reply_error(connection, limits, OutcomeCode::REJECT_INVALID, error);
      }
      const MutationResult result = router_->update_latency(std::move(evidence));
      return reply_status(connection, limits, result.code, result.explanation.message());
    }
    case MessageType::COST_UPDATE: {
      CostEvidence evidence;
      std::string error;
      if (!distributed::decode_cost(frame.payload, limits, &evidence, &error)) {
        return reply_error(connection, limits, OutcomeCode::REJECT_INVALID, error);
      }
      const MutationResult result = router_->set_cost(std::move(evidence));
      return reply_status(connection, limits, result.code, result.explanation.message());
    }
    case MessageType::ROUTE_REQUEST: {
      distributed::RouteRequestMessage message;
      std::string error;
      if (!distributed::decode_route_request(frame.payload, limits, &message, &error)) {
        return reply_error(connection, limits, OutcomeCode::REJECT_INVALID, error);
      }
      const RouteOutcome outcome = router_->route(message.request);
      if (outcome.has_decision) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requests_.size() >= options_.limits.max_route_requests) {
          requests_.clear();
        }
        requests_[outcome.decision.decision_id] = message.request;
      }
      distributed::RouteResponseMessage response;
      response.code = outcome.code;
      response.has_decision = outcome.has_decision;
      response.decision = outcome.decision;
      response.detail = outcome.explanation.message();
      // A rejection must be explainable: carry the typed per-candidate reasons
      // even when no decision was produced.
      if (!outcome.has_decision) {
        for (const RouteRejection& rejection : outcome.route.rejections) {
          response.detail += " | model=" + std::to_string(rejection.model_id) +
                             " backend=" + std::to_string(rejection.backend_id) +
                             " code=" + std::string(to_string(rejection.code)) +
                             " " + rejection.detail;
        }
      }
      return reply(connection, MessageType::ROUTE_RESPONSE, limits,
                   [&response, &limits](std::vector<std::uint8_t>* payload,
                                        std::string* encode_error) {
                     return distributed::encode_route_response(response, limits, payload,
                                                               encode_error);
                   });
    }
    case MessageType::DISPATCH_RESULT: {
      distributed::DispatchResultMessage message;
      std::string error;
      if (!distributed::decode_dispatch_result(frame.payload, limits, &message, &error)) {
        return reply_error(connection, limits, OutcomeCode::REJECT_INVALID, error);
      }
      std::shared_ptr<BackendLink> link;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [backend_id, candidate] : links_) {
          (void)backend_id;
          if (candidate->connection == connection) {
            link = candidate;
            break;
          }
        }
      }
      if (link != nullptr) {
        std::lock_guard<std::mutex> lock(link->mutex);
        link->results[message.dispatch_id] = message;
        link->condition.notify_all();
      }
      return true;
    }
    case MessageType::ADMIN_REQUEST: {
      distributed::AdminRequest request;
      std::string error;
      if (!distributed::decode_admin_request(frame.payload, limits, &request, &error)) {
        return reply_error(connection, limits, OutcomeCode::REJECT_INVALID, error);
      }
      const distributed::AdminResponse response = handle_admin(request);
      return reply(connection, MessageType::ADMIN_RESPONSE, limits,
                   [&response, &limits](std::vector<std::uint8_t>* payload,
                                        std::string* encode_error) {
                     return distributed::encode_admin_response(response, limits, payload,
                                                               encode_error);
                   });
    }
    case MessageType::PING: {
      distributed::Frame pong = make_frame(MessageType::PONG, {});
      std::string error;
      return connection->send(pong, &error);
    }
    case MessageType::BYE: {
      if (keep_going != nullptr) {
        *keep_going = false;
      }
      return true;
    }
    case MessageType::SNAPSHOT_REQUEST: {
      const std::string json = router_->snapshot().to_json();
      return reply(connection, MessageType::SNAPSHOT_RESPONSE, limits,
                   [&json, &limits](std::vector<std::uint8_t>* payload, std::string* encode_error) {
                     distributed::PayloadWriter writer(limits);
                     writer.put_string(json);
                     if (!writer.ok()) {
                       if (encode_error != nullptr) {
                         *encode_error = writer.error();
                       }
                       return false;
                     }
                     *payload = writer.bytes();
                     return true;
                   });
    }
    default:
      return reply_error(connection, limits, OutcomeCode::REJECT_INVALID,
                         "unsupported reference message type");
  }
}

distributed::AdminResponse Coordinator::handle_admin(const distributed::AdminRequest& request) {
  distributed::AdminResponse response;
  response.code = OutcomeCode::ACCEPTED;

  const auto apply = [&response](const MutationResult& result) {
    response.code = result.code;
    response.detail = result.explanation.to_text();
  };

  switch (request.op) {
    case distributed::AdminOp::SNAPSHOT:
      response.json = router_->snapshot().to_json();
      return response;
    case distributed::AdminOp::INVARIANTS:
      response.json = router_->check_invariants().to_json();
      return response;
    case distributed::AdminOp::SUMMARY:
      response.json = router_->summary().to_json();
      return response;
    case distributed::AdminOp::BACKEND_COUNT:
      response.detail = std::to_string(router_->summary().backend_count);
      return response;
    case distributed::AdminOp::REVALIDATE: {
      const MutationResult result = router_->revalidate(request.decision_id);
      apply(result);
      return response;
    }
    case distributed::AdminOp::DISPATCH: {
      const DispatchRecord record = router_->dispatch(request.decision_id);
      response.code = record.code;
      response.detail = std::string(to_string(record.code)) + " " + record.detail;
      response.json = "{\"code\":\"" + std::string(to_string(record.code)) +
                      "\",\"handed_off\":" + (record.handed_off ? "true" : "false") + "}";
      return response;
    }
    case distributed::AdminOp::REROUTE: {
      RouteRequest stored;
      bool found = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iter = requests_.find(request.decision_id);
        if (iter != requests_.end()) {
          stored = iter->second;
          found = true;
        }
      }
      if (!found) {
        response.code = OutcomeCode::REJECT_INVALID;
        response.detail = "no retained request for the decision";
        return response;
      }
      const auto failure = static_cast<FailureClass>(request.value);
      const RouteOutcome outcome = router_->reroute(request.decision_id, failure, stored);
      response.code = outcome.code;
      response.detail = outcome.explanation.to_text();
      if (outcome.has_decision) {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_[outcome.decision.decision_id] = stored;
        response.json = std::to_string(outcome.decision.decision_id.value());
      }
      return response;
    }
    case distributed::AdminOp::SET_POLICY_OPEN: {
      PolicySnapshot policy =
          PolicyBuilder(PolicyId(1), PolicyGeneration(request.generation))
              .fallback_policy(FallbackPolicy::PERMITTED)
              .max_fallback_depth(4)
              .build();
      apply(router_->set_policy(std::move(policy)));
      return response;
    }
    case distributed::AdminOp::SET_POLICY_DENY_BACKEND: {
      PolicySnapshot policy =
          PolicyBuilder(PolicyId(1), PolicyGeneration(request.generation))
              .deny_backend(request.backend_id)
              .fallback_policy(FallbackPolicy::PERMITTED)
              .max_fallback_depth(4)
              .build();
      apply(router_->set_policy(std::move(policy)));
      return response;
    }
    case distributed::AdminOp::SET_POLICY_DENY_MODEL: {
      PolicySnapshot policy =
          PolicyBuilder(PolicyId(1), PolicyGeneration(request.generation))
              .deny_model(request.model_id)
              .fallback_policy(FallbackPolicy::PERMITTED)
              .max_fallback_depth(4)
              .build();
      apply(router_->set_policy(std::move(policy)));
      return response;
    }
    case distributed::AdminOp::SET_BUDGET_DENIED:
    case distributed::AdminOp::SET_BUDGET_ALLOWED: {
      BudgetSnapshot budget;
      budget.budget_id = BudgetId(1);
      budget.generation = BudgetGeneration(request.generation);
      budget.verdict = request.op == distributed::AdminOp::SET_BUDGET_DENIED
                           ? BudgetVerdict::DENIED
                           : BudgetVerdict::ALLOWED;
      budget.unit = reference::reference_cost_unit();
      apply(router_->set_budget(std::move(budget)));
      return response;
    }
    case distributed::AdminOp::ADVANCE_PRICE: {
      const BackendId backend_id = request.backend_id;
      const std::int64_t base = static_cast<std::int64_t>(request.value);
      const PriceGeneration generation(request.generation);
      const UnixMillis current = 1;
      // The small model is priced lowest so the deterministic winner is stable.
      MutationResult last = router_->set_cost(reference::make_cost_evidence(
          backend_id, catalog_.small_model, generation, base, base, base, current));
      if (last.accepted()) {
        last = router_->set_cost(reference::make_cost_evidence(
            backend_id, catalog_.general_model, generation, base * 3, base * 3, base * 3, current));
      }
      if (last.accepted()) {
        last = router_->set_cost(reference::make_cost_evidence(
            backend_id, catalog_.specialist_model, generation, base * 5, base * 5, base * 5,
            current));
      }
      apply(last);
      return response;
    }
    case distributed::AdminOp::FENCE_BACKEND: {
      BackendDescriptor backend;
      if (!router_->find_backend(request.backend_id, &backend)) {
        response.code = OutcomeCode::REJECT_STALE_BACKEND;
        response.detail = "backend is not registered";
        return response;
      }
      apply(router_->fence_backend_boot(backend.backend_id, backend.backend_generation,
                                        backend.backend_boot, OutcomeCode::FENCED));
      return response;
    }
    case distributed::AdminOp::RETIRE_MODEL: {
      apply(router_->retire_model(request.model_id,
                                  ModelGeneration(request.generation)));
      return response;
    }
    case distributed::AdminOp::REVOKE_CAPABILITY: {
      BackendDescriptor backend;
      if (!router_->find_backend(request.backend_id, &backend)) {
        response.code = OutcomeCode::REJECT_STALE_BACKEND;
        response.detail = "backend is not registered";
        return response;
      }
      std::uint64_t& counter = evidence_generations_[request.backend_id];
      counter = std::max(counter, backend.capability_generation.value()) + 1;
      BackendCapabilityPublication publication;
      publication.backend_id = backend.backend_id;
      publication.backend_generation = backend.backend_generation;
      publication.backend_boot = backend.backend_boot;
      publication.profile_id = backend.capability_profile_id;
      publication.generation = CapabilityGeneration(counter);
      for (const CapabilityEvidence& existing : backend.capabilities.entries()) {
        CapabilityEvidence claim = existing;
        claim.state = request.detail.empty() ? CapabilityState::REVOKED
                                             : capability_state_from(request.value);
        claim.generation = publication.generation;
        claim.backend_boot = backend.backend_boot;
        claim.observed_at_unix_millis = 1;
        claim.source = "reference-control-plane";
        publication.claims.push_back(std::move(claim));
      }
      if (publication.claims.empty()) {
        CapabilityEvidence claim;
        claim.key = CapabilityKey(std::string(capability_keys::cuda_execution));
        claim.state = CapabilityState::REVOKED;
        claim.generation = publication.generation;
        claim.profile_id = publication.profile_id;
        claim.backend_boot = backend.backend_boot;
        claim.observed_at_unix_millis = 1;
        claim.source = "reference-control-plane";
        publication.claims.push_back(std::move(claim));
      }
      apply(router_->publish_capabilities(std::move(publication)));
      return response;
    }
    case distributed::AdminOp::SET_READINESS: {
      BackendDescriptor backend;
      if (!router_->find_backend(request.backend_id, &backend)) {
        response.code = OutcomeCode::REJECT_STALE_BACKEND;
        response.detail = "backend is not registered";
        return response;
      }
      std::uint64_t& counter = evidence_generations_[request.backend_id];
      counter = std::max(counter, backend.readiness.generation.value()) + 1;
      ReadinessEvidence evidence;
      evidence.backend_id = backend.backend_id;
      evidence.backend_boot = backend.backend_boot;
      evidence.generation = ReadinessGeneration(counter);
      evidence.state = request.value == 0 ? ReadinessState::NOT_READY : ReadinessState::READY;
      evidence.observed_at_unix_millis = 1;
      evidence.source = "reference-control-plane";
      apply(router_->update_readiness(std::move(evidence)));
      return response;
    }
    case distributed::AdminOp::SET_AVAILABILITY: {
      BackendDescriptor backend;
      if (!router_->find_backend(request.backend_id, &backend)) {
        response.code = OutcomeCode::REJECT_STALE_BACKEND;
        response.detail = "backend is not registered";
        return response;
      }
      std::uint64_t& counter = evidence_generations_[request.backend_id];
      counter = std::max(counter, backend.availability.generation.value()) + 1;
      AvailabilityEvidence evidence;
      evidence.backend_id = backend.backend_id;
      evidence.backend_boot = backend.backend_boot;
      evidence.generation = AvailabilityGeneration(counter);
      evidence.state = request.value == 0 ? AvailabilityState::UNAVAILABLE
                                          : AvailabilityState::AVAILABLE;
      evidence.observed_at_unix_millis = 1;
      evidence.source = "reference-control-plane";
      apply(router_->update_availability(std::move(evidence)));
      return response;
    }
    case distributed::AdminOp::SAVE: {
      const PersistenceResult saved = router_->save(request.detail);
      response.code = saved.code;
      response.detail = saved.detail;
      return response;
    }
    case distributed::AdminOp::LOAD: {
      const PersistenceResult loaded = router_->load(request.detail);
      response.code = loaded.code;
      response.detail = loaded.detail;
      return response;
    }
    case distributed::AdminOp::SHUTDOWN:
      response.detail = "shutdown requested";
      stopping_.store(true);
      listener_.stop();
      return response;
    default:
      response.code = OutcomeCode::REJECT_INVALID;
      response.detail = "unsupported admin operation";
      return response;
  }
}

DispatchRecord Coordinator::Dispatcher::handoff(const RouteDecision& decision,
                                                const RoutePlan& plan,
                                                const DiscoveryContext& context) {
  (void)context;
  DispatchRecord record;
  record.decision_id = decision.decision_id;
  record.decision_generation = decision.decision_generation;
  record.target = plan.primary;

  std::shared_ptr<BackendLink> link;
  {
    std::lock_guard<std::mutex> lock(coordinator_->mutex_);
    const auto iter = coordinator_->links_.find(plan.primary.backend_id);
    if (iter != coordinator_->links_.end()) {
      link = iter->second;
    }
  }
  if (link == nullptr) {
    record.code = OutcomeCode::REJECT_STALE_BACKEND;
    record.failure = FailureClass::BACKEND_UNAVAILABLE;
    record.detail = "no live backend worker connection for the target backend";
    return record;
  }
  if (!(link->backend_boot == plan.primary.backend_boot)) {
    record.code = OutcomeCode::REJECT_STALE_BACKEND_BOOT;
    record.failure = FailureClass::BACKEND_RESTARTED;
    record.detail = "live connection belongs to a different backend incarnation";
    return record;
  }

  distributed::DispatchMessage message;
  message.dispatch_id = allocate_id<DispatchTag>();
  message.dispatch_generation = Generation<DispatchTag>(1);
  message.decision_id = decision.decision_id;
  message.decision_generation = decision.decision_generation;
  message.target = plan.primary;
  message.attempt = 1;

  std::vector<std::uint8_t> payload;
  std::string error;
  if (!distributed::encode_dispatch(message, coordinator_->options_.limits, &payload, &error)) {
    record.code = OutcomeCode::REJECT_INVALID;
    record.failure = FailureClass::UNKNOWN;
    record.detail = error;
    return record;
  }
  Frame frame = make_frame(MessageType::DISPATCH, std::move(payload));
  if (!link->connection->send(frame, &error)) {
    record.code = OutcomeCode::REJECT_UNAVAILABLE;
    record.failure = FailureClass::BACKEND_UNAVAILABLE;
    record.detail = error;
    return record;
  }

  std::unique_lock<std::mutex> lock(link->mutex);
  link->condition.wait(lock, [&link, &message] {
    return link->closed || link->results.find(message.dispatch_id) != link->results.end();
  });
  const auto iter = link->results.find(message.dispatch_id);
  if (iter == link->results.end()) {
    record.code = OutcomeCode::REJECT_UNAVAILABLE;
    record.failure = FailureClass::BACKEND_UNAVAILABLE;
    record.detail = "backend connection closed before the dispatch result arrived";
    return record;
  }
  const distributed::DispatchResultMessage result = iter->second;
  link->results.erase(iter);
  lock.unlock();

  record.dispatch_id = message.dispatch_id;
  record.dispatch_generation = message.dispatch_generation;
  record.code = result.code;
  record.failure = result.failure;
  record.handed_off = result.code == OutcomeCode::DISPATCHED;
  record.dispatched_at_unix_millis = 1;
  record.detail = result.detail;
  return record;
}

}  // namespace model_router::reference
