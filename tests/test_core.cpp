// Model Router - core routing behaviour.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "router_fixture.hpp"

using model_router::BackendBootId;
using model_router::BackendGeneration;
using model_router::BackendId;
using model_router::CostUnit;
using model_router::Modality;
using model_router::ModalitySet;
using model_router::Money;
using model_router::OutcomeCode;
using model_router::PolicyGeneration;
using model_router::PolicyId;
using model_router::RouteOutcome;
using model_router::TenantId;

namespace {

constexpr TenantId kTenant(1);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);

[[nodiscard]] model_router::CapabilityRequirement require_capability(std::string key) {
  model_router::CapabilityRequirement requirement;
  requirement.key = model_router::CapabilityKey(std::move(key));
  requirement.minimum_state = model_router::CapabilityState::DECLARED;
  return requirement;
}

}  // namespace

MR_TEST(core, start_publishes_identity_and_advances_epochs) {
  mrtest::Fixture fixture;
  fixture.start();
  const model_router::RouterSummary summary = fixture.router->summary();
  MR_CHECK(summary.running);
  MR_CHECK(summary.router_id.valid());
  MR_CHECK(summary.router_epoch.valid());
  MR_CHECK(summary.coordinator_epoch.valid());
  MR_CHECK(fixture.router->check_invariants().ok());
  MR_CHECK(fixture.router->shutdown().accepted());
  MR_CHECK(!fixture.router->running());
}

MR_TEST(core, capability_request_routes_to_the_only_capable_model) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);

  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));

  const RouteOutcome outcome = fixture.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK(outcome.has_decision);
  MR_CHECK_EQ(outcome.decision.authority.model_id, fixture.catalog.specialist_model);
  MR_CHECK(outcome.route.ranking.size() == 1);
  MR_CHECK(fixture.router->check_invariants().ok());
}

MR_TEST(core, context_limit_is_a_hard_rejection) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);

  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  request.requirements.min_context_tokens = 200000;

  const RouteOutcome outcome = fixture.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::NO_ELIGIBLE_CANDIDATE);
  // Three bound models are discovered: the two without the required capability
  // fail capability first, and the specialist fails on context length.
  MR_CHECK_EQ(outcome.route.rejections.size(), std::size_t{3});
  MR_CHECK_EQ(outcome.route.rejections[0].code, OutcomeCode::REJECT_CAPABILITY);
  MR_CHECK_EQ(outcome.route.rejections[2].code, OutcomeCode::REJECT_CONTEXT_LIMIT);
}

MR_TEST(core, unknown_health_never_satisfies_a_hard_requirement) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  // Register without publishing any dynamic evidence.
  model_router::BackendDescriptor backend = model_router::reference::make_backend(
      fixture.catalog, BackendId(1), BackendGeneration(1), BackendBootId(1),
      model_router::BackendRegistrationGeneration(1), "127.0.0.1:7001",
      model_router::Provenance::SYNTHETIC, fixture.now());
  MR_CHECK(fixture.router->register_backend(std::move(backend)).accepted());

  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  const RouteOutcome outcome = fixture.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::NO_ELIGIBLE_CANDIDATE);
  MR_CHECK_EQ(outcome.route.rejections.front().code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
}

MR_TEST(core, cheaper_candidate_wins_when_requirements_allow_it) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);
  fixture.add_backend(BackendId(2), 1);
  fixture.publish_costs(BackendId(1), model_router::PriceGeneration(1), 900, 900, 900);
  fixture.publish_costs(BackendId(2), model_router::PriceGeneration(1), 100, 100, 100);

  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  request.requirements.require_known_cost = true;

  const RouteOutcome outcome = fixture.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, BackendId(2));
}

MR_TEST(core, capability_requirement_beats_cost) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);
  fixture.publish_costs(BackendId(1), model_router::PriceGeneration(1), 1, 1, 1);

  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::vision_input)));
  request.requirements.require_known_cost = true;

  const RouteOutcome outcome = fixture.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  // Only the specialist model accepts image input; the cheapest model loses.
  MR_CHECK_EQ(outcome.decision.authority.model_id, fixture.catalog.specialist_model);
}

