// Model Router - deterministic test fixture. Test support only.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_TESTS_SUPPORT_ROUTER_FIXTURE_HPP
#define MODEL_ROUTER_TESTS_SUPPORT_ROUTER_FIXTURE_HPP

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "model_router/model_router.hpp"
#include "test_framework.hpp"

namespace mrtest {

/// Failure messages use the canonical outcome name instead of a number.
[[nodiscard]] inline std::string describe(model_router::OutcomeCode code) {
  return std::string(model_router::to_string(code));
}

/// Records every dispatch handoff. It performs no inference and no network I/O:
/// it exists so the dispatch boundary can be exercised deterministically.
class RecordingDispatcher final : public model_router::DispatchHandler {
 public:
  [[nodiscard]] std::string name() const override { return "recording-dispatcher"; }

  [[nodiscard]] model_router::DispatchRecord handoff(
      const model_router::RouteDecision& decision, const model_router::RoutePlan& plan,
      const model_router::DiscoveryContext& context) override {
    (void)context;
    ++calls;
    last_decision = decision.decision_id;
    last_target = plan.primary;
    model_router::DispatchRecord record;
    record.decision_id = decision.decision_id;
    record.decision_generation = decision.decision_generation;
    record.target = plan.primary;
    record.code = fail ? model_router::OutcomeCode::FAILED
                       : model_router::OutcomeCode::DISPATCHED;
    record.handed_off = !fail;
    record.dispatched_at_unix_millis = 1;
    record.detail = fail ? "recorded failure" : "recorded handoff";
    return record;
  }

  std::uint64_t calls{0};
  bool fail{false};
  model_router::RouteDecisionId last_decision{};
  model_router::CandidateKey last_target{};
};

/// Tracks per-backend evidence generations so each publication advances
/// monotonically, exactly as a real backend must.
class EvidencePublisher {
 public:
  void reset() { generations_.clear(); }

  [[nodiscard]] model_router::MutationResult health(model_router::ModelRouter& router,
                                                    model_router::BackendId backend_id,
                                                    model_router::BackendBootId boot,
                                                    model_router::HealthState state,
                                                    model_router::UnixMillis now,
                                                    model_router::UnixMillis expires = 0) {
    model_router::HealthEvidence evidence;
    evidence.backend_id = backend_id;
    evidence.backend_boot = boot;
    evidence.generation = next<model_router::HealthTag>(backend_id);
    evidence.state = state;
    evidence.observed_at_unix_millis = now;
    evidence.expires_at_unix_millis = expires;
    evidence.source = "test-fixture";
    return router.update_health(std::move(evidence));
  }

  [[nodiscard]] model_router::MutationResult availability(
      model_router::ModelRouter& router, model_router::BackendId backend_id,
      model_router::BackendBootId boot, model_router::AvailabilityState state,
      model_router::UnixMillis now, model_router::UnixMillis expires = 0) {
    model_router::AvailabilityEvidence evidence;
    evidence.backend_id = backend_id;
    evidence.backend_boot = boot;
    evidence.generation = next<model_router::AvailabilityTag>(backend_id);
    evidence.state = state;
    evidence.observed_at_unix_millis = now;
    evidence.expires_at_unix_millis = expires;
    evidence.source = "test-fixture";
    return router.update_availability(std::move(evidence));
  }

  [[nodiscard]] model_router::MutationResult readiness(
      model_router::ModelRouter& router, model_router::BackendId backend_id,
      model_router::BackendBootId boot, model_router::ReadinessState state,
      model_router::UnixMillis now, model_router::UnixMillis expires = 0) {
    model_router::ReadinessEvidence evidence;
    evidence.backend_id = backend_id;
    evidence.backend_boot = boot;
    evidence.generation = next<model_router::ReadinessTag>(backend_id);
    evidence.state = state;
    evidence.observed_at_unix_millis = now;
    evidence.expires_at_unix_millis = expires;
    evidence.source = "test-fixture";
    return router.update_readiness(std::move(evidence));
  }

  [[nodiscard]] model_router::MutationResult residency(
      model_router::ModelRouter& router, model_router::BackendId backend_id,
      model_router::BackendBootId boot, model_router::ResidencyState state,
      model_router::UnixMillis now, model_router::UnixMillis expires = 0) {
    model_router::ResidencyEvidence evidence;
    evidence.backend_id = backend_id;
    evidence.backend_boot = boot;
    evidence.generation = next<model_router::ResidencyTag>(backend_id);
    evidence.state = state;
    evidence.observed_at_unix_millis = now;
    evidence.expires_at_unix_millis = expires;
    evidence.source = "test-fixture";
    return router.update_residency(std::move(evidence));
  }

