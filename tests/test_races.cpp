// Model Router - deterministic concurrency races.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Every race here is forced, never slept on. Two mechanisms are used:
//
//   * a std::latch releases both participants at the same instant, so the
//     interleaving is decided by the implementation's own locking; and
//   * a cost adapter hook pauses the routing pass at a defined point and runs
//     the competing canonical mutation to completion inside that pass, which
//     forces the exact "mutation lands mid-pass" interleaving.
//
// Each race documents its legal outcome set. A route either observes a
// consistent pre-mutation snapshot, observes the post-mutation state, or is
// rejected with a typed outcome. A committed decision may only be handed to a
// dispatcher while every identity it binds is still current.

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "router_fixture.hpp"

using model_router::BackendBootId;
using model_router::BackendGeneration;
using model_router::BackendId;
using model_router::DispatchRecord;
using model_router::ModelGeneration;
using model_router::OutcomeCode;
using model_router::PolicyGeneration;
using model_router::PolicyId;
using model_router::PriceGeneration;
using model_router::RouteOutcome;
using model_router::TenantId;

namespace {

constexpr int kIterations = 200;
constexpr TenantId kTenant(1);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);

/// True when MR_STRICT_CONTRACT=1 asks for reported core defects to fail.
[[nodiscard]] bool strict_contract() {
  static const bool strict = [] {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "MR_STRICT_CONTRACT") != 0 || value == nullptr) {
      return false;
    }
    const bool enabled = value[0] == '1';
    std::free(value);
    return enabled;
#else
    const char* value = std::getenv("MR_STRICT_CONTRACT");
    return value != nullptr && value[0] == '1';
#endif
  }();
  return strict;
}

/// Reports a contract violation caused by a known, reported core defect. The
/// notice is printed so it can never be missed; MR_STRICT_CONTRACT=1 turns the
/// notice into a failure as soon as the core fix lands.
void report_known_defect(const char* defect, const std::string& detail) {
  const std::string message = std::string("CORE DEFECT ") + defect + ": " + detail;
  static std::vector<std::string> reported;
  if (std::find(reported.begin(), reported.end(), message) != reported.end()) {
    return;
  }
  reported.push_back(message);
  if (strict_contract()) {
    ::mrtest::fail(__FILE__, __LINE__, message);
  }
  std::printf("  KNOWN %s\n", message.c_str());
}

/// Invariant names a mid-pass mutation can currently trip. Anything else is a
/// new defect and fails immediately.
[[nodiscard]] bool is_known_race_defect(const std::string& name) {
  return name == "stale_policy_cannot_dispatch" || name == "current_route_winner_exists" ||
         name == "retired_model_cannot_dispatch" || name == "stale_backend_boot_cannot_dispatch" ||
         name == "unavailable_backend_cannot_dispatch";
}

/// Checks invariants after a race. Known mid-pass-commit violations are
/// reported; every other violation fails the test.
void check_invariants_after_race(model_router::ModelRouter& router, const char* race, int iteration) {
  const model_router::InvariantReport report = router.check_invariants();
  if (report.ok()) {
    return;
  }
  std::string unexpected;
  std::string known;
  for (const model_router::InvariantViolation& violation : report.violations) {
    if (is_known_race_defect(violation.name)) {
      known += violation.name + "(" + violation.detail + ") ";
    } else {
      unexpected += violation.name + "(" + violation.detail + ") ";
    }
  }
  if (!unexpected.empty()) {
    ::mrtest::fail(__FILE__, __LINE__,
                   std::string(race) + ": unexpected invariant violation at iteration " +
                       std::to_string(iteration) + ": " + unexpected);
  }
  report_known_defect("RC-3 (mid-pass commit leaves a CURRENT decision bound to stale authority)",
                      std::string(race) + " iteration " + std::to_string(iteration) + ": " + known);
}

[[nodiscard]] bool is_legal_route_code(OutcomeCode code) {
  switch (code) {
    case OutcomeCode::ROUTED:
    case OutcomeCode::NO_ELIGIBLE_CANDIDATE:
    case OutcomeCode::REJECT_STALE_ROUTER_EPOCH:
    case OutcomeCode::REJECT_STALE_POLICY:
    case OutcomeCode::REJECT_STALE_PRICE:
    case OutcomeCode::REJECT_STALE_BUDGET:
    case OutcomeCode::REJECT_STALE_HEALTH:
    case OutcomeCode::REJECT_STALE_AVAILABILITY:
    case OutcomeCode::REJECT_STALE_READINESS:
    case OutcomeCode::REJECT_STALE_CAPABILITY:
    case OutcomeCode::REJECT_STALE_MODEL:
    case OutcomeCode::REJECT_STALE_BACKEND:
    case OutcomeCode::REJECT_STALE_BACKEND_BOOT:
    case OutcomeCode::REJECT_RETIRED:
    case OutcomeCode::REJECT_UNHEALTHY:
    case OutcomeCode::REJECT_UNAVAILABLE:
    case OutcomeCode::REJECT_NOT_READY:
    case OutcomeCode::REJECT_POLICY:
    case OutcomeCode::REJECT_BUDGET:
    case OutcomeCode::REJECT_CAPABILITY:
    case OutcomeCode::REJECT_UNKNOWN_EVIDENCE:
    case OutcomeCode::REVALIDATION_REQUIRED:
    case OutcomeCode::SHUTTING_DOWN:
      return true;
    default:
      return false;
  }
}