MR_TEST(core, dispatch_and_completion_are_recorded) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);

  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  const RouteOutcome outcome = fixture.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);

  const model_router::DispatchRecord dispatched =
      fixture.router->dispatch(outcome.decision.decision_id, kTenant);
  MR_CHECK_EQ(dispatched.code, OutcomeCode::DISPATCHED);
  MR_CHECK(dispatched.handed_off);
  MR_CHECK_EQ(fixture.dispatcher->calls, std::uint64_t{1});

  model_router::CompletionRecord completion;
  completion.dispatch_id = dispatched.dispatch_id;
  completion.decision_id = outcome.decision.decision_id;
  completion.code = OutcomeCode::COMPLETED;
  completion.failure = model_router::FailureClass::UNKNOWN;
  completion.observed_latency_micros = 1234;
  MR_CHECK_EQ(fixture.router->record_completion(completion).code, OutcomeCode::COMPLETED);

  model_router::RouteDecision stored;
  MR_CHECK(fixture.router->find_decision(outcome.decision.decision_id, &stored));
  MR_CHECK_EQ(stored.status, model_router::RouteStatus::COMPLETED);
  MR_CHECK(fixture.router->check_invariants().ok());
}

MR_TEST(core, no_dispatch_handler_is_an_explicit_rejection) {
  mrtest::Fixture fixture;
  model_router::ModelRouterOptions options;
  options.clock = fixture.clock;
  model_router::ModelRouter router(std::move(options));
  MR_CHECK(router.start().accepted());
  for (const model_router::ModelDescriptor& model :
       model_router::reference::make_models(fixture.catalog, fixture.now())) {
    MR_CHECK(router.register_model(model).accepted());
  }
  MR_CHECK(router.register_provider([] {
    model_router::ProviderDescriptor descriptor;
    descriptor.provider_id = model_router::ProviderId(1);
    descriptor.generation = model_router::ProviderGeneration(1);
    return descriptor;
  }()).accepted());
  model_router::BackendDescriptor backend = model_router::reference::make_backend(
      fixture.catalog, BackendId(1), BackendGeneration(1), BackendBootId(1),
      model_router::BackendRegistrationGeneration(1), "127.0.0.1:7001",
      model_router::Provenance::SYNTHETIC, fixture.now());
  MR_CHECK(router.register_backend(std::move(backend)).accepted());
  MR_CHECK(mrtest::install_open_policy(router, kPolicy, kPolicyGeneration).accepted());
  const model_router::UnixMillis current = fixture.now();
  MR_CHECK(fixture.publisher
               .health(router, BackendId(1), BackendBootId(1), model_router::HealthState::HEALTHY,
                       current)
               .accepted());
  MR_CHECK(fixture.publisher
               .availability(router, BackendId(1), BackendBootId(1),
                             model_router::AvailabilityState::AVAILABLE, current)
               .accepted());
  MR_CHECK(fixture.publisher
               .readiness(router, BackendId(1), BackendBootId(1),
                          model_router::ReadinessState::READY, current)
               .accepted());

  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  const RouteOutcome outcome = router.route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  const model_router::DispatchRecord dispatched = router.dispatch(outcome.decision.decision_id);
  MR_CHECK_EQ(dispatched.code, OutcomeCode::REJECT_INVALID);
  MR_CHECK(!dispatched.handed_off);
  MR_CHECK(dispatched.detail.find("no dispatch handler") != std::string::npos);
}

