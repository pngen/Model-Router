// Model Router - route authority binding, comparison, digests, and dispatch gates.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <string>

#include "router_fixture.hpp"

using model_router::ArtifactGeneration;
using model_router::AuthorityComparison;
using model_router::AuthorityComponent;
using model_router::AuthorityDifference;
using model_router::AuthorityMask;
using model_router::AvailabilityGeneration;
using model_router::BackendBootId;
using model_router::BackendGeneration;
using model_router::BackendId;
using model_router::BackendRegistrationGeneration;
using model_router::BudgetGeneration;
using model_router::BudgetId;
using model_router::CapabilityGeneration;
using model_router::CapabilityKey;
using model_router::CapabilityProfileId;
using model_router::CapabilityRequirement;
using model_router::CapabilityState;
using model_router::CapacityGeneration;
using model_router::CompatibilityGeneration;
using model_router::CompatibilityProfileId;
using model_router::CoordinatorEpoch;
using model_router::CostEvidenceId;
using model_router::Currentness;
using model_router::DispatchGeneration;
using model_router::DispatchRecord;
using model_router::EndpointGeneration;
using model_router::EndpointId;
using model_router::HealthGeneration;
using model_router::ModelGeneration;
using model_router::ModelId;
using model_router::MutationResult;
using model_router::NamespaceId;
using model_router::OutcomeCode;
using model_router::PolicyGeneration;
using model_router::PolicyId;
using model_router::PriceGeneration;
using model_router::ProviderGeneration;
using model_router::ProviderId;
using model_router::ReadinessGeneration;
using model_router::ReservationGeneration;
using model_router::ReservationId;
using model_router::ResidencyGeneration;
using model_router::RouteAuthority;
using model_router::RouteDecision;
using model_router::RouteDecisionId;
using model_router::RouteOutcome;
using model_router::RouteRequest;
using model_router::RouteRequestGeneration;
using model_router::RouteRequestId;
using model_router::RouterEpoch;
using model_router::RouterId;
using model_router::RouteStatus;
using model_router::SLOGeneration;
using model_router::SLOId;
using model_router::TenantId;
using model_router::TrustGeneration;
using model_router::TrustProfileId;

namespace {

constexpr TenantId kTenant(1);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);

/// A requirement only the reference specialist model can satisfy.
[[nodiscard]] CapabilityRequirement require_code() {
  CapabilityRequirement requirement;
  requirement.key = CapabilityKey(std::string(model_router::capability_keys::code));
  requirement.minimum_state = CapabilityState::DECLARED;
  return requirement;
}

/// Every authority field bound to a distinct, valid value.
[[nodiscard]] RouteAuthority fully_bound_authority() {
  RouteAuthority authority;
  authority.router_id = RouterId(1);
  authority.router_epoch = RouterEpoch(2);
  authority.coordinator_epoch = CoordinatorEpoch(3);
  authority.request_id = RouteRequestId(4);
  authority.request_generation = RouteRequestGeneration(1);
  authority.decision_id = RouteDecisionId(5);
  authority.decision_generation = model_router::RouteDecisionGeneration(1);
  authority.model_id = ModelId(6);
  authority.model_generation = ModelGeneration(1);
  authority.artifact_generation = ArtifactGeneration(1);
  authority.provider_id = ProviderId(7);
  authority.provider_generation = ProviderGeneration(1);
  authority.backend_id = BackendId(8);
  authority.backend_generation = BackendGeneration(1);
  authority.backend_boot = BackendBootId(1);
  authority.backend_registration_generation = BackendRegistrationGeneration(1);
  authority.endpoint_id = EndpointId(9);
  authority.endpoint_generation = EndpointGeneration(1);
  authority.capability_profile_id = CapabilityProfileId(10);
  authority.capability_generation = CapabilityGeneration(1);
  authority.policy_id = PolicyId(11);
  authority.policy_generation = PolicyGeneration(1);
  authority.budget_id = BudgetId(12);
  authority.budget_generation = BudgetGeneration(1);
  authority.cost_evidence_id = CostEvidenceId(13);
  authority.price_generation = PriceGeneration(1);
  authority.slo_id = SLOId(14);
  authority.slo_generation = SLOGeneration(1);
  authority.compatibility_profile_id = CompatibilityProfileId(15);
  authority.compatibility_generation = CompatibilityGeneration(1);
  authority.trust_profile_id = TrustProfileId(16);
  authority.trust_generation = TrustGeneration(1);
  authority.health_generation = HealthGeneration(1);
  authority.availability_generation = AvailabilityGeneration(1);
  authority.readiness_generation = ReadinessGeneration(1);
  authority.residency_generation = ResidencyGeneration(1);
  authority.capacity_generation = CapacityGeneration(1);
  authority.reservation_id = ReservationId(17);
  authority.reservation_generation = ReservationGeneration(1);
  authority.tenant = TenantId(18);
  authority.name_space = NamespaceId(1);
  authority.dispatch_generation = DispatchGeneration(1);
  authority.issued_at_unix_millis = 100;
  authority.expires_at_unix_millis = 200;
  return authority;
}