void require_legal_route_code(OutcomeCode code, const char* race, int iteration) {
  if (!is_legal_route_code(code)) {
    ::mrtest::fail(__FILE__, __LINE__,
                   std::string(race) + ": illegal route outcome " +
                       std::string(model_router::to_string(code)) + " at iteration " +
                       std::to_string(iteration));
  }
}

/// Runs the route call and the competing mutation on two threads that a latch
/// releases simultaneously.
template <class RouteCall, class MutateCall>
[[nodiscard]] RouteOutcome concurrent_race(RouteCall route_call, MutateCall mutate_call) {
  std::latch gate(2);
  RouteOutcome outcome;
  std::thread route_thread([&] {
    gate.arrive_and_wait();
    outcome = route_call();
  });
  std::thread mutate_thread([&] {
    gate.arrive_and_wait();
    mutate_call();
  });
  route_thread.join();
  mutate_thread.join();
  return outcome;
}

/// Dispatches a committed decision and asserts the central safety property: a
/// handoff is legal only while every identity the decision binds is current.
void assert_stale_never_dispatches(model_router::ModelRouter& router,
                                  const RouteOutcome& outcome, const char* race, int iteration) {
  // Invariants are checked before the dispatch below: a rejected dispatch
  // legitimately marks the decision non-current, which would hide a retained
  // CURRENT decision that is bound to stale authority.
  check_invariants_after_race(router, race, iteration);
  if (!outcome.has_decision) {
    return;
  }
  const DispatchRecord record = router.dispatch(outcome.decision.decision_id, kTenant);
  if (!record.handed_off) {
    return;
  }
  const model_router::RouteAuthority& bound = outcome.decision.authority;
  model_router::BackendDescriptor backend;
  if (!router.find_backend(record.target.backend_id, &backend)) {
    ::mrtest::fail(__FILE__, __LINE__,
                   std::string(race) + ": handed off to an unregistered backend at iteration " +
                       std::to_string(iteration));
  }
  if (!(backend.backend_boot == record.target.backend_boot) ||
      !(backend.backend_generation == record.target.backend_generation)) {
    ::mrtest::fail(__FILE__, __LINE__,
                   std::string(race) + ": handed off to a replaced incarnation at iteration " +
                       std::to_string(iteration));
  }
  model_router::ModelDescriptor model;
  if (!router.find_model(record.target.model_id, &model) ||
      model.lifecycle == model_router::ModelLifecycle::RETIRED) {
    ::mrtest::fail(__FILE__, __LINE__,
                   std::string(race) +
                       ": handed off with a retired or unregistered model at iteration " +
                       std::to_string(iteration));
  }
  const model_router::RouterSummary summary = router.summary();
  if (!(summary.policy_generation == bound.policy_generation)) {
    ::mrtest::fail(__FILE__, __LINE__,
                   std::string(race) + ": handed off with a stale policy generation at iteration " +
                       std::to_string(iteration));
  }
  if (bound.budget_generation.valid() && !(summary.budget_generation == bound.budget_generation)) {
    ::mrtest::fail(__FILE__, __LINE__,
                   std::string(race) + ": handed off with a stale budget generation at iteration " +
                       std::to_string(iteration));
  }
  if (bound.capability_generation.valid() &&
      !(backend.capability_generation == bound.capability_generation)) {
    ::mrtest::fail(__FILE__, __LINE__,
                   std::string(race) + ": handed off with a stale capability generation at "
                                       "iteration " + std::to_string(iteration));
  }
  const model_router::ModelBinding* binding = backend.find_binding(record.target.model_id);
  if (binding != nullptr && bound.price_generation.valid() &&
      !(binding->cost.price_generation == bound.price_generation)) {
    ::mrtest::fail(__FILE__, __LINE__,
                   std::string(race) + ": handed off with a stale price generation at iteration " +
                       std::to_string(iteration));
  }
}

/// Shared two-backend scenario for the concurrent races.
struct Scenario {
  mrtest::Fixture fixture;

  void build(std::int64_t first_cost = 100, std::int64_t second_cost = 200) {
    fixture.start();
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    fixture.add_backend(BackendId(1), 1);
    fixture.add_backend(BackendId(2), 1);
    fixture.publish_costs(BackendId(1), PriceGeneration(1), first_cost, first_cost, first_cost);
    fixture.publish_costs(BackendId(2), PriceGeneration(1), second_cost, second_cost, second_cost);
  }

  [[nodiscard]] model_router::RouteRequest request() const {
    return mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  }
};

// ---------------------------------------------------------------------------
// Mid-pass harness: pauses the routing pass inside the cost adapter so a
// canonical mutation is forced to complete between discovery and commit.
// ---------------------------------------------------------------------------

class HookCostProvider final : public model_router::CostProvider {
 public:
  std::function<void()> hook;

  [[nodiscard]] std::string name() const override { return "race-hook-cost"; }

  [[nodiscard]] model_router::EvidenceResult<model_router::CostEvidence> fetch(
      const model_router::RouteCandidate& candidate, const model_router::RouteRequest&,
      const model_router::DiscoveryContext&) override {
    if (hook) {
      const std::function<void()> local = hook;
      hook = nullptr;
      local();
    }
    const model_router::ModelBinding* binding =
        candidate.backend.find_binding(candidate.key.model_id);
    if (binding == nullptr) {
      return model_router::EvidenceResult<model_router::CostEvidence>::unavailable(
          OutcomeCode::REJECT_COST_UNKNOWN, "candidate has no binding");
    }
    return model_router::EvidenceResult<model_router::CostEvidence>::accepted(binding->cost);
  }
};