MR_TEST(core, policy_denial_selects_the_alternate_legal_backend) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);
  fixture.add_backend(BackendId(2), 1);
  fixture.publish_costs(BackendId(1), model_router::PriceGeneration(1), 100, 100, 100);
  fixture.publish_costs(BackendId(2), model_router::PriceGeneration(1), 900, 900, 900);

  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  request.requirements.require_known_cost = true;
  MR_CHECK_EQ(fixture.router->route(request).decision.authority.backend_id, BackendId(1));

  // Policy generation 2 denies backend 1 entirely.
  model_router::PolicySnapshot policy =
      model_router::PolicyBuilder(kPolicy, PolicyGeneration(2))
          .deny_backend(BackendId(1))
          .fallback_policy(model_router::FallbackPolicy::PERMITTED)
          .build();
  MR_CHECK(fixture.router->set_policy(std::move(policy)).accepted());

  request.request_id = model_router::allocate_id<model_router::RouteRequestTag>();
  request.policy_generation = PolicyGeneration(2);
  const RouteOutcome outcome = fixture.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, BackendId(2));
}

MR_TEST(core, budget_denial_is_hard) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);

  model_router::BudgetSnapshot budget;
  budget.budget_id = model_router::BudgetId(1);
  budget.generation = model_router::BudgetGeneration(1);
  budget.verdict = model_router::BudgetVerdict::DENIED;
  budget.unit = model_router::reference::reference_cost_unit();
  MR_CHECK(fixture.router->set_budget(std::move(budget)).accepted());

  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  request.budget_id = model_router::BudgetId(1);
  request.budget_generation = model_router::BudgetGeneration(1);

  const RouteOutcome outcome = fixture.router->route(request);
  // A budget denial is a request-level hard rejection: no candidate is ever
  // considered, and the rejection is typed.
  MR_CHECK_EQ(outcome.code, OutcomeCode::REJECT_BUDGET);
  MR_CHECK(!outcome.has_decision);
}

MR_TEST(core, tenant_isolation_holds_across_policies_and_decisions) {
  mrtest::Fixture fixture;
  fixture.start();
  fixture.add_backend(BackendId(1), 1);
  fixture.add_backend(BackendId(2), 1);
  fixture.publish_costs(BackendId(1), model_router::PriceGeneration(1), 100, 100, 100);
  fixture.publish_costs(BackendId(2), model_router::PriceGeneration(1), 900, 900, 900);

  const model_router::TenantId tenant_a(11);
  const model_router::TenantId tenant_b(22);
  const model_router::PolicyId policy_a(2);
  const model_router::PolicyId policy_b(3);

  // Tenant A's policy denies backend 1 and is bound to tenant A only.
  model_router::PolicySnapshot policy_for_a =
      model_router::PolicyBuilder(policy_a, PolicyGeneration(1))
          .deny_backend(BackendId(1))
          .bind_tenant(tenant_a)
          .build();
  MR_CHECK(fixture.router->set_policy(std::move(policy_for_a)).accepted());

  model_router::RouteRequest request_a =
      mrtest::make_request(fixture, tenant_a, policy_a, PolicyGeneration(1));
  request_a.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  request_a.requirements.require_known_cost = true;
  const RouteOutcome outcome_a = fixture.router->route(request_a);
  MR_CHECK_EQ(outcome_a.code, OutcomeCode::ROUTED);
  // The cheapest backend is denied for tenant A, so it must not leak into A's route.
  MR_CHECK_EQ(outcome_a.decision.authority.backend_id, BackendId(2));

  // A decision bound to tenant A cannot be revalidated or dispatched by tenant B.
  MR_CHECK_EQ(fixture.router->revalidate(outcome_a.decision.decision_id, tenant_b).code,
              OutcomeCode::REJECT_CONFLICT);
  MR_CHECK_EQ(fixture.router->dispatch(outcome_a.decision.decision_id, tenant_b).code,
              OutcomeCode::REJECT_CONFLICT);
  // The owning tenant can revalidate its own decision while its policy is current.
  MR_CHECK(fixture.router->revalidate(outcome_a.decision.decision_id, tenant_a).accepted());

  // The same request shape for tenant B under a policy that allows backend 1.
  model_router::PolicySnapshot policy_for_b =
      model_router::PolicyBuilder(policy_b, PolicyGeneration(2))
          .bind_tenant(tenant_b)
          .build();
  MR_CHECK(fixture.router->set_policy(std::move(policy_for_b)).accepted());

  model_router::RouteRequest request_b =
      mrtest::make_request(fixture, tenant_b, policy_b, PolicyGeneration(2));
  request_b.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  request_b.requirements.require_known_cost = true;
  const RouteOutcome outcome_b = fixture.router->route(request_b);
  MR_CHECK_EQ(outcome_b.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome_b.decision.authority.backend_id, BackendId(1));
  // Tenant A's decision bound an older policy generation and is now stale.
  MR_CHECK(model_router::is_rejection(
      fixture.router->revalidate(outcome_a.decision.decision_id, tenant_a).code));
  MR_CHECK(fixture.router->check_invariants().ok());
}