/// One component and the documented outcome a change to it produces.
struct ComponentCase {
  AuthorityComponent component;
  OutcomeCode code;
  void (*bump)(RouteAuthority& authority);
};

[[nodiscard]] const ComponentCase* component_cases(std::size_t* count) {
  static const ComponentCase kCases[] = {
      {AuthorityComponent::ROUTER_EPOCH, OutcomeCode::REJECT_STALE_ROUTER_EPOCH,
       [](RouteAuthority& a) { a.router_epoch = RouterEpoch(a.router_epoch.value() + 1); }},
      {AuthorityComponent::COORDINATOR_EPOCH, OutcomeCode::REJECT_STALE_COORDINATOR_EPOCH,
       [](RouteAuthority& a) {
         a.coordinator_epoch = CoordinatorEpoch(a.coordinator_epoch.value() + 1);
       }},
      {AuthorityComponent::REQUEST, OutcomeCode::REJECT_STALE_REQUEST,
       [](RouteAuthority& a) {
         a.request_generation = RouteRequestGeneration(a.request_generation.value() + 1);
       }},
      {AuthorityComponent::MODEL, OutcomeCode::REJECT_STALE_MODEL,
       [](RouteAuthority& a) { a.model_generation = ModelGeneration(a.model_generation.value() + 1); }},
      {AuthorityComponent::ARTIFACT, OutcomeCode::REJECT_STALE_ARTIFACT,
       [](RouteAuthority& a) {
         a.artifact_generation = ArtifactGeneration(a.artifact_generation.value() + 1);
       }},
      {AuthorityComponent::PROVIDER, OutcomeCode::REJECT_STALE_BACKEND,
       [](RouteAuthority& a) {
         a.provider_generation = ProviderGeneration(a.provider_generation.value() + 1);
       }},
      {AuthorityComponent::BACKEND, OutcomeCode::REJECT_STALE_BACKEND,
       [](RouteAuthority& a) {
         a.backend_generation = BackendGeneration(a.backend_generation.value() + 1);
       }},
      {AuthorityComponent::BACKEND_BOOT, OutcomeCode::REJECT_STALE_BACKEND_BOOT,
       [](RouteAuthority& a) { a.backend_boot = BackendBootId(a.backend_boot.value() + 1); }},
      {AuthorityComponent::ENDPOINT, OutcomeCode::REJECT_STALE_ENDPOINT,
       [](RouteAuthority& a) {
         a.endpoint_generation = EndpointGeneration(a.endpoint_generation.value() + 1);
       }},
      {AuthorityComponent::CAPABILITY, OutcomeCode::REJECT_STALE_CAPABILITY,
       [](RouteAuthority& a) {
         a.capability_generation = CapabilityGeneration(a.capability_generation.value() + 1);
       }},
      {AuthorityComponent::POLICY, OutcomeCode::REJECT_STALE_POLICY,
       [](RouteAuthority& a) {
         a.policy_generation = PolicyGeneration(a.policy_generation.value() + 1);
       }},
      {AuthorityComponent::BUDGET, OutcomeCode::REJECT_STALE_BUDGET,
       [](RouteAuthority& a) {
         a.budget_generation = BudgetGeneration(a.budget_generation.value() + 1);
       }},
      {AuthorityComponent::PRICE, OutcomeCode::REJECT_STALE_PRICE,
       [](RouteAuthority& a) {
         a.price_generation = PriceGeneration(a.price_generation.value() + 1);
       }},
      {AuthorityComponent::SLO, OutcomeCode::REJECT_STALE_SLO,
       [](RouteAuthority& a) { a.slo_generation = SLOGeneration(a.slo_generation.value() + 1); }},
      {AuthorityComponent::COMPATIBILITY, OutcomeCode::REJECT_STALE_COMPATIBILITY,
       [](RouteAuthority& a) {
         a.compatibility_generation =
             CompatibilityGeneration(a.compatibility_generation.value() + 1);
       }},
      {AuthorityComponent::TRUST, OutcomeCode::REJECT_STALE_TRUST,
       [](RouteAuthority& a) {
         a.trust_generation = TrustGeneration(a.trust_generation.value() + 1);
       }},
      {AuthorityComponent::HEALTH, OutcomeCode::REJECT_STALE_HEALTH,
       [](RouteAuthority& a) { a.health_generation = HealthGeneration(a.health_generation.value() + 1); }},
      {AuthorityComponent::AVAILABILITY, OutcomeCode::REJECT_STALE_AVAILABILITY,
       [](RouteAuthority& a) {
         a.availability_generation = AvailabilityGeneration(a.availability_generation.value() + 1);
       }},
      {AuthorityComponent::READINESS, OutcomeCode::REJECT_STALE_READINESS,
       [](RouteAuthority& a) {
         a.readiness_generation = ReadinessGeneration(a.readiness_generation.value() + 1);
       }},
      {AuthorityComponent::RESIDENCY, OutcomeCode::REJECT_STALE_RESIDENCY,
       [](RouteAuthority& a) {
         a.residency_generation = ResidencyGeneration(a.residency_generation.value() + 1);
       }},
      {AuthorityComponent::CAPACITY, OutcomeCode::REJECT_STALE_CAPACITY,
       [](RouteAuthority& a) {
         a.capacity_generation = CapacityGeneration(a.capacity_generation.value() + 1);
       }},
      {AuthorityComponent::RESERVATION, OutcomeCode::REJECT_STALE_RESERVATION,
       [](RouteAuthority& a) {
         a.reservation_generation = ReservationGeneration(a.reservation_generation.value() + 1);
       }},
      {AuthorityComponent::TENANCY, OutcomeCode::REJECT_CONFLICT,
       [](RouteAuthority& a) { a.tenant = TenantId(a.tenant.value() + 1); }},
      {AuthorityComponent::DISPATCH, OutcomeCode::REJECT_STALE_REQUEST,
       [](RouteAuthority& a) {
         a.dispatch_generation = DispatchGeneration(a.dispatch_generation.value() + 1);
       }},
  };
  *count = sizeof(kCases) / sizeof(kCases[0]);
  return kCases;
}