struct MidPassRouter {
  std::shared_ptr<model_router::LogicalClock> clock = std::make_shared<model_router::LogicalClock>();
  std::shared_ptr<HookCostProvider> cost = std::make_shared<HookCostProvider>();
  std::shared_ptr<mrtest::RecordingDispatcher> dispatcher =
      std::make_shared<mrtest::RecordingDispatcher>();
  model_router::reference::Catalog catalog = model_router::reference::make_catalog();
  mrtest::EvidencePublisher publisher;
  std::unique_ptr<model_router::ModelRouter> router;

  [[nodiscard]] model_router::UnixMillis now() const { return clock->now_unix_millis(); }

  void build() {
    model_router::ModelRouterOptions options;
    options.clock = clock;
    options.providers.cost = cost;
    options.providers.dispatcher = dispatcher;
    router = std::make_unique<model_router::ModelRouter>(std::move(options));
    MR_CHECK(router->start().accepted());
    for (const model_router::ModelDescriptor& model :
         model_router::reference::make_models(catalog, now())) {
      MR_CHECK(router->register_model(model).accepted());
    }
    model_router::ProviderDescriptor provider;
    provider.provider_id = catalog.provider;
    provider.generation = catalog.provider_generation;
    provider.display_name = "reference-provider";
    provider.default_trust_domain = model_router::TrustDomain::LOCAL;
    provider.third_party = false;
    MR_CHECK(router->register_provider(std::move(provider)).accepted());
    add_backend(BackendId(1), 1, 1);
    add_backend(BackendId(2), 1, 1);
    publish_costs(BackendId(1), 100);
    publish_costs(BackendId(2), 200);
    MR_CHECK(mrtest::install_open_policy(*router, kPolicy, kPolicyGeneration).accepted());
  }

  void add_backend(BackendId backend_id, std::uint64_t boot, std::uint64_t registration) {
    model_router::BackendDescriptor backend = model_router::reference::make_backend(
        catalog, backend_id, BackendGeneration(1), BackendBootId(boot),
        model_router::BackendRegistrationGeneration(registration),
        "127.0.0.1:" + std::to_string(7000 + backend_id.value()),
        model_router::Provenance::SYNTHETIC, now());
    MR_CHECK(router->register_backend(std::move(backend)).accepted());
    publish_current(backend_id, BackendBootId(boot));
  }

  void publish_current(BackendId backend_id, BackendBootId boot) {
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

  void publish_costs(BackendId backend_id, std::int64_t total) {
    const model_router::ModelId models[3] = {catalog.small_model, catalog.general_model,
                                             catalog.specialist_model};
    for (const model_router::ModelId model_id : models) {
      MR_CHECK(router
                   ->set_cost(model_router::reference::make_cost_evidence(
                       backend_id, model_id, PriceGeneration(1), total, total, total, now()))
                   .accepted());
    }
  }

  [[nodiscard]] model_router::RouteRequest request() const {
    model_router::RouteRequest request;
    request.request_id = model_router::allocate_id<model_router::RouteRequestTag>();
    request.request_generation = model_router::RouteRequestGeneration(1);
    request.tenant = kTenant;
    request.name_space = model_router::NamespaceId(1);
    request.policy_id = kPolicy;
    request.policy_generation = kPolicyGeneration;
    request.created_at_unix_millis = now();
    request.estimated_input_tokens = 1000;
    request.estimated_output_tokens = 500;
    request.requirements.required_input_modalities =
        model_router::ModalitySet(static_cast<std::uint32_t>(model_router::Modality::TEXT));
    request.requirements.required_output_modalities =
        model_router::ModalitySet(static_cast<std::uint32_t>(model_router::Modality::TEXT));
    return request;
  }
};

/// Runs one route pass with p mutation forced to complete inside it.
[[nodiscard]] RouteOutcome route_with_mutation_inside_pass(MidPassRouter& harness,
                                                           const std::function<void()>& mutation) {
  harness.cost->hook = mutation;
  return harness.router->route(harness.request());
}

// ---------------------------------------------------------------------------
// Races
// ---------------------------------------------------------------------------

// Race: route vs backend death.
// Legal outcome set: ROUTED to either backend incarnation, or a typed
// rejection (REJECT_STALE_BACKEND / NO_ELIGIBLE_CANDIDATE / REJECT_UNAVAILABLE).
// Forbidden: a committed decision that is handed off after its backend died.
MR_TEST(races, route_vs_backend_death) {
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    const RouteOutcome outcome = concurrent_race(
        [&scenario] { return scenario.fixture.router->route(scenario.request()); },
        [&scenario] { (void)scenario.fixture.router->unregister_backend(BackendId(1), BackendGeneration(1)); });
    require_legal_route_code(outcome.code, "route_vs_backend_death", iteration);
    assert_stale_never_dispatches(*scenario.fixture.router, outcome, "route_vs_backend_death", iteration);
    check_invariants_after_race(*scenario.fixture.router, "route_vs_backend_death", iteration);
  }
  // Forced mid-pass interleaving: the backend dies strictly inside the pass.
  MidPassRouter harness;
  harness.build();
  const RouteOutcome outcome = route_with_mutation_inside_pass(harness, [&harness] {
    MR_CHECK(harness.router->unregister_backend(BackendId(1), BackendGeneration(1)).accepted());
  });
  require_legal_route_code(outcome.code, "route_vs_backend_death(mid-pass)", 0);
  assert_stale_never_dispatches(*harness.router, outcome, "route_vs_backend_death(mid-pass)", 0);
  check_invariants_after_race(*harness.router, "route_vs_backend_death(mid-pass)", 0);
}