  [[nodiscard]] model_router::MutationResult capacity(
      model_router::ModelRouter& router, model_router::BackendId backend_id,
      model_router::BackendBootId boot, model_router::CapacityState state,
      std::uint32_t available_slots, std::uint32_t total_slots, model_router::UnixMillis now) {
    model_router::CapacityEvidence evidence;
    evidence.backend_id = backend_id;
    evidence.backend_boot = boot;
    evidence.generation = next<model_router::CapacityTag>(backend_id);
    evidence.state = state;
    evidence.observed_at_unix_millis = now;
    evidence.source = "test-fixture";
    model_router::CapacityDetail detail;
    detail.backend_id = backend_id;
    detail.backend_boot = boot;
    detail.available_slots = available_slots;
    detail.total_slots = total_slots;
    detail.observed_at_unix_millis = now;
    return router.update_capacity(std::move(evidence), std::move(detail));
  }

  [[nodiscard]] model_router::MutationResult latency(model_router::ModelRouter& router,
                                                     model_router::BackendId backend_id,
                                                     model_router::BackendBootId boot,
                                                     std::uint32_t dispatch_micros,
                                                     std::uint32_t queue_micros,
                                                     model_router::UnixMillis now) {
    model_router::LatencyEvidence evidence;
    evidence.backend_id = backend_id;
    evidence.backend_boot = boot;
    evidence.dispatch_micros = dispatch_micros;
    evidence.queue_micros = queue_micros;
    evidence.tail_micros = dispatch_micros;
    evidence.sample_count = 16;
    evidence.observed_at_unix_millis = now;
    evidence.source = "test-fixture";
    return router.update_latency(std::move(evidence));
  }

 private:
  /// Advances the shared per-backend evidence counter. Real backends advance
  /// their evidence generations monotonically too; a shared counter keeps the
  /// fixture simple without weakening the monotonicity requirement.
  template <class Tag>
  [[nodiscard]] model_router::Generation<Tag> next(model_router::BackendId backend_id) {
    std::uint64_t& value = generations_[backend_id];
    value = value + 1;
    return model_router::Generation<Tag>(value);
  }

  std::map<model_router::BackendId, std::uint64_t> generations_;
};

/// A deterministic in-process router with the SYNTHETIC reference catalog.
struct Fixture {
  std::shared_ptr<model_router::LogicalClock> clock =
      std::make_shared<model_router::LogicalClock>();
  std::shared_ptr<RecordingDispatcher> dispatcher = std::make_shared<RecordingDispatcher>();
  model_router::reference::Catalog catalog = model_router::reference::make_catalog();
  EvidencePublisher publisher;
  std::unique_ptr<model_router::ModelRouter> router;

  [[nodiscard]] model_router::UnixMillis now() const { return clock->now_unix_millis(); }

  void start(std::uint32_t max_route_history = 0) {
    model_router::ModelRouterOptions options;
    options.clock = clock;
    options.providers.dispatcher = dispatcher;
    if (max_route_history != 0) {
      options.limits.max_route_history = max_route_history;
    }
    router = std::make_unique<model_router::ModelRouter>(std::move(options));
    const model_router::MutationResult started = router->start();
    MR_CHECK(started.accepted());
    register_catalog();
  }

  void register_catalog() {
    for (const model_router::ModelDescriptor& model :
         model_router::reference::make_models(catalog, now())) {
      const model_router::MutationResult result = router->register_model(model);
      MR_CHECK(result.accepted());
    }
    const model_router::MutationResult provider = router->register_provider([] {
      model_router::ProviderDescriptor descriptor;
      descriptor.provider_id = model_router::reference::make_catalog().provider;
      descriptor.generation = model_router::ProviderGeneration(1);
      descriptor.display_name = "reference-provider";
      descriptor.default_trust_domain = model_router::TrustDomain::LOCAL;
      descriptor.third_party = false;
      return descriptor;
    }());
    MR_CHECK(provider.accepted());
  }

  /// Registers one reference backend incarnation and publishes fully current
  /// evidence for it.
  void add_backend(model_router::BackendId backend_id, std::uint64_t boot,
                   model_router::BackendGeneration generation = model_router::BackendGeneration(1),
                   std::uint64_t registration = 1) {
    model_router::BackendDescriptor backend = model_router::reference::make_backend(
        catalog, backend_id, generation, model_router::BackendBootId(boot),
        model_router::BackendRegistrationGeneration(registration),
        "127.0.0.1:" + std::to_string(7000 + backend_id.value()), model_router::Provenance::SYNTHETIC,
        now());
    const model_router::MutationResult registered = router->register_backend(std::move(backend));
    MR_CHECK(registered.accepted());
    publish_current(backend_id, model_router::BackendBootId(boot));
  }