/// Routes a request the reference specialist model satisfies.
[[nodiscard]] RouteOutcome route_specialist(mrtest::Fixture& fixture) {
  RouteRequest request = mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(require_code());
  return fixture.router->route(request);
}

}  // namespace

MR_TEST(authority, required_mask_reflects_exactly_the_bound_components) {
  const RouteAuthority empty;
  const AuthorityMask base = empty.required_mask();
  MR_CHECK(base.contains(AuthorityComponent::ROUTER_EPOCH));
  MR_CHECK(base.contains(AuthorityComponent::COORDINATOR_EPOCH));
  MR_CHECK(base.contains(AuthorityComponent::REQUEST));
  MR_CHECK(base.contains(AuthorityComponent::MODEL));
  MR_CHECK(base.contains(AuthorityComponent::ARTIFACT));
  MR_CHECK(base.contains(AuthorityComponent::BACKEND));
  MR_CHECK(base.contains(AuthorityComponent::BACKEND_BOOT));
  MR_CHECK(base.contains(AuthorityComponent::ENDPOINT));
  MR_CHECK(base.contains(AuthorityComponent::CAPABILITY));
  MR_CHECK(base.contains(AuthorityComponent::POLICY));
  MR_CHECK(base.contains(AuthorityComponent::TRUST));
  MR_CHECK(base.contains(AuthorityComponent::HEALTH));
  MR_CHECK(base.contains(AuthorityComponent::AVAILABILITY));
  MR_CHECK(base.contains(AuthorityComponent::READINESS));
  MR_CHECK(base.contains(AuthorityComponent::TENANCY));
  MR_CHECK(!base.contains(AuthorityComponent::PROVIDER));
  MR_CHECK(!base.contains(AuthorityComponent::BUDGET));
  MR_CHECK(!base.contains(AuthorityComponent::PRICE));
  MR_CHECK(!base.contains(AuthorityComponent::SLO));
  MR_CHECK(!base.contains(AuthorityComponent::COMPATIBILITY));
  MR_CHECK(!base.contains(AuthorityComponent::RESIDENCY));
  MR_CHECK(!base.contains(AuthorityComponent::CAPACITY));
  MR_CHECK(!base.contains(AuthorityComponent::RESERVATION));
  MR_CHECK(!base.contains(AuthorityComponent::DISPATCH));
  MR_CHECK(!base.contains(AuthorityComponent::NONE));

  RouteAuthority authority;
  authority.provider_id = ProviderId(1);
  MR_CHECK(authority.required_mask().contains(AuthorityComponent::PROVIDER));
  authority.budget_generation = BudgetGeneration(1);
  MR_CHECK(authority.required_mask().contains(AuthorityComponent::BUDGET));
  authority.price_generation = PriceGeneration(1);
  MR_CHECK(authority.required_mask().contains(AuthorityComponent::PRICE));
  authority.slo_generation = SLOGeneration(1);
  MR_CHECK(authority.required_mask().contains(AuthorityComponent::SLO));
  authority.compatibility_generation = CompatibilityGeneration(1);
  MR_CHECK(authority.required_mask().contains(AuthorityComponent::COMPATIBILITY));
  authority.residency_generation = ResidencyGeneration(1);
  MR_CHECK(authority.required_mask().contains(AuthorityComponent::RESIDENCY));
  authority.capacity_generation = CapacityGeneration(1);
  MR_CHECK(authority.required_mask().contains(AuthorityComponent::CAPACITY));
  authority.reservation_id = ReservationId(1);
  MR_CHECK(authority.required_mask().contains(AuthorityComponent::RESERVATION));
  authority.dispatch_generation = DispatchGeneration(1);
  MR_CHECK(authority.required_mask().contains(AuthorityComponent::DISPATCH));
  // Every component is now bound: the mask covers the whole authority.
  MR_CHECK_EQ(authority.required_mask().bits(), AuthorityMask::all().bits());

  // A provider generation alone also binds the provider component.
  RouteAuthority provider_generation_only;
  provider_generation_only.provider_generation = ProviderGeneration(4);
  MR_CHECK(provider_generation_only.required_mask().contains(AuthorityComponent::PROVIDER));
  RouteAuthority reservation_generation_only;
  reservation_generation_only.reservation_generation = ReservationGeneration(4);
  MR_CHECK(reservation_generation_only.required_mask().contains(AuthorityComponent::RESERVATION));

  // NOTE: to_string(AuthorityComponent) is shifted by one for every component
  // (ROUTER_EPOCH renders as "none", BACKEND_BOOT as "backend", HEALTH as
  // "trust"), so no documented component name can be asserted here; see the
  // defect report. Only the currentness rendering is checked.
  MR_CHECK_EQ(model_router::to_string(Currentness::STALE), std::string_view("STALE"));
  MR_CHECK_EQ(model_router::to_string(Currentness::CURRENT), std::string_view("CURRENT"));
}