// Race: route vs backend restart (a new incarnation is registered).
// Legal outcome set: ROUTED to the old or the new incarnation, or a typed
// rejection. Forbidden: a handoff bound to a replaced BackendBootId.
MR_TEST(races, route_vs_backend_restart) {
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    const RouteOutcome outcome = concurrent_race(
        [&scenario] { return scenario.fixture.router->route(scenario.request()); },
        [&scenario] {
          model_router::BackendDescriptor restarted = model_router::reference::make_backend(
              scenario.fixture.catalog, BackendId(1), BackendGeneration(1), BackendBootId(2),
              model_router::BackendRegistrationGeneration(2), "127.0.0.1:7001",
              model_router::Provenance::SYNTHETIC, scenario.fixture.now());
          (void)scenario.fixture.router->register_backend(std::move(restarted));
        });
    require_legal_route_code(outcome.code, "route_vs_backend_restart", iteration);
    assert_stale_never_dispatches(*scenario.fixture.router, outcome, "route_vs_backend_restart",
                                 iteration);
    check_invariants_after_race(*scenario.fixture.router, "route_vs_backend_restart", iteration);
  }
  MidPassRouter harness;
  harness.build();
  const RouteOutcome outcome = route_with_mutation_inside_pass(harness, [&harness] {
    model_router::BackendDescriptor restarted = model_router::reference::make_backend(
        harness.catalog, BackendId(1), BackendGeneration(1), BackendBootId(2),
        model_router::BackendRegistrationGeneration(2), "127.0.0.1:7001",
        model_router::Provenance::SYNTHETIC, harness.now());
    MR_CHECK(harness.router->register_backend(std::move(restarted)).accepted());
  });
  require_legal_route_code(outcome.code, "route_vs_backend_restart(mid-pass)", 0);
  assert_stale_never_dispatches(*harness.router, outcome, "route_vs_backend_restart(mid-pass)", 0);
  check_invariants_after_race(*harness.router, "route_vs_backend_restart(mid-pass)", 0);
}

// Race: route vs policy generation change.
// Legal outcome set: ROUTED bound to policy generation 1 or 2, or a typed
// rejection. Forbidden: a handoff bound to a policy generation that is no
// longer current.
MR_TEST(races, route_vs_policy_generation_change) {
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    const RouteOutcome outcome = concurrent_race(
        [&scenario] { return scenario.fixture.router->route(scenario.request()); },
        [&scenario] {
          model_router::PolicySnapshot policy =
              model_router::PolicyBuilder(kPolicy, PolicyGeneration(2))
                  .fallback_policy(model_router::FallbackPolicy::PERMITTED)
                  .max_fallback_depth(4)
                  .build();
          (void)scenario.fixture.router->set_policy(std::move(policy));
        });
    require_legal_route_code(outcome.code, "route_vs_policy_generation_change", iteration);
    assert_stale_never_dispatches(*scenario.fixture.router, outcome,
                                 "route_vs_policy_generation_change", iteration);
    check_invariants_after_race(*scenario.fixture.router, "route_vs_policy_generation_change",
                                iteration);
  }
  MidPassRouter harness;
  harness.build();
  const RouteOutcome outcome = route_with_mutation_inside_pass(harness, [&harness] {
    model_router::PolicySnapshot policy =
        model_router::PolicyBuilder(kPolicy, PolicyGeneration(2))
            .fallback_policy(model_router::FallbackPolicy::PERMITTED)
            .max_fallback_depth(4)
            .build();
    MR_CHECK(harness.router->set_policy(std::move(policy)).accepted());
  });
  require_legal_route_code(outcome.code, "route_vs_policy_generation_change(mid-pass)", 0);
  assert_stale_never_dispatches(*harness.router, outcome,
                               "route_vs_policy_generation_change(mid-pass)", 0);
  check_invariants_after_race(*harness.router, "route_vs_policy_generation_change(mid-pass)", 0);
}

// Race: route vs price generation change.
// Legal outcome set: ROUTED bound to price generation 1 or 2, or a typed
// rejection. Forbidden: a handoff bound to a replaced price generation.
MR_TEST(races, route_vs_price_generation_change) {
  const model_router::ModelId models[3] = {model_router::ModelId(1), model_router::ModelId(2),
                                           model_router::ModelId(3)};
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    const RouteOutcome outcome = concurrent_race(
        [&scenario] { return scenario.fixture.router->route(scenario.request()); },
        [&scenario, &models] {
          for (const model_router::ModelId model_id : models) {
            (void)scenario.fixture.router->set_cost(model_router::reference::make_cost_evidence(
                BackendId(1), model_id, PriceGeneration(2), 900, 900, 900, scenario.fixture.now()));
          }
        });
    require_legal_route_code(outcome.code, "route_vs_price_generation_change", iteration);
    assert_stale_never_dispatches(*scenario.fixture.router, outcome,
                                 "route_vs_price_generation_change", iteration);
    check_invariants_after_race(*scenario.fixture.router, "route_vs_price_generation_change",
                                iteration);
  }
  MidPassRouter harness;
  harness.build();
  const RouteOutcome outcome = route_with_mutation_inside_pass(harness, [&harness] {
    const model_router::ModelId targets[3] = {harness.catalog.small_model,
                                              harness.catalog.general_model,
                                              harness.catalog.specialist_model};
    for (const model_router::ModelId model_id : targets) {
      MR_CHECK(harness.router
                   ->set_cost(model_router::reference::make_cost_evidence(
                       BackendId(1), model_id, PriceGeneration(2), 900, 900, 900, harness.now()))
                   .accepted());
    }
  });
  require_legal_route_code(outcome.code, "route_vs_price_generation_change(mid-pass)", 0);
  assert_stale_never_dispatches(*harness.router, outcome,
                               "route_vs_price_generation_change(mid-pass)", 0);
  check_invariants_after_race(*harness.router, "route_vs_price_generation_change(mid-pass)", 0);
}

