// Model Router - route authority binding and revalidation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_AUTHORITY_HPP
#define MODEL_ROUTER_AUTHORITY_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "model_router/clock.hpp"
#include "model_router/ids.hpp"
#include "model_router/outcome.hpp"

namespace model_router {

/// A component of route authority. A route is executable only while every
/// required component remains current.
enum class AuthorityComponent : std::uint32_t {
  NONE = 0,
  ROUTER_EPOCH = 1u << 0,
  COORDINATOR_EPOCH = 1u << 1,
  REQUEST = 1u << 2,
  MODEL = 1u << 3,
  ARTIFACT = 1u << 4,
  PROVIDER = 1u << 5,
  BACKEND = 1u << 6,
  BACKEND_BOOT = 1u << 7,
  ENDPOINT = 1u << 8,
  CAPABILITY = 1u << 9,
  POLICY = 1u << 10,
  BUDGET = 1u << 11,
  PRICE = 1u << 12,
  SLO = 1u << 13,
  COMPATIBILITY = 1u << 14,
  TRUST = 1u << 15,
  HEALTH = 1u << 16,
  AVAILABILITY = 1u << 17,
  READINESS = 1u << 18,
  RESIDENCY = 1u << 19,
  CAPACITY = 1u << 20,
  RESERVATION = 1u << 21,
  TENANCY = 1u << 22,
  DISPATCH = 1u << 23,
  kCount = 24
};

/// Bit set of authority components.
class AuthorityMask {
 public:
  constexpr AuthorityMask() noexcept = default;
  constexpr explicit AuthorityMask(std::uint32_t bits) noexcept : bits_(bits) {}

  [[nodiscard]] constexpr std::uint32_t bits() const noexcept { return bits_; }
  [[nodiscard]] constexpr bool contains(AuthorityComponent component) const noexcept {
    return (bits_ & static_cast<std::uint32_t>(component)) != 0;
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
  constexpr void add(AuthorityComponent component) noexcept {
    bits_ |= static_cast<std::uint32_t>(component);
  }
  constexpr void remove(AuthorityComponent component) noexcept {
    bits_ &= ~static_cast<std::uint32_t>(component);
  }
  [[nodiscard]] static constexpr AuthorityMask all() noexcept {
    return AuthorityMask((1u << static_cast<std::uint32_t>(AuthorityComponent::kCount)) - 1u);
  }

  friend constexpr bool operator==(AuthorityMask, AuthorityMask) noexcept = default;

 private:
  std::uint32_t bits_{0};
};

[[nodiscard]] std::string_view to_string(AuthorityComponent component) noexcept;

/// The complete authority binding of a route decision. Every field is the value
/// that was current when the decision was made. Nothing here is a promise that
/// the value is still current at dispatch time.
struct RouteAuthority {
  RouterId router_id{};
  RouterEpoch router_epoch{};
  CoordinatorEpoch coordinator_epoch{};

  RouteRequestId request_id{};
  RouteRequestGeneration request_generation{};

  RouteDecisionId decision_id{};
  RouteDecisionGeneration decision_generation{};

  ModelId model_id{};
  ModelGeneration model_generation{};
  ArtifactGeneration artifact_generation{};

  ProviderId provider_id{};
  ProviderGeneration provider_generation{};

  BackendId backend_id{};
  BackendGeneration backend_generation{};
  BackendBootId backend_boot{};
  BackendRegistrationGeneration backend_registration_generation{};

  EndpointId endpoint_id{};
  EndpointGeneration endpoint_generation{};

  CapabilityProfileId capability_profile_id{};
  CapabilityGeneration capability_generation{};

  PolicyId policy_id{};
  PolicyGeneration policy_generation{};

  BudgetId budget_id{};
  BudgetGeneration budget_generation{};

  CostEvidenceId cost_evidence_id{};
  PriceGeneration price_generation{};

  SLOId slo_id{};
  SLOGeneration slo_generation{};

  CompatibilityProfileId compatibility_profile_id{};
  CompatibilityGeneration compatibility_generation{};

  TrustProfileId trust_profile_id{};
  TrustGeneration trust_generation{};

  HealthGeneration health_generation{};
  AvailabilityGeneration availability_generation{};
  ReadinessGeneration readiness_generation{};
  ResidencyGeneration residency_generation{};
  CapacityGeneration capacity_generation{};

  ReservationId reservation_id{};
  ReservationGeneration reservation_generation{};

  TenantId tenant{};
  NamespaceId name_space{};

  DispatchGeneration dispatch_generation{};

  UnixMillis issued_at_unix_millis{0};
  UnixMillis expires_at_unix_millis{kNoExpiry};

  /// The components this route actually depends on. A component absent from the
  /// mask is not revalidated and never blocks dispatch.
  [[nodiscard]] AuthorityMask required_mask() const noexcept;

  [[nodiscard]] bool expired(UnixMillis now) const noexcept {
    return expires_at_unix_millis != kNoExpiry && now >= expires_at_unix_millis;
  }

  friend bool operator==(const RouteAuthority&, const RouteAuthority&) = default;
};

/// One component whose bound value no longer matches current authority.
struct AuthorityDifference {
  AuthorityComponent component{AuthorityComponent::NONE};
  std::uint64_t bound{0};
  std::uint64_t current{0};
  OutcomeCode code{OutcomeCode::REVALIDATION_REQUIRED};

  [[nodiscard]] bool canonical_less(const AuthorityDifference& other) const noexcept;
  friend bool operator==(const AuthorityDifference&, const AuthorityDifference&) = default;
};

/// Result of comparing a bound route authority against currently observed
/// authority. Differences are returned in canonical component order.
struct AuthorityComparison {
  OutcomeCode code{OutcomeCode::ACCEPTED};
  std::vector<AuthorityDifference> differences;

  [[nodiscard]] bool current() const noexcept { return differences.empty(); }
};

/// Compares a bound authority against current authority for the components in
/// p required. Returns ACCEPTED when every required component matches exactly,
/// otherwise the most specific stale/reject outcome with all differences listed.
[[nodiscard]] AuthorityComparison compare_authority(const RouteAuthority& bound,
                                                    const RouteAuthority& current,
                                                    AuthorityMask required) noexcept;

/// Canonical SHA-256-based digest of the authority binding, used to bind a
/// route decision to its exact evidence generations.
[[nodiscard]] std::string authority_digest(const RouteAuthority& authority);

/// Currentness of a route plan or snapshot.
enum class Currentness : std::uint8_t {
  /// Every required component matches current authority.
  CURRENT = 0,
  /// A required component advanced; the route may not be dispatched.
  STALE = 1,
  /// Authority must be re-observed from adapters before a verdict is possible.
  REVALIDATION_REQUIRED = 2,
  /// Rebuilt from durable state; dynamic evidence is not current.
  RECONSTRUCTED = 3,
  kCount
};

[[nodiscard]] std::string_view to_string(Currentness currentness) noexcept;

}  // namespace model_router

#endif  // MODEL_ROUTER_AUTHORITY_HPP