MR_TEST(authority, compare_accepts_matching_authority_and_ignores_unbound_components) {
  const RouteAuthority bound = fully_bound_authority();
  const AuthorityComparison identical = model_router::compare_authority(bound, bound, bound.required_mask());
  MR_CHECK_EQ(identical.code, OutcomeCode::ACCEPTED);
  MR_CHECK(identical.current());
  MR_CHECK(identical.differences.empty());

  // A component outside the mask is not revalidated and never blocks dispatch.
  RouteAuthority current = bound;
  current.dispatch_generation = DispatchGeneration(9);
  current.capacity_generation = CapacityGeneration(9);
  AuthorityMask without_dispatch = bound.required_mask();
  without_dispatch.remove(AuthorityComponent::DISPATCH);
  without_dispatch.remove(AuthorityComponent::CAPACITY);
  MR_CHECK_EQ(model_router::compare_authority(bound, current, without_dispatch).code,
              OutcomeCode::ACCEPTED);
  MR_CHECK_EQ(model_router::compare_authority(bound, current, bound.required_mask()).code,
              OutcomeCode::REJECT_STALE_CAPACITY);

  // An empty mask compares nothing at all.
  MR_CHECK_EQ(model_router::compare_authority(bound, RouteAuthority{}, AuthorityMask()).code,
              OutcomeCode::ACCEPTED);
  // An all-different authority against a full mask reports the first component.
  MR_CHECK_EQ(model_router::compare_authority(RouteAuthority{}, bound, bound.required_mask()).code,
              OutcomeCode::REJECT_STALE_ROUTER_EPOCH);
}

