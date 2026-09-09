// Model Router example - policy-bound fallback after invalidation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// A route is built with an explicit ordered fallback set. The primary backend
// incarnation is then fenced, which permanently invalidates it, and a reroute is
// requested. The fallback is not reused from the old plan: it is freshly
// revalidated against current authority and evidence before it may be selected.
// The example never prompts and never waits.

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "model_router/model_router.hpp"

namespace {

using model_router::BackendBootId;
using model_router::BackendGeneration;
using model_router::BackendId;
using model_router::PolicyGeneration;
using model_router::PolicyId;
using model_router::TenantId;

constexpr TenantId kTenant(1);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);

struct ExampleError {
  std::string message;
};

void require(bool value, const char* what) {
  if (!value) {
    throw ExampleError{std::string(what)};
  }
}

/// Publishes fully current dynamic evidence for one backend incarnation.
class Publisher {
 public:
  [[nodiscard]] bool publish(model_router::ModelRouter& router, BackendId backend_id,
                             BackendBootId boot, model_router::UnixMillis now,
                             std::uint32_t dispatch_micros, std::uint32_t queue_micros) {
    model_router::HealthEvidence health;
    health.backend_id = backend_id;
    health.backend_boot = boot;
    health.generation = next<model_router::HealthTag>(backend_id);
    health.state = model_router::HealthState::HEALTHY;
    health.observed_at_unix_millis = now;
    health.source = "example-publisher";
    if (!router.update_health(std::move(health)).accepted()) {
      return false;
    }

    model_router::AvailabilityEvidence availability;
    availability.backend_id = backend_id;
    availability.backend_boot = boot;
    availability.generation = next<model_router::AvailabilityTag>(backend_id);
    availability.state = model_router::AvailabilityState::AVAILABLE;
    availability.observed_at_unix_millis = now;
    availability.source = "example-publisher";
    if (!router.update_availability(std::move(availability)).accepted()) {
      return false;
    }

    model_router::ReadinessEvidence readiness;
    readiness.backend_id = backend_id;
    readiness.backend_boot = boot;
    readiness.generation = next<model_router::ReadinessTag>(backend_id);
    readiness.state = model_router::ReadinessState::READY;
    readiness.observed_at_unix_millis = now;
    readiness.source = "example-publisher";
    if (!router.update_readiness(std::move(readiness)).accepted()) {
      return false;
    }

    model_router::ResidencyEvidence residency;
    residency.backend_id = backend_id;
    residency.backend_boot = boot;
    residency.generation = next<model_router::ResidencyTag>(backend_id);
    residency.state = model_router::ResidencyState::RESIDENT;
    residency.observed_at_unix_millis = now;
    residency.source = "example-publisher";
    if (!router.update_residency(std::move(residency)).accepted()) {
      return false;
    }

    model_router::CapacityEvidence capacity;
    capacity.backend_id = backend_id;
    capacity.backend_boot = boot;
    capacity.generation = next<model_router::CapacityTag>(backend_id);
    capacity.state = model_router::CapacityState::AVAILABLE;
    capacity.observed_at_unix_millis = now;
    capacity.source = "example-publisher";
    model_router::CapacityDetail detail;
    detail.backend_id = backend_id;
    detail.backend_boot = boot;
    detail.available_slots = 8;
    detail.total_slots = 16;
    detail.observed_at_unix_millis = now;
    if (!router.update_capacity(std::move(capacity), std::move(detail)).accepted()) {
      return false;
    }

    model_router::LatencyEvidence latency;
    latency.backend_id = backend_id;
    latency.backend_boot = boot;
    latency.dispatch_micros = dispatch_micros;
    latency.tail_micros = dispatch_micros;
    latency.queue_micros = queue_micros;
    latency.sample_count = 16;
    latency.observed_at_unix_millis = now;
    latency.source = "example-publisher";
    return router.update_latency(std::move(latency)).accepted();
  }

