// Model Router - route authority binding and revalidation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/authority.hpp"

#include <algorithm>
#include <array>

#include "model_router/detail/sha256.hpp"

namespace model_router {
namespace {

/// Indexed by bit position: entry i is the component whose mask is (1u << i).
constexpr std::array<std::string_view, static_cast<std::size_t>(AuthorityComponent::kCount)>
    kComponentNames = {
        "router_epoch", "coordinator_epoch", "request",     "model",
        "artifact",     "provider",          "backend",     "backend_boot",
        "endpoint",     "capability",        "policy",      "budget",
        "price",        "slo",               "compatibility", "trust",
        "health",       "availability",      "readiness",   "residency",
        "capacity",     "reservation",       "tenancy",     "dispatch",
};

void record(std::vector<AuthorityDifference>* differences, AuthorityComponent component,
            std::uint64_t bound, std::uint64_t current, OutcomeCode code) {
  if (bound == current) {
    return;
  }
  AuthorityDifference difference;
  difference.component = component;
  difference.bound = bound;
  difference.current = current;
  difference.code = code;
  differences->push_back(difference);
}

}  // namespace

std::string_view to_string(AuthorityComponent component) noexcept {
  if (component == AuthorityComponent::NONE) {
    return "none";
  }
  const std::uint32_t bits = static_cast<std::uint32_t>(component);
  for (std::size_t index = 0; index < kComponentNames.size(); ++index) {
    if (bits == (1u << index)) {
      return kComponentNames[index];
    }
  }
  return "invalid";
}

AuthorityMask RouteAuthority::required_mask() const noexcept {
  AuthorityMask mask;
  mask.add(AuthorityComponent::ROUTER_EPOCH);
  mask.add(AuthorityComponent::COORDINATOR_EPOCH);
  mask.add(AuthorityComponent::REQUEST);
  mask.add(AuthorityComponent::MODEL);
  mask.add(AuthorityComponent::ARTIFACT);
  mask.add(AuthorityComponent::BACKEND);
  mask.add(AuthorityComponent::BACKEND_BOOT);
  mask.add(AuthorityComponent::ENDPOINT);
  mask.add(AuthorityComponent::CAPABILITY);
  mask.add(AuthorityComponent::POLICY);
  mask.add(AuthorityComponent::TRUST);
  mask.add(AuthorityComponent::HEALTH);
  mask.add(AuthorityComponent::AVAILABILITY);
  mask.add(AuthorityComponent::READINESS);
  mask.add(AuthorityComponent::TENANCY);
  if (provider_id.valid() || provider_generation.valid()) {
    mask.add(AuthorityComponent::PROVIDER);
  }
  if (budget_generation.valid()) {
    mask.add(AuthorityComponent::BUDGET);
  }
  if (price_generation.valid()) {
    mask.add(AuthorityComponent::PRICE);
  }
  if (slo_generation.valid()) {
    mask.add(AuthorityComponent::SLO);
  }
  if (compatibility_generation.valid()) {
    mask.add(AuthorityComponent::COMPATIBILITY);
  }
  if (residency_generation.valid()) {
    mask.add(AuthorityComponent::RESIDENCY);
  }
  if (capacity_generation.valid()) {
    mask.add(AuthorityComponent::CAPACITY);
  }
  if (reservation_id.valid() || reservation_generation.valid()) {
    mask.add(AuthorityComponent::RESERVATION);
  }
  if (dispatch_generation.valid()) {
    mask.add(AuthorityComponent::DISPATCH);
  }
  return mask;
}

bool AuthorityDifference::canonical_less(const AuthorityDifference& other) const noexcept {
  if (component != other.component) {
    return static_cast<std::uint32_t>(component) < static_cast<std::uint32_t>(other.component);
  }
  if (bound != other.bound) {
    return bound < other.bound;
  }
  return current < other.current;
}

AuthorityComparison compare_authority(const RouteAuthority& bound, const RouteAuthority& current,
                                     AuthorityMask required) noexcept {
  AuthorityComparison comparison;
  auto& differences = comparison.differences;

  const auto has = [required](AuthorityComponent component) { return required.contains(component); };

  if (has(AuthorityComponent::ROUTER_EPOCH)) {
    record(&differences, AuthorityComponent::ROUTER_EPOCH, bound.router_epoch.value(),
           current.router_epoch.value(), OutcomeCode::REJECT_STALE_ROUTER_EPOCH);
  }
  if (has(AuthorityComponent::COORDINATOR_EPOCH)) {
    record(&differences, AuthorityComponent::COORDINATOR_EPOCH, bound.coordinator_epoch.value(),
           current.coordinator_epoch.value(), OutcomeCode::REJECT_STALE_COORDINATOR_EPOCH);
  }
  if (has(AuthorityComponent::REQUEST)) {
    record(&differences, AuthorityComponent::REQUEST, bound.request_id.value(),
           current.request_id.value(), OutcomeCode::REJECT_STALE_REQUEST);
    record(&differences, AuthorityComponent::REQUEST, bound.request_generation.value(),
           current.request_generation.value(), OutcomeCode::REJECT_STALE_REQUEST);
  }
  if (has(AuthorityComponent::MODEL)) {
    record(&differences, AuthorityComponent::MODEL, bound.model_id.value(),
           current.model_id.value(), OutcomeCode::REJECT_STALE_MODEL);
    record(&differences, AuthorityComponent::MODEL, bound.model_generation.value(),
           current.model_generation.value(), OutcomeCode::REJECT_STALE_MODEL);
  }
  if (has(AuthorityComponent::ARTIFACT)) {
    record(&differences, AuthorityComponent::ARTIFACT, bound.artifact_generation.value(),
           current.artifact_generation.value(), OutcomeCode::REJECT_STALE_ARTIFACT);
  }
  if (has(AuthorityComponent::PROVIDER)) {
    record(&differences, AuthorityComponent::PROVIDER, bound.provider_id.value(),
           current.provider_id.value(), OutcomeCode::REJECT_STALE_BACKEND);
    record(&differences, AuthorityComponent::PROVIDER, bound.provider_generation.value(),
           current.provider_generation.value(), OutcomeCode::REJECT_STALE_BACKEND);
  }
  if (has(AuthorityComponent::BACKEND)) {
    record(&differences, AuthorityComponent::BACKEND, bound.backend_id.value(),
           current.backend_id.value(), OutcomeCode::REJECT_STALE_BACKEND);
    record(&differences, AuthorityComponent::BACKEND, bound.backend_generation.value(),
           current.backend_generation.value(), OutcomeCode::REJECT_STALE_BACKEND);
    record(&differences, AuthorityComponent::BACKEND, bound.backend_registration_generation.value(),
           current.backend_registration_generation.value(), OutcomeCode::REJECT_STALE_BACKEND);
  }
  if (has(AuthorityComponent::BACKEND_BOOT)) {
    record(&differences, AuthorityComponent::BACKEND_BOOT, bound.backend_boot.value(),
           current.backend_boot.value(), OutcomeCode::REJECT_STALE_BACKEND_BOOT);
  }
  if (has(AuthorityComponent::ENDPOINT)) {
    record(&differences, AuthorityComponent::ENDPOINT, bound.endpoint_id.value(),
           current.endpoint_id.value(), OutcomeCode::REJECT_STALE_ENDPOINT);
    record(&differences, AuthorityComponent::ENDPOINT, bound.endpoint_generation.value(),
           current.endpoint_generation.value(), OutcomeCode::REJECT_STALE_ENDPOINT);
  }
  if (has(AuthorityComponent::CAPABILITY)) {
    record(&differences, AuthorityComponent::CAPABILITY, bound.capability_profile_id.value(),
           current.capability_profile_id.value(), OutcomeCode::REJECT_STALE_CAPABILITY);
    record(&differences, AuthorityComponent::CAPABILITY, bound.capability_generation.value(),
           current.capability_generation.value(), OutcomeCode::REJECT_STALE_CAPABILITY);
  }
  if (has(AuthorityComponent::POLICY)) {
    record(&differences, AuthorityComponent::POLICY, bound.policy_id.value(),
           current.policy_id.value(), OutcomeCode::REJECT_STALE_POLICY);
    record(&differences, AuthorityComponent::POLICY, bound.policy_generation.value(),
           current.policy_generation.value(), OutcomeCode::REJECT_STALE_POLICY);
  }
  if (has(AuthorityComponent::BUDGET)) {
    record(&differences, AuthorityComponent::BUDGET, bound.budget_id.value(),
           current.budget_id.value(), OutcomeCode::REJECT_STALE_BUDGET);
    record(&differences, AuthorityComponent::BUDGET, bound.budget_generation.value(),
           current.budget_generation.value(), OutcomeCode::REJECT_STALE_BUDGET);
  }
  if (has(AuthorityComponent::PRICE)) {
    record(&differences, AuthorityComponent::PRICE, bound.cost_evidence_id.value(),
           current.cost_evidence_id.value(), OutcomeCode::REJECT_STALE_PRICE);
    record(&differences, AuthorityComponent::PRICE, bound.price_generation.value(),
           current.price_generation.value(), OutcomeCode::REJECT_STALE_PRICE);
  }
  if (has(AuthorityComponent::SLO)) {
    record(&differences, AuthorityComponent::SLO, bound.slo_id.value(), current.slo_id.value(),
           OutcomeCode::REJECT_STALE_SLO);
    record(&differences, AuthorityComponent::SLO, bound.slo_generation.value(),
           current.slo_generation.value(), OutcomeCode::REJECT_STALE_SLO);
  }
  if (has(AuthorityComponent::COMPATIBILITY)) {
    record(&differences, AuthorityComponent::COMPATIBILITY,
           bound.compatibility_profile_id.value(), current.compatibility_profile_id.value(),
           OutcomeCode::REJECT_STALE_COMPATIBILITY);
    record(&differences, AuthorityComponent::COMPATIBILITY, bound.compatibility_generation.value(),
           current.compatibility_generation.value(), OutcomeCode::REJECT_STALE_COMPATIBILITY);
  }
  if (has(AuthorityComponent::TRUST)) {
    record(&differences, AuthorityComponent::TRUST, bound.trust_profile_id.value(),
           current.trust_profile_id.value(), OutcomeCode::REJECT_STALE_TRUST);
    record(&differences, AuthorityComponent::TRUST, bound.trust_generation.value(),
           current.trust_generation.value(), OutcomeCode::REJECT_STALE_TRUST);
  }
  if (has(AuthorityComponent::HEALTH)) {
    record(&differences, AuthorityComponent::HEALTH, bound.health_generation.value(),
           current.health_generation.value(), OutcomeCode::REJECT_STALE_HEALTH);
  }
  if (has(AuthorityComponent::AVAILABILITY)) {
    record(&differences, AuthorityComponent::AVAILABILITY, bound.availability_generation.value(),
           current.availability_generation.value(), OutcomeCode::REJECT_STALE_AVAILABILITY);
  }
  if (has(AuthorityComponent::READINESS)) {
    record(&differences, AuthorityComponent::READINESS, bound.readiness_generation.value(),
           current.readiness_generation.value(), OutcomeCode::REJECT_STALE_READINESS);
  }
  if (has(AuthorityComponent::RESIDENCY)) {
    record(&differences, AuthorityComponent::RESIDENCY, bound.residency_generation.value(),
           current.residency_generation.value(), OutcomeCode::REJECT_STALE_RESIDENCY);
  }
  if (has(AuthorityComponent::CAPACITY)) {
    record(&differences, AuthorityComponent::CAPACITY, bound.capacity_generation.value(),
           current.capacity_generation.value(), OutcomeCode::REJECT_STALE_CAPACITY);
  }
  if (has(AuthorityComponent::RESERVATION)) {
    record(&differences, AuthorityComponent::RESERVATION, bound.reservation_id.value(),
           current.reservation_id.value(), OutcomeCode::REJECT_STALE_RESERVATION);
    record(&differences, AuthorityComponent::RESERVATION, bound.reservation_generation.value(),
           current.reservation_generation.value(), OutcomeCode::REJECT_STALE_RESERVATION);
  }
  if (has(AuthorityComponent::TENANCY)) {
    record(&differences, AuthorityComponent::TENANCY, bound.tenant.value(), current.tenant.value(),
           OutcomeCode::REJECT_CONFLICT);
    record(&differences, AuthorityComponent::TENANCY, bound.name_space.value(),
           current.name_space.value(), OutcomeCode::REJECT_CONFLICT);
  }
  if (has(AuthorityComponent::DISPATCH)) {
    record(&differences, AuthorityComponent::DISPATCH, bound.dispatch_generation.value(),
           current.dispatch_generation.value(), OutcomeCode::REJECT_STALE_REQUEST);
  }

  if (differences.empty()) {
    comparison.code = OutcomeCode::ACCEPTED;
    return comparison;
  }

  std::sort(differences.begin(), differences.end(),
            [](const AuthorityDifference& lhs, const AuthorityDifference& rhs) {
              return lhs.canonical_less(rhs);
            });

  // The reported code is the most specific stale/reject reason, chosen by
  // canonical component order so the result is deterministic.
  comparison.code = differences.front().code;
  return comparison;
}

std::string authority_digest(const RouteAuthority& authority) {
  std::string canonical;
  const auto append = [&canonical](std::uint64_t value) {
    canonical += std::to_string(value);
    canonical.push_back(',');
  };
  append(authority.router_id.value());
  append(authority.router_epoch.value());
  append(authority.coordinator_epoch.value());
  append(authority.request_id.value());
  append(authority.request_generation.value());
  append(authority.decision_id.value());
  append(authority.decision_generation.value());
  append(authority.model_id.value());
  append(authority.model_generation.value());
  append(authority.artifact_generation.value());
  append(authority.provider_id.value());
  append(authority.provider_generation.value());
  append(authority.backend_id.value());
  append(authority.backend_generation.value());
  append(authority.backend_boot.value());
  append(authority.backend_registration_generation.value());
  append(authority.endpoint_id.value());
  append(authority.endpoint_generation.value());
  append(authority.capability_profile_id.value());
  append(authority.capability_generation.value());
  append(authority.policy_id.value());
  append(authority.policy_generation.value());
  append(authority.budget_id.value());
  append(authority.budget_generation.value());
  append(authority.cost_evidence_id.value());
  append(authority.price_generation.value());
  append(authority.slo_id.value());
  append(authority.slo_generation.value());
  append(authority.compatibility_profile_id.value());
  append(authority.compatibility_generation.value());
  append(authority.trust_profile_id.value());
  append(authority.trust_generation.value());
  append(authority.health_generation.value());
  append(authority.availability_generation.value());
  append(authority.readiness_generation.value());
  append(authority.residency_generation.value());
  append(authority.capacity_generation.value());
  append(authority.reservation_id.value());
  append(authority.reservation_generation.value());
  append(authority.tenant.value());
  append(authority.name_space.value());
  append(authority.dispatch_generation.value());
  return detail::to_hex(detail::sha256(canonical));
}

std::string_view to_string(Currentness currentness) noexcept {
  switch (currentness) {
    case Currentness::CURRENT:
      return "CURRENT";
    case Currentness::STALE:
      return "STALE";
    case Currentness::REVALIDATION_REQUIRED:
      return "REVALIDATION_REQUIRED";
    case Currentness::RECONSTRUCTED:
      return "RECONSTRUCTED";
    default:
      return "INVALID";
  }
}

}  // namespace model_router