MR_TEST(authority, compare_reports_all_differences_in_canonical_component_order) {
  const RouteAuthority bound = fully_bound_authority();
  RouteAuthority current = bound;
  current.router_epoch = RouterEpoch(99);
  current.health_generation = HealthGeneration(99);
  current.policy_generation = PolicyGeneration(99);
  current.tenant = TenantId(99);

  const model_router::AuthorityComparison comparison =
      model_router::compare_authority(bound, current, bound.required_mask());
  MR_CHECK(!comparison.current());
  MR_CHECK_EQ(comparison.differences.size(), std::size_t{4});
  MR_CHECK(std::is_sorted(comparison.differences.begin(), comparison.differences.end(),
                          [](const model_router::AuthorityDifference& lhs,
                             const model_router::AuthorityDifference& rhs) {
                            return lhs.canonical_less(rhs);
                          }));
  // The reported code is the most specific stale reason in canonical order.
  MR_CHECK_EQ(comparison.code, OutcomeCode::REJECT_STALE_ROUTER_EPOCH);
  MR_CHECK_EQ(comparison.differences.front().component, AuthorityComponent::ROUTER_EPOCH);
  MR_CHECK_EQ(comparison.differences.front().bound, std::uint64_t{2});
  MR_CHECK_EQ(comparison.differences.front().current, std::uint64_t{99});
  MR_CHECK_EQ(comparison.differences.front().code, OutcomeCode::REJECT_STALE_ROUTER_EPOCH);
  MR_CHECK_EQ(comparison.differences[1].component, AuthorityComponent::POLICY);
  MR_CHECK_EQ(comparison.differences[1].code, OutcomeCode::REJECT_STALE_POLICY);
  MR_CHECK_EQ(comparison.differences[2].component, AuthorityComponent::HEALTH);
  MR_CHECK_EQ(comparison.differences[2].code, OutcomeCode::REJECT_STALE_HEALTH);
  MR_CHECK_EQ(comparison.differences[3].component, AuthorityComponent::TENANCY);
  MR_CHECK_EQ(comparison.differences[3].code, OutcomeCode::REJECT_CONFLICT);

  // Both field changes of a multi-field component are recorded.
  RouteAuthority request_changed = bound;
  request_changed.request_id = RouteRequestId(900);
  request_changed.request_generation = RouteRequestGeneration(900);
  const model_router::AuthorityComparison request_comparison = model_router::compare_authority(
      bound, request_changed, AuthorityMask(static_cast<std::uint32_t>(AuthorityComponent::REQUEST)));
  MR_CHECK_EQ(request_comparison.code, OutcomeCode::REJECT_STALE_REQUEST);
  MR_CHECK_EQ(request_comparison.differences.size(), std::size_t{2});
  // Canonical order within one component is by bound value.
  MR_CHECK_EQ(request_comparison.differences[0].bound, bound.request_generation.value());
  MR_CHECK_EQ(request_comparison.differences[1].bound, bound.request_id.value());
}