 private:
  template <class Tag>
  [[nodiscard]] model_router::Generation<Tag> next(BackendId backend_id) {
    std::uint64_t& value = generations_[backend_id];
    value = value + 1;
    return model_router::Generation<Tag>(value);
  }

  std::map<BackendId, std::uint64_t> generations_;
};

void register_catalog(model_router::ModelRouter& router,
                      const model_router::reference::Catalog& catalog,
                      model_router::UnixMillis now) {
  for (const model_router::ModelDescriptor& model :
       model_router::reference::make_models(catalog, now)) {
    require(router.register_model(model).accepted(), "register_model");
  }
  model_router::ProviderDescriptor provider;
  provider.provider_id = catalog.provider;
  provider.generation = model_router::ProviderGeneration(1);
  provider.display_name = "reference-provider";
  provider.default_trust_domain = model_router::TrustDomain::LOCAL;
  provider.third_party = false;
  require(router.register_provider(std::move(provider)).accepted(), "register_provider");
}

void register_backend(model_router::ModelRouter& router,
                      const model_router::reference::Catalog& catalog, BackendId backend_id,
                      model_router::UnixMillis now) {
  model_router::BackendDescriptor backend = model_router::reference::make_backend(
      catalog, backend_id, BackendGeneration(1), BackendBootId(1),
      model_router::BackendRegistrationGeneration(1),
      "127.0.0.1:" + std::to_string(7000 + backend_id.value()),
      model_router::Provenance::SYNTHETIC, now);
  require(router.register_backend(std::move(backend)).accepted(), "register_backend");
}

/// Publishes the reference price table for one backend incarnation.
void publish_costs(model_router::ModelRouter& router,
                   const model_router::reference::Catalog& catalog, BackendId backend_id,
                   model_router::PriceGeneration generation, std::int64_t small_total,
                   std::int64_t general_total, std::int64_t specialist_total,
                   model_router::UnixMillis now) {
  require(router
              .set_cost(model_router::reference::make_cost_evidence(
                  backend_id, catalog.small_model, generation, small_total, small_total,
                  small_total, now))
              .accepted(),
          "set_cost small");
  require(router
              .set_cost(model_router::reference::make_cost_evidence(
                  backend_id, catalog.general_model, generation, general_total, general_total,
                  general_total, now))
              .accepted(),
          "set_cost general");
  require(router
              .set_cost(model_router::reference::make_cost_evidence(
                  backend_id, catalog.specialist_model, generation, specialist_total,
                  specialist_total, specialist_total, now))
              .accepted(),
          "set_cost specialist");
}

[[nodiscard]] model_router::CapabilityRequirement require_capability(std::string key) {
  model_router::CapabilityRequirement requirement;
  requirement.key = model_router::CapabilityKey(std::move(key));
  requirement.minimum_state = model_router::CapabilityState::DECLARED;
  return requirement;
}

[[nodiscard]] model_router::RouteRequest make_request(model_router::UnixMillis now) {
  model_router::RouteRequest request;
  request.request_id = model_router::allocate_id<model_router::RouteRequestTag>();
  request.request_generation = model_router::RouteRequestGeneration(1);
  request.tenant = kTenant;
  request.name_space = model_router::NamespaceId(1);
  request.policy_id = kPolicy;
  request.policy_generation = kPolicyGeneration;
  request.created_at_unix_millis = now;
  request.estimated_input_tokens = 1000;
  request.estimated_output_tokens = 500;
  request.requirements.required_input_modalities = model_router::ModalitySet(
      static_cast<std::uint32_t>(model_router::Modality::TEXT));
  request.requirements.required_output_modalities = model_router::ModalitySet(
      static_cast<std::uint32_t>(model_router::Modality::TEXT));
  request.requirements.allow_fallbacks = true;
  request.requirements.retry_policy.max_fallbacks = 2;
  request.requirements.retry_policy.max_attempts = 2;
  return request;
}