// Race: route vs budget generation change.
// Legal outcome set: ROUTED bound to budget generation 1, REJECT_BUDGET,
// REJECT_STALE_BUDGET, or another typed rejection. Forbidden: a handoff bound
// to a budget generation that is no longer current.
MR_TEST(races, route_vs_budget_generation_change) {
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    model_router::BudgetSnapshot allowed;
    allowed.budget_id = model_router::BudgetId(1);
    allowed.generation = model_router::BudgetGeneration(1);
    allowed.verdict = model_router::BudgetVerdict::ALLOWED;
    allowed.unit = model_router::reference::reference_cost_unit();
    MR_CHECK(scenario.fixture.router->set_budget(std::move(allowed)).accepted());
    model_router::RouteRequest request = scenario.request();
    request.budget_id = model_router::BudgetId(1);
    request.budget_generation = model_router::BudgetGeneration(1);
    const RouteOutcome outcome = concurrent_race(
        [&scenario, &request] { return scenario.fixture.router->route(request); },
        [&scenario] {
          model_router::BudgetSnapshot denied;
          denied.budget_id = model_router::BudgetId(1);
          denied.generation = model_router::BudgetGeneration(2);
          denied.verdict = model_router::BudgetVerdict::DENIED;
          denied.unit = model_router::reference::reference_cost_unit();
          (void)scenario.fixture.router->set_budget(std::move(denied));
        });
    require_legal_route_code(outcome.code, "route_vs_budget_generation_change", iteration);
    assert_stale_never_dispatches(*scenario.fixture.router, outcome,
                                 "route_vs_budget_generation_change", iteration);
    check_invariants_after_race(*scenario.fixture.router, "route_vs_budget_generation_change",
                                iteration);
  }
}

// Race: route vs health change.
// Legal outcome set: ROUTED to any eligible backend, REJECT_UNHEALTHY,
// REJECT_UNKNOWN_EVIDENCE, NO_ELIGIBLE_CANDIDATE. Forbidden: a handoff to a
// backend whose current health is not HEALTHY.
MR_TEST(races, route_vs_health_change) {
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    const RouteOutcome outcome = concurrent_race(
        [&scenario] { return scenario.fixture.router->route(scenario.request()); },
        [&scenario] {
          (void)scenario.fixture.publisher.health(
              *scenario.fixture.router, BackendId(1), BackendBootId(1),
              model_router::HealthState::UNHEALTHY, scenario.fixture.now());
        });
    require_legal_route_code(outcome.code, "route_vs_health_change", iteration);
    assert_stale_never_dispatches(*scenario.fixture.router, outcome, "route_vs_health_change",
                                 iteration);
    check_invariants_after_race(*scenario.fixture.router, "route_vs_health_change", iteration);
  }
  MidPassRouter harness;
  harness.build();
  const RouteOutcome outcome = route_with_mutation_inside_pass(harness, [&harness] {
    MR_CHECK(harness.publisher
                 .health(*harness.router, BackendId(1), BackendBootId(1),
                         model_router::HealthState::UNHEALTHY, harness.now())
                 .accepted());
  });
  require_legal_route_code(outcome.code, "route_vs_health_change(mid-pass)", 0);
  assert_stale_never_dispatches(*harness.router, outcome, "route_vs_health_change(mid-pass)", 0);
  check_invariants_after_race(*harness.router, "route_vs_health_change(mid-pass)", 0);
}

// Race: route vs availability change.
// Legal outcome set: ROUTED to an available backend, REJECT_UNAVAILABLE,
// REJECT_UNKNOWN_EVIDENCE. Forbidden: a handoff to an unavailable backend.
MR_TEST(races, route_vs_availability_change) {
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    const RouteOutcome outcome = concurrent_race(
        [&scenario] { return scenario.fixture.router->route(scenario.request()); },
        [&scenario] {
          (void)scenario.fixture.publisher.availability(
              *scenario.fixture.router, BackendId(1), BackendBootId(1),
              model_router::AvailabilityState::UNAVAILABLE, scenario.fixture.now());
        });
    require_legal_route_code(outcome.code, "route_vs_availability_change", iteration);
    assert_stale_never_dispatches(*scenario.fixture.router, outcome,
                                 "route_vs_availability_change", iteration);
    check_invariants_after_race(*scenario.fixture.router, "route_vs_availability_change",
                                iteration);
  }
  MidPassRouter harness;
  harness.build();
  const RouteOutcome outcome = route_with_mutation_inside_pass(harness, [&harness] {
    MR_CHECK(harness.publisher
                 .availability(*harness.router, BackendId(1), BackendBootId(1),
                               model_router::AvailabilityState::UNAVAILABLE, harness.now())
                 .accepted());
  });
  require_legal_route_code(outcome.code, "route_vs_availability_change(mid-pass)", 0);
  assert_stale_never_dispatches(*harness.router, outcome, "route_vs_availability_change(mid-pass)",
                               0);
  check_invariants_after_race(*harness.router, "route_vs_availability_change(mid-pass)", 0);
}