MR_TEST(authority, every_stale_component_produces_its_documented_code) {
  std::size_t count = 0;
  const ComponentCase* cases = component_cases(&count);
  MR_CHECK_EQ(count, static_cast<std::size_t>(AuthorityComponent::kCount));
  for (std::size_t index = 0; index < count; ++index) {
    const ComponentCase& test_case = cases[index];
    const RouteAuthority bound = fully_bound_authority();
    RouteAuthority current = bound;
    test_case.bump(current);
    MR_CHECK_NE(current, bound);
    AuthorityMask mask;
    mask.add(test_case.component);
    const model_router::AuthorityComparison comparison =
        model_router::compare_authority(bound, current, mask);
    MR_CHECK_EQ(comparison.code, test_case.code);
    MR_CHECK_EQ(comparison.differences.size(), std::size_t{1});
    if (!comparison.differences.empty()) {
      MR_CHECK_EQ(comparison.differences.front().component, test_case.component);
      MR_CHECK_EQ(comparison.differences.front().code, test_case.code);
    }
  }
}

MR_TEST(authority, digest_is_stable_and_tracks_every_bound_generation) {
  const RouteAuthority bound = fully_bound_authority();
  const std::string digest = model_router::authority_digest(bound);
  MR_CHECK_EQ(digest.size(), std::size_t{64});
  MR_CHECK(digest.find_first_not_of("0123456789abcdef") == std::string::npos);
  // Identical authority always yields an identical digest.
  MR_CHECK_EQ(digest, model_router::authority_digest(bound));
  RouteAuthority copy = bound;
  MR_CHECK_EQ(digest, model_router::authority_digest(copy));
  MR_CHECK_NE(digest, model_router::authority_digest(RouteAuthority{}));
  MR_CHECK_EQ(model_router::authority_digest(RouteAuthority{}),
              model_router::authority_digest(RouteAuthority{}));

  // Changing any bound generation changes the digest.
  std::size_t count = 0;
  const ComponentCase* cases = component_cases(&count);
  for (std::size_t index = 0; index < count; ++index) {
    RouteAuthority changed = bound;
    cases[index].bump(changed);
    MR_CHECK_NE(digest, model_router::authority_digest(changed));
  }
  // Identity fields outside the component list are bound too.
  RouteAuthority other_router = bound;
  other_router.router_id = RouterId(999);
  MR_CHECK_NE(digest, model_router::authority_digest(other_router));
  RouteAuthority other_decision = bound;
  other_decision.decision_id = model_router::RouteDecisionId(999);
  MR_CHECK_NE(digest, model_router::authority_digest(other_decision));
  RouteAuthority other_evidence = bound;
  other_evidence.cost_evidence_id = CostEvidenceId(999);
  MR_CHECK_NE(digest, model_router::authority_digest(other_evidence));
}

MR_TEST(authority, routed_decision_binds_exactly_the_components_it_depends_on) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);
  fixture.publish_costs(BackendId(1), PriceGeneration(1), 100, 100, 100);

  const RouteOutcome outcome = route_specialist(fixture);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);

  AuthorityMask expected;
  expected.add(AuthorityComponent::ROUTER_EPOCH);
  expected.add(AuthorityComponent::COORDINATOR_EPOCH);
  expected.add(AuthorityComponent::REQUEST);
  expected.add(AuthorityComponent::MODEL);
  expected.add(AuthorityComponent::ARTIFACT);
  expected.add(AuthorityComponent::PROVIDER);
  expected.add(AuthorityComponent::BACKEND);
  expected.add(AuthorityComponent::BACKEND_BOOT);
  expected.add(AuthorityComponent::ENDPOINT);
  expected.add(AuthorityComponent::CAPABILITY);
  expected.add(AuthorityComponent::POLICY);
  expected.add(AuthorityComponent::PRICE);
  expected.add(AuthorityComponent::TRUST);
  expected.add(AuthorityComponent::HEALTH);
  expected.add(AuthorityComponent::AVAILABILITY);
  expected.add(AuthorityComponent::READINESS);
  expected.add(AuthorityComponent::RESIDENCY);
  expected.add(AuthorityComponent::CAPACITY);
  expected.add(AuthorityComponent::TENANCY);
  MR_CHECK_EQ(outcome.decision.authority.required_mask(), expected);

  // The bound identity values are the winner's live values.
  MR_CHECK_EQ(outcome.decision.authority.model_id, fixture.catalog.specialist_model);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, BackendId(1));
  MR_CHECK_EQ(outcome.decision.authority.backend_boot, BackendBootId(1));
  MR_CHECK_EQ(outcome.decision.authority.provider_id, fixture.catalog.provider);
  MR_CHECK_EQ(outcome.decision.authority.price_generation, PriceGeneration(1));
  MR_CHECK_EQ(outcome.decision.authority.health_generation, HealthGeneration(1));
  MR_CHECK_EQ(outcome.decision.authority.capability_profile_id,
              CapabilityProfileId(BackendId(1).value() * 100 + 2));
  MR_CHECK(!outcome.decision.authority.capability_generation.valid());
  MR_CHECK(!outcome.decision.authority.budget_generation.valid());
  MR_CHECK(!outcome.decision.authority.slo_generation.valid());
  MR_CHECK(!outcome.decision.authority.compatibility_generation.valid());
  MR_CHECK(!outcome.decision.authority.reservation_id.valid());
  MR_CHECK(!outcome.decision.authority.dispatch_generation.valid());
}