MR_TEST(core, identical_state_produces_identical_routing) {
  // The same canonical state must produce the same winner, digest and
  // explanation text regardless of the order in which candidates were created.
  const auto run = [](bool reverse) {
    mrtest::Fixture fixture;
    fixture.start();
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    if (reverse) {
      fixture.add_backend(BackendId(3), 1);
      fixture.add_backend(BackendId(2), 1);
      fixture.add_backend(BackendId(1), 1);
    } else {
      fixture.add_backend(BackendId(1), 1);
      fixture.add_backend(BackendId(2), 1);
      fixture.add_backend(BackendId(3), 1);
    }
    fixture.publish_costs(BackendId(1), model_router::PriceGeneration(1), 100, 100, 100);
    fixture.publish_costs(BackendId(2), model_router::PriceGeneration(1), 400, 400, 400);
    fixture.publish_costs(BackendId(3), model_router::PriceGeneration(1), 900, 900, 900);
    model_router::RouteRequest request =
        mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
    request.requirements.required_capabilities.push_back(
        require_capability(std::string(model_router::capability_keys::code)));
    request.requirements.require_known_cost = true;
    return fixture.router->route(request);
  };

  const RouteOutcome forward = run(false);
  const RouteOutcome reversed = run(true);
  MR_CHECK_EQ(forward.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(reversed.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(forward.decision.authority.backend_id, reversed.decision.authority.backend_id);
  MR_CHECK_EQ(forward.decision.authority.model_id, reversed.decision.authority.model_id);
  // The explanation digest is content-derived, so identical canonical state
  // produces an identical value. A decision digest deliberately also binds its
  // own identity and is therefore compared through its content component.
  MR_CHECK_EQ(forward.decision.explanation.semantic_digest,
              reversed.decision.explanation.semantic_digest);
  MR_CHECK_EQ(forward.decision.fallbacks.size(), reversed.decision.fallbacks.size());
  MR_CHECK_EQ(forward.route.ranking.size(), reversed.route.ranking.size());
  MR_CHECK(forward.route.ranking == reversed.route.ranking);
  MR_CHECK_EQ(forward.route.rejections, reversed.route.rejections);
  MR_CHECK_EQ(forward.route.winner, reversed.route.winner);
  MR_CHECK_EQ(forward.route.fallback_order, reversed.route.fallback_order);
}

MR_TEST(core, cost_ceiling_rejects_without_comparable_units) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);
  fixture.publish_costs(BackendId(1), model_router::PriceGeneration(1), 500000, 500000, 500000);

  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  request.requirements.enforce_cost_ceiling = true;
  request.requirements.maximum_cost.unit.currency = "EUR";
  request.requirements.maximum_cost.unit.basis = "per-million-tokens";
  request.requirements.maximum_cost.micros = 1000000;

  const RouteOutcome outcome = fixture.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::NO_ELIGIBLE_CANDIDATE);
  bool saw_cost_rejection = false;
  for (const model_router::RouteRejection& rejection : outcome.route.rejections) {
    if (rejection.code == OutcomeCode::REJECT_COST) {
      saw_cost_rejection = true;
    }
  }
  MR_CHECK(saw_cost_rejection);
}
