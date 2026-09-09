// Model Router example - conservative recovery from durable state.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// A router with nontrivial state saves it to a state file. The router is
// destroyed, a fresh router loads the file, and the recovered view is inspected:
// dynamic evidence is invalidated, previously retained routes are not
// dispatchable, and only freshly published evidence makes routing possible
// again. The state file is removed at the end of main(). The example never
// prompts and never waits.

#include <cstdint>
#include <cstdio>
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

void install_policy(model_router::ModelRouter& router) {
  model_router::PolicySnapshot policy =
      model_router::PolicyBuilder(kPolicy, kPolicyGeneration)
          .fallback_policy(model_router::FallbackPolicy::PERMITTED)
          .max_fallback_depth(2)
          .build();
  require(router.set_policy(std::move(policy)).accepted(), "set_policy");
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
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  request.requirements.require_known_cost = true;
  return request;
}

int run() {
  const std::string state_path = "model_router_example_recovery.state";
  const model_router::reference::Catalog catalog = model_router::reference::make_catalog();

  std::uint64_t epoch_before = 0;
  std::uint64_t coordinator_before = 0;
  std::uint64_t boot_before = 0;
  std::uint64_t old_decision_id = 0;
  model_router::OutcomeCode save_code = model_router::OutcomeCode::INTERNAL_ERROR;

  {
    // --- phase 1: build nontrivial state and save it -----------------------
    const std::shared_ptr<model_router::LogicalClock> clock =
        std::make_shared<model_router::LogicalClock>();
    model_router::ModelRouterOptions options;
    options.clock = clock;
    model_router::ModelRouter router(std::move(options));
    require(router.start().accepted(), "start");

    const model_router::UnixMillis now = clock->now_unix_millis();
    register_catalog(router, catalog, now);
    register_backend(router, catalog, BackendId(1), now);
    register_backend(router, catalog, BackendId(2), now);
    register_backend(router, catalog, BackendId(3), now);
    Publisher publisher;
    require(publisher.publish(router, BackendId(1), BackendBootId(1), now, 1000, 100),
            "publish evidence backend 1");
    require(publisher.publish(router, BackendId(2), BackendBootId(1), now, 1000, 100),
            "publish evidence backend 2");
    require(router
                .fence_backend_boot(BackendId(3), BackendGeneration(1), BackendBootId(1),
                                    model_router::OutcomeCode::FENCED)
                .accepted(),
            "fence_backend_boot");
    publish_costs(router, catalog, BackendId(1), model_router::PriceGeneration(1), 500, 700, 100,
                  now);
    publish_costs(router, catalog, BackendId(2), model_router::PriceGeneration(1), 500, 700, 900,
                  now);
    install_policy(router);

    const model_router::RouteOutcome outcome = router.route(make_request(now));
    require(outcome.code == model_router::OutcomeCode::ROUTED, "route must be routed");
    old_decision_id = outcome.decision.decision_id.value();

    epoch_before = router.router_epoch().value();
    coordinator_before = router.coordinator_epoch().value();
    boot_before = router.router_boot().value();
    const model_router::PersistenceResult saved = router.save(state_path);
    if (!saved.ok()) {
      throw ExampleError{"save durable state: " +
                         std::string(model_router::to_string(saved.code)) + " " + saved.detail};
    }
    save_code = saved.code;
  }

  // --- phase 2: fresh router recovers from the state file ------------------
  const std::shared_ptr<model_router::LogicalClock> clock =
      std::make_shared<model_router::LogicalClock>();
  model_router::ModelRouterOptions options;
  options.clock = clock;
  model_router::ModelRouter router(std::move(options));

  const model_router::PersistenceResult loaded = router.load(state_path);
  if (!loaded.ok()) {
    throw ExampleError{"load durable state: " +
                       std::string(model_router::to_string(loaded.code)) + " " + loaded.detail};
  }
  require(router.start().accepted(), "restart");
  const model_router::UnixMillis now = clock->now_unix_millis();

  const model_router::RouterSnapshot recovered = router.snapshot();
  const model_router::DispatchRecord refused =
      router.dispatch(model_router::RouteDecisionId(old_decision_id), kTenant);

  // Fresh evidence and a fresh policy generation are the only legal basis for a
  // new route.
  Publisher publisher;
  require(publisher.publish(router, BackendId(1), BackendBootId(1), now, 1000, 100),
          "republish evidence backend 1");
  require(publisher.publish(router, BackendId(2), BackendBootId(1), now, 1000, 100),
          "republish evidence backend 2");
  publish_costs(router, catalog, BackendId(1), model_router::PriceGeneration(2), 5000, 5000,
                5000, now);
  install_policy(router);

  const model_router::RouteOutcome fresh = router.route(make_request(now));
  require(fresh.code == model_router::OutcomeCode::ROUTED, "fresh route must be routed");

  std::cout << "recovery example: durable state, restart, evidence invalidation, fresh evidence\n";
  std::cout << "state_file=" << state_path << "\n";
  std::cout << "save_code=" << model_router::to_string(save_code) << "\n";
  std::cout << "epoch_before_restart router_epoch=" << epoch_before
            << " coordinator_epoch=" << coordinator_before << " boot=" << boot_before << "\n";
  std::cout << "load_code=" << model_router::to_string(loaded.code)
            << " detail=" << loaded.detail << "\n";
  std::cout << "epoch_after_restart router_epoch=" << router.router_epoch().value()
            << " coordinator_epoch=" << router.coordinator_epoch().value()
            << " boot=" << router.router_boot().value()
            << " recovered=" << (router.summary().recovered ? 1 : 0) << "\n";
  for (const model_router::BackendSummary& backend : recovered.backends) {
    std::cout << "recovered_backend backend_id=" << backend.backend_id.value()
              << " health=" << model_router::to_string(backend.health)
              << " availability=" << model_router::to_string(backend.availability)
              << " readiness=" << model_router::to_string(backend.readiness)
              << " evidence_current=" << (backend.evidence_current ? 1 : 0) << "\n";
  }
  for (const model_router::RouteSummary& route : recovered.routes) {
    std::cout << "recovered_route decision_id=" << route.decision_id.value()
              << " status=" << model_router::to_string(route.status)
              << " currentness=" << model_router::to_string(route.currentness) << "\n";
  }
  std::cout << "dispatch_without_fresh_evidence code=" << model_router::to_string(refused.code)
            << " handed_off=" << (refused.handed_off ? 1 : 0) << "\n";
  std::cout << "republished_evidence backend_id=1 accepted=1\n";
  std::cout << "republished_evidence backend_id=2 accepted=1\n";
  std::cout << "republished_price backend_id=1 price_generation=2 specialist_total_micros=5000\n";
  std::cout << "fresh_route_code=" << model_router::to_string(fresh.code)
            << " fresh_winner_model_id=" << fresh.decision.authority.model_id.value()
            << " fresh_winner_backend_id=" << fresh.decision.authority.backend_id.value() << "\n";

  const int removed = std::remove(state_path.c_str());
  require(removed == 0, "remove the state file");
  std::cout << "state_file_removed=1\n";
  return 0;
}

}  // namespace

int main() {
  try {
    return run();
  } catch (const ExampleError& error) {
    std::cerr << "recovery example failed: " << error.message << "\n";
    std::remove("model_router_example_recovery.state");
    return 1;
  }
}