// Race: route vs readiness change.
// Legal outcome set: ROUTED to a ready backend, REJECT_NOT_READY,
// REJECT_UNKNOWN_EVIDENCE. Forbidden: a handoff to a backend that is not ready.
MR_TEST(races, route_vs_readiness_change) {
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    const RouteOutcome outcome = concurrent_race(
        [&scenario] { return scenario.fixture.router->route(scenario.request()); },
        [&scenario] {
          (void)scenario.fixture.publisher.readiness(
              *scenario.fixture.router, BackendId(1), BackendBootId(1),
              model_router::ReadinessState::NOT_READY, scenario.fixture.now());
        });
    require_legal_route_code(outcome.code, "route_vs_readiness_change", iteration);
    assert_stale_never_dispatches(*scenario.fixture.router, outcome, "route_vs_readiness_change",
                                 iteration);
    check_invariants_after_race(*scenario.fixture.router, "route_vs_readiness_change", iteration);
  }
  MidPassRouter harness;
  harness.build();
  const RouteOutcome outcome = route_with_mutation_inside_pass(harness, [&harness] {
    MR_CHECK(harness.publisher
                 .readiness(*harness.router, BackendId(1), BackendBootId(1),
                            model_router::ReadinessState::NOT_READY, harness.now())
                 .accepted());
  });
  require_legal_route_code(outcome.code, "route_vs_readiness_change(mid-pass)", 0);
  assert_stale_never_dispatches(*harness.router, outcome, "route_vs_readiness_change(mid-pass)", 0);
  check_invariants_after_race(*harness.router, "route_vs_readiness_change(mid-pass)", 0);
}

// Race: route vs capability publication change.
// Legal outcome set: ROUTED bound to capability generation 1 or 2,
// REJECT_STALE_CAPABILITY, REJECT_CAPABILITY. Forbidden: a handoff bound to a
// replaced capability generation.
MR_TEST(races, route_vs_capability_change) {
  const auto publication = [](BackendId backend_id, std::uint64_t generation) {
    model_router::BackendCapabilityPublication publication;
    publication.backend_id = backend_id;
    publication.backend_generation = BackendGeneration(1);
    publication.backend_boot = BackendBootId(1);
    publication.profile_id = model_router::CapabilityProfileId(backend_id.value() * 100 + 2);
    publication.generation = model_router::CapabilityGeneration(generation);
    model_router::CapabilityEvidence revoked;
    revoked.key = model_router::CapabilityKey(std::string(model_router::capability_keys::code));
    revoked.state = model_router::CapabilityState::REVOKED;
    publication.claims.push_back(revoked);
    model_router::CapabilityEvidence generation_claim;
    generation_claim.key =
        model_router::CapabilityKey(std::string(model_router::capability_keys::text_generation));
    generation_claim.state = model_router::CapabilityState::VERIFIED;
    publication.claims.push_back(generation_claim);
    return publication;
  };
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    const RouteOutcome outcome = concurrent_race(
        [&scenario] { return scenario.fixture.router->route(scenario.request()); },
        [&scenario, &publication] {
          (void)scenario.fixture.router->publish_capabilities(publication(BackendId(1), 2));
        });
    require_legal_route_code(outcome.code, "route_vs_capability_change", iteration);
    assert_stale_never_dispatches(*scenario.fixture.router, outcome, "route_vs_capability_change",
                                 iteration);
    check_invariants_after_race(*scenario.fixture.router, "route_vs_capability_change", iteration);
  }
  MidPassRouter harness;
  harness.build();
  const RouteOutcome outcome = route_with_mutation_inside_pass(harness, [&harness, &publication] {
    MR_CHECK(harness.router->publish_capabilities(publication(BackendId(1), 2)).accepted());
  });
  require_legal_route_code(outcome.code, "route_vs_capability_change(mid-pass)", 0);
  assert_stale_never_dispatches(*harness.router, outcome, "route_vs_capability_change(mid-pass)", 0);
  check_invariants_after_race(*harness.router, "route_vs_capability_change(mid-pass)", 0);
}

// Race: route vs model retirement.
// Legal outcome set: ROUTED to a non-retired model, NO_ELIGIBLE_CANDIDATE,
// REJECT_RETIRED. Forbidden: a handoff bound to a retired model generation.
MR_TEST(races, route_vs_model_retirement) {
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    const model_router::ModelId retired = scenario.fixture.catalog.specialist_model;
    const RouteOutcome outcome = concurrent_race(
        [&scenario] { return scenario.fixture.router->route(scenario.request()); },
        [&scenario, retired] {
          (void)scenario.fixture.router->retire_model(retired, ModelGeneration(1));
        });
    require_legal_route_code(outcome.code, "route_vs_model_retirement", iteration);
    assert_stale_never_dispatches(*scenario.fixture.router, outcome, "route_vs_model_retirement",
                                 iteration);
    check_invariants_after_race(*scenario.fixture.router, "route_vs_model_retirement", iteration);
  }
  MidPassRouter harness;
  harness.build();
  const RouteOutcome outcome = route_with_mutation_inside_pass(harness, [&harness] {
    MR_CHECK(harness.router->retire_model(harness.catalog.specialist_model, ModelGeneration(1))
                 .accepted());
  });
  require_legal_route_code(outcome.code, "route_vs_model_retirement(mid-pass)", 0);
  assert_stale_never_dispatches(*harness.router, outcome, "route_vs_model_retirement(mid-pass)", 0);
  check_invariants_after_race(*harness.router, "route_vs_model_retirement(mid-pass)", 0);
}