  void publish_current(model_router::BackendId backend_id, model_router::BackendBootId boot) {
    const model_router::UnixMillis current = now();
    MR_CHECK(publisher.health(*router, backend_id, boot, model_router::HealthState::HEALTHY, current)
                 .accepted());
    MR_CHECK(publisher
                 .availability(*router, backend_id, boot,
                               model_router::AvailabilityState::AVAILABLE, current)
                 .accepted());
    MR_CHECK(publisher
                 .readiness(*router, backend_id, boot, model_router::ReadinessState::READY, current)
                 .accepted());
    MR_CHECK(publisher
                 .residency(*router, backend_id, boot, model_router::ResidencyState::RESIDENT, current)
                 .accepted());
    MR_CHECK(publisher
                 .capacity(*router, backend_id, boot, model_router::CapacityState::AVAILABLE, 8, 16,
                           current)
                 .accepted());
    MR_CHECK(publisher.latency(*router, backend_id, boot, 1000, 100, current).accepted());
  }

  /// Publishes the reference cost table for one backend incarnation.
  void publish_costs(model_router::BackendId backend_id, model_router::PriceGeneration generation,
                     std::int64_t small_total, std::int64_t general_total,
                     std::int64_t specialist_total) {
    const model_router::UnixMillis current = now();
    MR_CHECK(router
                 ->set_cost(model_router::reference::make_cost_evidence(
                     backend_id, catalog.small_model, generation, small_total, small_total,
                     small_total, current))
                 .accepted());
    MR_CHECK(router
                 ->set_cost(model_router::reference::make_cost_evidence(
                     backend_id, catalog.general_model, generation, general_total, general_total,
                     general_total, current))
                 .accepted());
    MR_CHECK(router
                 ->set_cost(model_router::reference::make_cost_evidence(
                     backend_id, catalog.specialist_model, generation, specialist_total,
                     specialist_total, specialist_total, current))
                 .accepted());
  }
};

/// Builds a request with current tenancy and policy identity.
[[nodiscard]] inline model_router::RouteRequest make_request(const Fixture& fixture,
                                                            model_router::TenantId tenant,
                                                            model_router::PolicyId policy_id,
                                                            model_router::PolicyGeneration policy) {
  model_router::RouteRequest request;
  request.request_id = model_router::allocate_id<model_router::RouteRequestTag>();
  request.request_generation = model_router::RouteRequestGeneration(1);
  request.tenant = tenant;
  request.name_space = model_router::NamespaceId(1);
  request.policy_id = policy_id;
  request.policy_generation = policy;
  request.created_at_unix_millis = fixture.now();
  request.estimated_input_tokens = 1000;
  request.estimated_output_tokens = 500;
  request.requirements.required_input_modalities =
      model_router::ModalitySet(static_cast<std::uint32_t>(model_router::Modality::TEXT));
  request.requirements.required_output_modalities =
      model_router::ModalitySet(static_cast<std::uint32_t>(model_router::Modality::TEXT));
  return request;
}

/// Installs a permissive-but-explicit policy that allows every unlisted
/// identity and permits fallbacks for transient outcomes.
[[nodiscard]] inline model_router::MutationResult install_open_policy(
    model_router::ModelRouter& router, model_router::PolicyId policy_id,
    model_router::PolicyGeneration generation) {
  model_router::PolicySnapshot policy =
      model_router::PolicyBuilder(policy_id, generation)
          .fallback_policy(model_router::FallbackPolicy::PERMITTED)
          .max_fallback_depth(4)
          .permit_fallback_outcome(model_router::OutcomeCode::REVALIDATION_REQUIRED)
          .permit_fallback_outcome(model_router::OutcomeCode::REROUTE_REQUIRED)
          .permit_fallback_outcome(model_router::OutcomeCode::REJECT_STALE_BACKEND_BOOT)
          .permit_fallback_outcome(model_router::OutcomeCode::REJECT_STALE_HEALTH)
          .permit_fallback_outcome(model_router::OutcomeCode::REJECT_UNAVAILABLE)
          .permit_fallback_outcome(model_router::OutcomeCode::REJECT_NOT_READY)
          .permit_fallback_outcome(model_router::OutcomeCode::REJECT_UNHEALTHY)
          .build();
  return router.set_policy(std::move(policy));
}

}  // namespace mrtest

#endif  // MODEL_ROUTER_TESTS_SUPPORT_ROUTER_FIXTURE_HPP