int run() {
  const std::shared_ptr<model_router::LogicalClock> clock =
      std::make_shared<model_router::LogicalClock>();
  const model_router::reference::Catalog catalog = model_router::reference::make_catalog();

  model_router::ModelRouterOptions options;
  options.clock = clock;
  model_router::ModelRouter router(std::move(options));
  require(router.start().accepted(), "start");

  const model_router::UnixMillis now = clock->now_unix_millis();
  register_catalog(router, catalog, now);
  register_backend(router, catalog, BackendId(1), now);
  register_backend(router, catalog, BackendId(2), now);
  Publisher publisher;
  require(publisher.publish(router, BackendId(1), BackendBootId(1), now, 1000, 100),
          "publish evidence backend 1");
  require(publisher.publish(router, BackendId(2), BackendBootId(1), now, 1000, 100),
          "publish evidence backend 2");
  publish_costs(router, catalog, BackendId(1), model_router::PriceGeneration(1), 500, 700, 100,
                now);
  publish_costs(router, catalog, BackendId(2), model_router::PriceGeneration(1), 500, 700, 900,
                now);

  model_router::PolicySnapshot policy =
      model_router::PolicyBuilder(kPolicy, kPolicyGeneration)
          .fallback_policy(model_router::FallbackPolicy::PERMITTED)
          .max_fallback_depth(2)
          .permit_fallback_outcome(model_router::OutcomeCode::REVALIDATION_REQUIRED)
          .permit_fallback_outcome(model_router::OutcomeCode::REJECT_STALE_BACKEND_BOOT)
          .build();
  require(router.set_policy(std::move(policy)).accepted(), "set_policy");

  model_router::RouteRequest request = make_request(now);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  request.requirements.require_known_cost = true;

  const model_router::RouteOutcome primary = router.route(request);
  require(primary.code == model_router::OutcomeCode::ROUTED, "primary route must be routed");
  require(!primary.decision.fallbacks.empty(), "primary route must carry a fallback");

  // The primary backend incarnation is fenced: it can never become
  // authoritative again in this process or any other.
  const model_router::MutationResult fenced = router.fence_backend_boot(
      BackendId(1), BackendGeneration(1), BackendBootId(1), model_router::OutcomeCode::FENCED);
  require(fenced.accepted(), "fence_backend_boot");

  const model_router::RouteOutcome fallback = router.reroute(
      primary.decision.decision_id, model_router::FailureClass::TRANSIENT_BACKEND_FAILURE,
      request);
  require(fallback.code == model_router::OutcomeCode::ROUTED, "fallback must be routed");

  model_router::RouteDecision superseded;
  require(router.find_decision(primary.decision.decision_id, &superseded),
          "find_decision primary");

  std::cout << "fallback example: primary invalidated by fencing, fallback freshly revalidated\n";
  std::cout << "primary_decision_id=" << primary.decision.decision_id.value()
            << " primary_winner_model_id=" << primary.decision.authority.model_id.value()
            << " primary_winner_backend_id=" << primary.decision.authority.backend_id.value()
            << "\n";
  std::cout << "fallback_count=" << primary.decision.fallbacks.size() << "\n";
  for (const model_router::CandidateKey& key : primary.decision.fallbacks) {
    std::cout << "fallback_candidate model_id=" << key.model_id.value()
              << " backend_id=" << key.backend_id.value()
              << " backend_boot=" << key.backend_boot.value() << "\n";
  }
  std::cout << "fence_code=" << model_router::to_string(fenced.code)
            << " primary_status_after_reroute=" << model_router::to_string(superseded.status)
            << "\n";
  std::cout << "fallback_decision_id=" << fallback.decision.decision_id.value()
            << " fallback_winner_model_id=" << fallback.decision.authority.model_id.value()
            << " fallback_winner_backend_id=" << fallback.decision.authority.backend_id.value()
            << "\n";
  std::cout << "fallback_message=" << fallback.explanation.message() << "\n";
  return 0;
}

}  // namespace

int main() {
  try {
    return run();
  } catch (const ExampleError& error) {
    std::cerr << "fallback example failed: " << error.message << "\n";
    return 1;
  }
}