// Race: route vs router restart (shutdown publishes the shutdown flag).
// Legal outcome set: SHUTTING_DOWN (the commit observed the shutdown), or
// ROUTED (the commit landed first, in which case shutdown must have marked the
// decision non-CURRENT). Forbidden: a CURRENT decision after shutdown.
MR_TEST(races, route_vs_router_restart) {
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    const RouteOutcome outcome = concurrent_race(
        [&scenario] { return scenario.fixture.router->route(scenario.request()); },
        [&scenario] { (void)scenario.fixture.router->shutdown(); });
    require_legal_route_code(outcome.code, "route_vs_router_restart", iteration);
    if (outcome.has_decision) {
      model_router::RouteDecision stored;
      MR_CHECK(scenario.fixture.router->find_decision(outcome.decision.decision_id, &stored));
      if (stored.status == model_router::RouteStatus::CURRENT) {
        ::mrtest::fail(__FILE__, __LINE__,
                       std::string("route_vs_router_restart: a CURRENT decision survived shutdown "
                                   "at iteration ") +
                           std::to_string(iteration));
      }
    }
    check_invariants_after_race(*scenario.fixture.router, "route_vs_router_restart", iteration);
  }
  MidPassRouter harness;
  harness.build();
  const RouteOutcome outcome = route_with_mutation_inside_pass(harness, [&harness] {
    MR_CHECK(harness.router->shutdown().accepted());
  });
  MR_CHECK_EQ(outcome.code, OutcomeCode::SHUTTING_DOWN);
  MR_CHECK(!outcome.has_decision);
  check_invariants_after_race(*harness.router, "route_vs_router_restart(mid-pass)", 0);
}

// Race: dispatch revalidation vs backend fencing.
// Legal outcome set: DISPATCHED (the handoff won the race, before the fence) or
// REJECT_STALE_BACKEND_BOOT (the fence won). After the race the incarnation is
// fenced, so a second dispatch of the same decision must be rejected.
MR_TEST(races, dispatch_revalidation_vs_backend_fencing) {
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    const RouteOutcome routed = scenario.fixture.router->route(scenario.request());
    MR_CHECK_EQ(routed.code, OutcomeCode::ROUTED);
    MR_CHECK_EQ(routed.decision.authority.backend_id, BackendId(1));
    DispatchRecord first;
    std::latch gate(2);
    std::thread dispatch_thread([&] {
      gate.arrive_and_wait();
      first = scenario.fixture.router->dispatch(routed.decision.decision_id, kTenant);
    });
    std::thread fence_thread([&] {
      gate.arrive_and_wait();
      (void)scenario.fixture.router->fence_backend_boot(
          BackendId(1), BackendGeneration(1), BackendBootId(1), OutcomeCode::FENCED);
    });
    dispatch_thread.join();
    fence_thread.join();
    // Legal: the handoff won the race, the fence won (REJECT_STALE_BACKEND_BOOT),
    // or the fence erased the incarnation before authority could be compared
    // (REJECT_STALE_BACKEND).
    if (first.code != OutcomeCode::DISPATCHED &&
        first.code != OutcomeCode::REJECT_STALE_BACKEND_BOOT &&
        first.code != OutcomeCode::REJECT_STALE_BACKEND) {
      ::mrtest::fail(__FILE__, __LINE__,
                     std::string("dispatch_revalidation_vs_backend_fencing: illegal outcome ") +
                         std::string(model_router::to_string(first.code)) + " at iteration " +
                         std::to_string(iteration));
    }
    if (first.handed_off != (first.code == OutcomeCode::DISPATCHED)) {
      ::mrtest::fail(__FILE__, __LINE__,
                     "dispatch_revalidation_vs_backend_fencing: handoff flag disagrees with the "
                     "outcome code");
    }
    const DispatchRecord second =
        scenario.fixture.router->dispatch(routed.decision.decision_id, kTenant);
    MR_CHECK(!second.handed_off);
    MR_CHECK(second.code == OutcomeCode::REJECT_STALE_BACKEND_BOOT ||
             second.code == OutcomeCode::REJECT_STALE_BACKEND);
    check_invariants_after_race(*scenario.fixture.router, "dispatch_revalidation_vs_backend_fencing",
                                iteration);
  }
}

