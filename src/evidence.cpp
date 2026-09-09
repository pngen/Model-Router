// Model Router - external evidence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/evidence.hpp"

#include <algorithm>

namespace model_router {
namespace {

constexpr std::size_t kMaxLocalityKeyBytes = 128;

[[nodiscard]] bool is_locality_character(char ch) noexcept {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
         ch == '.' || ch == '-' || ch == '_' || ch == '/';
}

}  // namespace

std::string_view to_string(TrustDomain domain) noexcept {
  switch (domain) {
    case TrustDomain::UNKNOWN:
      return "UNKNOWN";
    case TrustDomain::PUBLIC_REMOTE:
      return "PUBLIC_REMOTE";
    case TrustDomain::TRUSTED_REMOTE:
      return "TRUSTED_REMOTE";
    case TrustDomain::PRIVATE_NETWORK:
      return "PRIVATE_NETWORK";
    case TrustDomain::LOCAL:
      return "LOCAL";
    default:
      return "INVALID";
  }
}

std::uint8_t trust_rank(TrustDomain domain) noexcept {
  switch (domain) {
    case TrustDomain::UNKNOWN:
      return 0;
    case TrustDomain::PUBLIC_REMOTE:
      return 1;
    case TrustDomain::TRUSTED_REMOTE:
      return 2;
    case TrustDomain::PRIVATE_NETWORK:
      return 3;
    case TrustDomain::LOCAL:
      return 4;
    default:
      return 0;
  }
}

LocalityKey::LocalityKey(std::string value) : value_(std::move(value)) {}

std::string LocalityKey::validate(std::string_view value) {
  if (value.empty()) {
    return "locality key is empty";
  }
  if (value.size() > kMaxLocalityKeyBytes) {
    return "locality key exceeds " + std::to_string(kMaxLocalityKeyBytes) + " bytes";
  }
  if (value.front() == '/' || value.back() == '/') {
    return "locality key has a leading or trailing separator";
  }
  bool previous_separator = false;
  for (const char ch : value) {
    if (!is_locality_character(ch)) {
      return "locality key contains an invalid character";
    }
    if (ch == '/') {
      if (previous_separator) {
        return "locality key contains an empty segment";
      }
      previous_separator = true;
    } else {
      previous_separator = false;
    }
  }
  return {};
}

std::string LocalityKey::canonicalize(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char raw : value) {
    char ch = raw;
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (ch == '_') {
      ch = '-';
    }
    out.push_back(ch);
  }
  std::string collapsed;
  collapsed.reserve(out.size());
  bool previous_separator = false;
  for (const char ch : out) {
    if (ch == '/') {
      if (collapsed.empty() || previous_separator) {
        continue;
      }
      previous_separator = true;
    } else {
      previous_separator = false;
    }
    collapsed.push_back(ch);
  }
  while (!collapsed.empty() && collapsed.back() == '/') {
    collapsed.pop_back();
  }
  if (!validate(collapsed).empty()) {
    return {};
  }
  return collapsed;
}

std::string_view to_string(HealthState state) noexcept {
  switch (state) {
    case HealthState::UNKNOWN:
      return "UNKNOWN";
    case HealthState::HEALTHY:
      return "HEALTHY";
    case HealthState::DEGRADED:
      return "DEGRADED";
    case HealthState::UNHEALTHY:
      return "UNHEALTHY";
    case HealthState::DRAINING:
      return "DRAINING";
    case HealthState::RETIRED:
      return "RETIRED";
    default:
      return "INVALID";
  }
}

std::string_view to_string(AvailabilityState state) noexcept {
  switch (state) {
    case AvailabilityState::UNKNOWN:
      return "UNKNOWN";
    case AvailabilityState::AVAILABLE:
      return "AVAILABLE";
    case AvailabilityState::UNAVAILABLE:
      return "UNAVAILABLE";
    default:
      return "INVALID";
  }
}

std::string_view to_string(ReadinessState state) noexcept {
  switch (state) {
    case ReadinessState::UNKNOWN:
      return "UNKNOWN";
    case ReadinessState::READY:
      return "READY";
    case ReadinessState::WARMING:
      return "WARMING";
    case ReadinessState::NOT_READY:
      return "NOT_READY";
    default:
      return "INVALID";
  }
}

std::string_view to_string(ResidencyState state) noexcept {
  switch (state) {
    case ResidencyState::UNKNOWN:
      return "UNKNOWN";
    case ResidencyState::RESIDENT:
      return "RESIDENT";
    case ResidencyState::LOADING:
      return "LOADING";
    case ResidencyState::NOT_RESIDENT:
      return "NOT_RESIDENT";
    case ResidencyState::EVICTING:
      return "EVICTING";
    default:
      return "INVALID";
  }
}