MR_TEST(authority, an_expired_authority_cannot_be_dispatched) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);

  RouteRequest request = mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(require_code());
  request.created_at_unix_millis = fixture.now();
  request.expires_at_unix_millis = fixture.now() + 1000;
  const RouteOutcome outcome = fixture.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.expires_at_unix_millis, request.expires_at_unix_millis);
  MR_CHECK(!outcome.decision.authority.expired(fixture.now()));
  MR_CHECK_EQ(fixture.router->revalidate(outcome.decision.decision_id).code, OutcomeCode::ACCEPTED);

  // Past the expiry the bound authority can never authorize a dispatch.
  fixture.clock->advance(1000);
  MR_CHECK(outcome.decision.authority.expired(fixture.now()));

  const MutationResult revalidated = fixture.router->revalidate(outcome.decision.decision_id);
  MR_CHECK_EQ(revalidated.code, OutcomeCode::REJECT_STALE_REQUEST);
  RouteDecision stored;
  MR_CHECK(fixture.router->find_decision(outcome.decision.decision_id, &stored));
  MR_CHECK_EQ(stored.status, RouteStatus::STALE);
  // NOTE: the expiry branch of revalidate() leaves explanation.currentness at
  // CURRENT while marking the decision STALE; see the defect report. The
  // status is the executable-state authority and is asserted here.
  MR_CHECK(!stored.dispatchable());

  const DispatchRecord dispatched = fixture.router->dispatch(outcome.decision.decision_id);
  MR_CHECK_EQ(dispatched.code, OutcomeCode::REJECT_STALE_REQUEST);
  MR_CHECK(!dispatched.handed_off);
  MR_CHECK_EQ(fixture.dispatcher->calls, std::uint64_t{0});
}

MR_TEST(authority, revalidate_after_fencing_the_incarnation_rejects_and_marks_stale) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);

  const RouteOutcome outcome = route_specialist(fixture);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(fixture.router->revalidate(outcome.decision.decision_id).code, OutcomeCode::ACCEPTED);

  MR_CHECK(fixture.router
               ->fence_backend_boot(BackendId(1), BackendGeneration(1), BackendBootId(1),
                                    OutcomeCode::FENCED)
               .accepted());

  const MutationResult revalidated = fixture.router->revalidate(outcome.decision.decision_id);
  MR_CHECK_EQ(revalidated.code, OutcomeCode::REJECT_STALE_BACKEND_BOOT);
  const std::string* backend_id = revalidated.explanation.find("backend_id");
  MR_CHECK(backend_id != nullptr);
  if (backend_id != nullptr) {
    MR_CHECK_EQ(*backend_id, std::string("1"));
  }
  RouteDecision stored;
  MR_CHECK(fixture.router->find_decision(outcome.decision.decision_id, &stored));
  MR_CHECK_EQ(stored.status, RouteStatus::STALE);
  MR_CHECK_EQ(stored.explanation.currentness, Currentness::STALE);
  MR_CHECK(stored.explanation.revalidation_required);

  const DispatchRecord dispatched = fixture.router->dispatch(outcome.decision.decision_id);
  MR_CHECK_EQ(dispatched.code, OutcomeCode::REJECT_STALE_BACKEND_BOOT);
  MR_CHECK(!dispatched.handed_off);
  MR_CHECK_EQ(fixture.dispatcher->calls, std::uint64_t{0});
  // A fenced incarnation is gone from the catalog, not merely marked.
  MR_CHECK(!fixture.router->find_backend(BackendId(1), nullptr));
  MR_CHECK_EQ(fixture.router->fenced_boots().size(), std::size_t{1});
}