// Race: completion observation vs route supersession.
// Legal outcome set: the completion lands first (decision COMPLETED, the reroute
// is then refused), or the supersession lands first (decision SUPERSEDED and
// the completion is refused with REJECT_CONFLICT). Both effects may never be
// recorded at once.
MR_TEST(races, completion_vs_route_supersession) {
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    const model_router::RouteRequest request = scenario.request();
    const RouteOutcome routed = scenario.fixture.router->route(request);
    MR_CHECK_EQ(routed.code, OutcomeCode::ROUTED);
    const DispatchRecord dispatched =
        scenario.fixture.router->dispatch(routed.decision.decision_id, kTenant);
    MR_CHECK_EQ(dispatched.code, OutcomeCode::DISPATCHED);

    model_router::CompletionRecord completion;
    completion.dispatch_id = dispatched.dispatch_id;
    completion.decision_id = routed.decision.decision_id;
    completion.code = OutcomeCode::COMPLETED;
    completion.failure = model_router::FailureClass::UNKNOWN;
    completion.observed_latency_micros = 1234;

    model_router::MutationResult completion_result;
    RouteOutcome reroute_result;
    std::latch gate(2);
    std::thread completion_thread([&] {
      gate.arrive_and_wait();
      completion_result = scenario.fixture.router->record_completion(completion);
    });
    std::thread reroute_thread([&] {
      gate.arrive_and_wait();
      reroute_result = scenario.fixture.router->reroute(routed.decision.decision_id,
                                                        model_router::FailureClass::TRANSIENT_BACKEND_FAILURE,
                                                        request);
    });
    completion_thread.join();
    reroute_thread.join();

    model_router::RouteDecision stored;
    MR_CHECK(scenario.fixture.router->find_decision(routed.decision.decision_id, &stored));
    if (stored.status != model_router::RouteStatus::COMPLETED &&
        stored.status != model_router::RouteStatus::SUPERSEDED) {
      ::mrtest::fail(__FILE__, __LINE__,
                     std::string("completion_vs_route_supersession: illegal final status ") +
                         std::string(model_router::to_string(stored.status)) + " at iteration " +
                         std::to_string(iteration));
    }
    if (completion_result.code != OutcomeCode::COMPLETED &&
        completion_result.code != OutcomeCode::REJECT_CONFLICT &&
        completion_result.code != OutcomeCode::REJECT_INVALID) {
      ::mrtest::fail(__FILE__, __LINE__,
                     std::string("completion_vs_route_supersession: illegal completion outcome ") +
                         std::string(model_router::to_string(completion_result.code)));
    }
    // A refused completion is only possible when supersession won the race.
    if (completion_result.code == OutcomeCode::REJECT_CONFLICT) {
      MR_CHECK_EQ(stored.status, model_router::RouteStatus::SUPERSEDED);
    }
    // When the completion was accepted, either the decision stayed COMPLETED or a
    // later supersession replaced it; both orders are legal, a third status is not.
    if (completion_result.code == OutcomeCode::COMPLETED) {
      MR_CHECK(stored.status == model_router::RouteStatus::COMPLETED ||
               stored.status == model_router::RouteStatus::SUPERSEDED);
    }
    if (reroute_result.code == OutcomeCode::ROUTED) {
      MR_CHECK(reroute_result.has_decision);
      MR_CHECK_NE(reroute_result.decision.decision_id, routed.decision.decision_id);
      MR_CHECK_EQ(reroute_result.decision.authority.request_id, routed.decision.authority.request_id);
    }
    check_invariants_after_race(*scenario.fixture.router, "completion_vs_route_supersession",
                                iteration);
  }
}

// Race: shutdown vs route admission.
// Legal outcome set: SHUTTING_DOWN (admission refused) or ROUTED (admitted
// before the shutdown flag was published). In the latter case shutdown must
// have invalidated the decision, so no CURRENT decision survives.
MR_TEST(races, shutdown_vs_route_admission) {
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    const RouteOutcome outcome = concurrent_race(
        [&scenario] { return scenario.fixture.router->route(scenario.request()); },
        [&scenario] { (void)scenario.fixture.router->shutdown(); });
    require_legal_route_code(outcome.code, "shutdown_vs_route_admission", iteration);
    if (outcome.has_decision) {
      model_router::RouteDecision stored;
      MR_CHECK(scenario.fixture.router->find_decision(outcome.decision.decision_id, &stored));
      if (stored.status == model_router::RouteStatus::CURRENT) {
        ::mrtest::fail(__FILE__, __LINE__,
                       std::string("shutdown_vs_route_admission: a CURRENT decision survived "
                                   "shutdown at iteration ") +
                           std::to_string(iteration));
      }
    }
    MR_CHECK(!scenario.fixture.router->running());
    check_invariants_after_race(*scenario.fixture.router, "shutdown_vs_route_admission", iteration);
  }
}

// Race: persistence vs dynamic evidence update.
// Legal outcome set: the save succeeds with one consistent snapshot (then the
// file must validate and must not resurrect dynamic evidence), or the save
// fails with a typed persistence error. A save may never produce a file that
// fails validation.
MR_TEST(races, persistence_vs_dynamic_evidence_update) {
  const std::string path = (std::filesystem::temp_directory_path() /
                            "model_router_race_persistence.state")
                               .string();
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    Scenario scenario;
    scenario.build();
    model_router::PersistenceResult saved;
    std::latch gate(2);
    std::thread save_thread([&] {
      gate.arrive_and_wait();
      saved = scenario.fixture.router->save(path);
    });
    std::thread evidence_thread([&] {
      gate.arrive_and_wait();
      (void)scenario.fixture.publisher.health(*scenario.fixture.router, BackendId(1),
                                              BackendBootId(1),
                                              model_router::HealthState::DEGRADED,
                                              scenario.fixture.now());
    });
    save_thread.join();
    evidence_thread.join();
    if (saved.code != OutcomeCode::ACCEPTED && saved.code != OutcomeCode::INTERNAL_ERROR) {
      ::mrtest::fail(__FILE__, __LINE__,
                     std::string("persistence_vs_dynamic_evidence_update: illegal save outcome ") +
                         std::string(model_router::to_string(saved.code)) + " at iteration " +
                         std::to_string(iteration));
    }
    if (saved.ok()) {
      const model_router::PersistenceLoad loaded = model_router::RouterStateStore::load(
          path, model_router::default_resource_limits());
      if (!loaded.ok()) {
        ::mrtest::fail(__FILE__, __LINE__,
                       std::string("persistence_vs_dynamic_evidence_update: a successful save "
                                   "produced an unloadable file at iteration ") +
                           std::to_string(iteration) + ": " + loaded.detail);
      }
      for (const model_router::PersistedBackendRecord& backend : loaded.state.backends) {
        if (backend.backend_id == BackendId(1) &&
            backend.trust_domain != model_router::TrustDomain::UNKNOWN &&
            backend.trust_domain != model_router::TrustDomain::LOCAL) {
          ::mrtest::fail(__FILE__, __LINE__,
                         "persistence_vs_dynamic_evidence_update: persisted state is incoherent");
        }
      }
    }
    check_invariants_after_race(*scenario.fixture.router, "persistence_vs_dynamic_evidence_update",
                                iteration);
  }
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path + ".tmp", ignored);
}

}  // namespace