std::string_view to_string(CapacityState state) noexcept {
  switch (state) {
    case CapacityState::UNKNOWN:
      return "UNKNOWN";
    case CapacityState::AVAILABLE:
      return "AVAILABLE";
    case CapacityState::SATURATED:
      return "SATURATED";
    default:
      return "INVALID";
  }
}

std::string_view to_string(CostKnownness knownness) noexcept {
  switch (knownness) {
    case CostKnownness::UNKNOWN:
      return "UNKNOWN";
    case CostKnownness::ESTIMATED:
      return "ESTIMATED";
    case CostKnownness::QUOTED:
      return "QUOTED";
    case CostKnownness::CONTRACTED:
      return "CONTRACTED";
    default:
      return "INVALID";
  }
}

std::string_view to_string(BudgetVerdict verdict) noexcept {
  switch (verdict) {
    case BudgetVerdict::UNKNOWN:
      return "UNKNOWN";
    case BudgetVerdict::ALLOWED:
      return "ALLOWED";
    case BudgetVerdict::DENIED:
      return "DENIED";
    default:
      return "INVALID";
  }
}

std::string_view to_string(SloVerdict verdict) noexcept {
  switch (verdict) {
    case SloVerdict::UNKNOWN:
      return "UNKNOWN";
    case SloVerdict::FEASIBLE:
      return "FEASIBLE";
    case SloVerdict::INFEASIBLE:
      return "INFEASIBLE";
    default:
      return "INVALID";
  }
}

std::string_view to_string(CompatibilityVerdict verdict) noexcept {
  switch (verdict) {
    case CompatibilityVerdict::UNKNOWN:
      return "UNKNOWN";
    case CompatibilityVerdict::COMPATIBLE:
      return "COMPATIBLE";
    case CompatibilityVerdict::INCOMPATIBLE:
      return "INCOMPATIBLE";
    default:
      return "INVALID";
  }
}

std::string_view to_string(FallbackPolicy policy) noexcept {
  switch (policy) {
    case FallbackPolicy::FORBIDDEN:
      return "FORBIDDEN";
    case FallbackPolicy::EXPLICIT_ONLY:
      return "EXPLICIT_ONLY";
    case FallbackPolicy::PERMITTED:
      return "PERMITTED";
    default:
      return "INVALID";
  }
}

void PolicySnapshot::canonicalize() {
  const auto sort_ids = [](auto& list) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
  };
  sort_ids(allowed_models);
  sort_ids(denied_models);
  sort_ids(allowed_providers);
  sort_ids(denied_providers);
  sort_ids(allowed_backends);
  sort_ids(denied_backends);
  sort_ids(preferred_model_families);
  sort_ids(denied_model_families);
  sort_ids(bound_tenants);
  const auto sort_localities = [](auto& list) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
  };
  sort_localities(allowed_localities);
  sort_localities(denied_localities);
  std::sort(fallback_permitted_outcomes.begin(), fallback_permitted_outcomes.end());
  fallback_permitted_outcomes.erase(
      std::unique(fallback_permitted_outcomes.begin(), fallback_permitted_outcomes.end()),
      fallback_permitted_outcomes.end());
}

std::string PolicySnapshot::validate() const {
  if (!generation.valid()) {
    return "policy generation is not valid";
  }
  if (minimum_trust != TrustDomain::UNKNOWN && trust_rank(minimum_trust) == 0) {
    return "policy minimum trust domain is not a known domain";
  }
  if (local_only && deny_public_remote) {
    // Consistent: local-only already excludes public remote. No violation.
  }
  if (maximum_request_cost.micros < 0) {
    return "policy maximum request cost is negative";
  }
  if (maximum_request_cost.micros > 0 && !maximum_request_cost.unit.valid()) {
    return "policy maximum request cost has no unit identity";
  }
  for (const ModelId model : allowed_models) {
    if (!model.valid()) {
      return "policy allowlist contains the invalid model identity";
    }
  }
  for (const ModelId model : denied_models) {
    if (!model.valid()) {
      return "policy denylist contains the invalid model identity";
    }
  }
  if (max_provider_concentration_ppm > 10000u) {
    return "policy max provider concentration exceeds 10000 parts per 10000";
  }
  if (fallback_policy == FallbackPolicy::FORBIDDEN && max_fallback_depth != 0) {
    return "policy forbids fallback but permits a nonzero fallback depth";
  }
  return {};
}

}  // namespace model_router