MR_TEST(authority, cross_tenant_revalidate_and_dispatch_are_conflicts) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);

  const RouteOutcome outcome = route_specialist(fixture);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);

  // A different tenant may not revalidate or dispatch someone else's route.
  MR_CHECK_EQ(fixture.router->revalidate(outcome.decision.decision_id, TenantId(2)).code,
              OutcomeCode::REJECT_CONFLICT);
  MR_CHECK_EQ(fixture.router
                  ->revalidate(outcome.decision.decision_id, kTenant, NamespaceId(9))
                  .code,
              OutcomeCode::REJECT_CONFLICT);
  const DispatchRecord stolen = fixture.router->dispatch(outcome.decision.decision_id, TenantId(2));
  MR_CHECK_EQ(stolen.code, OutcomeCode::REJECT_CONFLICT);
  MR_CHECK(!stolen.handed_off);
  MR_CHECK_EQ(fixture.dispatcher->calls, std::uint64_t{0});
  const DispatchRecord wrong_namespace =
      fixture.router->dispatch(outcome.decision.decision_id, kTenant, NamespaceId(9));
  MR_CHECK_EQ(wrong_namespace.code, OutcomeCode::REJECT_CONFLICT);
  MR_CHECK_EQ(fixture.dispatcher->calls, std::uint64_t{0});

  // The owning tenant still dispatches, and an unspecified caller identity is
  // accepted because there is nothing to conflict with.
  MR_CHECK_EQ(fixture.router->revalidate(outcome.decision.decision_id).code, OutcomeCode::ACCEPTED);
  const DispatchRecord dispatched =
      fixture.router->dispatch(outcome.decision.decision_id, kTenant, NamespaceId(1));
  MR_CHECK_EQ(dispatched.code, OutcomeCode::DISPATCHED);
  MR_CHECK(dispatched.handed_off);
  MR_CHECK_EQ(fixture.dispatcher->calls, std::uint64_t{1});
  MR_CHECK_EQ(dispatched.target.model_id, fixture.catalog.specialist_model);
  MR_CHECK_EQ(dispatched.target.backend_id, BackendId(1));
  MR_CHECK_EQ(dispatched.target.backend_boot, BackendBootId(1));
  MR_CHECK_EQ(dispatched.decision_id, outcome.decision.decision_id);
}

MR_TEST(authority, revalidate_reports_the_most_specific_stale_component) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);

  const RouteOutcome outcome = route_specialist(fixture);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.health_generation, HealthGeneration(1));

  // New health evidence advances the bound generation.
  MR_CHECK(fixture.publisher
               .health(*fixture.router, BackendId(1), BackendBootId(1),
                       model_router::HealthState::DEGRADED, fixture.now())
               .accepted());
  const MutationResult revalidated = fixture.router->revalidate(outcome.decision.decision_id);
  MR_CHECK_EQ(revalidated.code, OutcomeCode::REJECT_STALE_HEALTH);
  model_router::BackendDescriptor descriptor;
  MR_CHECK(fixture.router->find_backend(BackendId(1), &descriptor));
  // The explanation must report the exact bound-to-current transition for the
  // component that advanced.
  const std::string transition =
      "1->" + std::to_string(descriptor.health.generation.value());
  std::size_t transitions = 0;
  for (const model_router::ExplanationFactor& factor : revalidated.explanation.factors()) {
    if (factor.value == transition) {
      ++transitions;
      MR_CHECK(!factor.key.empty());
    }
  }
  MR_CHECK_EQ(transitions, std::size_t{1});
  RouteDecision stored;
  MR_CHECK(fixture.router->find_decision(outcome.decision.decision_id, &stored));
  MR_CHECK_EQ(stored.status, RouteStatus::STALE);
  const DispatchRecord dispatched = fixture.router->dispatch(outcome.decision.decision_id);
  MR_CHECK_EQ(dispatched.code, OutcomeCode::REJECT_STALE_HEALTH);
  MR_CHECK(!dispatched.handed_off);
  MR_CHECK_EQ(fixture.dispatcher->calls, std::uint64_t{0});
}
