// Model Router - strongly typed identities, generations, and epochs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_IDS_HPP
#define MODEL_ROUTER_IDS_HPP

#include <compare>
#include <cstdint>
#include <functional>
#include <string>

namespace model_router {

/// Identity tag types. Each tag produces a distinct EntityId/Generation type so
/// that identities with different authority semantics can never be interchanged
/// by accident. A raw string is never a substitute where authority matters.
struct RouterTag {};
struct RouterBootTag {};
struct RouterEpochTag {};
struct CoordinatorEpochTag {};

struct RouteRequestTag {};
struct RouteDecisionTag {};
struct RoutePlanTag {};
struct DispatchTag {};

struct ModelTag {};
struct ModelVersionTag {};
struct ArtifactTag {};
struct ModelFamilyTag {};

struct ProviderTag {};
struct BackendTag {};
struct BackendBootTag {};
struct BackendRegistrationTag {};
struct EndpointTag {};
struct SessionTag {};

struct CapabilityTag {};
struct CapabilityProfileTag {};
struct HealthTag {};
struct AvailabilityTag {};
struct ReadinessTag {};
struct ResidencyTag {};
struct CapacityTag {};
struct ReservationTag {};

struct PolicyTag {};
struct BudgetTag {};
struct CostEvidenceTag {};
struct PriceTag {};
struct SloTag {};
struct CompatibilityTag {};
struct TrustTag {};

struct TenantTag {};
struct NamespaceTag {};
struct CorrelationTag {};

/// A durable entity identity. Value 0 is the invalid/absent identity.
template <class Tag>
class EntityId {
 public:
  using tag = Tag;

  constexpr EntityId() noexcept = default;
  constexpr explicit EntityId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  explicit constexpr operator bool() const noexcept { return valid(); }

  friend constexpr bool operator==(const EntityId&, const EntityId&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const EntityId&,
                                                    const EntityId&) noexcept = default;

 private:
  std::uint64_t value_{0};
};

/// A monotonically increasing generation/incarnation/epoch number.
/// Value 0 means "unknown / never issued". Generations only move forward, and
/// next() saturates instead of wrapping into a previously valid value.
template <class Tag>
class Generation {
 public:
  using tag = Tag;

  constexpr Generation() noexcept = default;
  constexpr explicit Generation(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  explicit constexpr operator bool() const noexcept { return valid(); }

  [[nodiscard]] constexpr Generation next() const noexcept {
    return Generation(value_ == kMax ? kMax : value_ + 1);
  }

  static constexpr std::uint64_t kMax = 0xFFFFFFFFFFFFFFFFull;

  friend constexpr bool operator==(const Generation&, const Generation&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const Generation&,
                                                    const Generation&) noexcept = default;

 private:
  std::uint64_t value_{0};
};

// --- Router identity and authority epochs ---------------------------------

using RouterId = EntityId<RouterTag>;
using RouterGeneration = Generation<RouterTag>;
using RouterBootId = Generation<RouterBootTag>;
/// Advanced on every restart. Old-epoch traffic is never accepted.
using RouterEpoch = Generation<RouterEpochTag>;
using CoordinatorEpoch = Generation<CoordinatorEpochTag>;

// --- Routing pipeline identity --------------------------------------------

using RouteRequestId = EntityId<RouteRequestTag>;
using RouteRequestGeneration = Generation<RouteRequestTag>;

using RouteDecisionId = EntityId<RouteDecisionTag>;
using RouteDecisionGeneration = Generation<RouteDecisionTag>;

using RoutePlanId = EntityId<RoutePlanTag>;
using RoutePlanGeneration = Generation<RoutePlanTag>;

using DispatchId = EntityId<DispatchTag>;
using DispatchGeneration = Generation<DispatchTag>;

// --- Model identity (separate from serving backend identity) ---------------

using ModelId = EntityId<ModelTag>;
using ModelGeneration = Generation<ModelTag>;
using ModelVersionId = EntityId<ModelVersionTag>;
using ArtifactGeneration = Generation<ArtifactTag>;
using ModelFamilyId = EntityId<ModelFamilyTag>;

// --- Provider / backend / endpoint / process incarnation -------------------

using ProviderId = EntityId<ProviderTag>;
using ProviderGeneration = Generation<ProviderTag>;

using BackendId = EntityId<BackendTag>;
using BackendGeneration = Generation<BackendTag>;
/// Process/service incarnation. Independent per BackendId: restarting one
/// backend never invalidates another backend's incarnation.
using BackendBootId = Generation<BackendBootTag>;
using BackendRegistrationGeneration = Generation<BackendRegistrationTag>;

using EndpointId = EntityId<EndpointTag>;
using EndpointGeneration = Generation<EndpointTag>;

using SessionId = EntityId<SessionTag>;

// --- Evidence identity ----------------------------------------------------

using CapabilityProfileId = EntityId<CapabilityProfileTag>;
using CapabilityGeneration = Generation<CapabilityTag>;

using HealthGeneration = Generation<HealthTag>;
using AvailabilityGeneration = Generation<AvailabilityTag>;
using ReadinessGeneration = Generation<ReadinessTag>;
using ResidencyGeneration = Generation<ResidencyTag>;
using CapacityGeneration = Generation<CapacityTag>;
using ReservationId = EntityId<ReservationTag>;
using ReservationGeneration = Generation<ReservationTag>;

using PolicyId = EntityId<PolicyTag>;
using PolicyGeneration = Generation<PolicyTag>;

using BudgetId = EntityId<BudgetTag>;
using BudgetGeneration = Generation<BudgetTag>;

using CostEvidenceId = EntityId<CostEvidenceTag>;
using PriceGeneration = Generation<PriceTag>;

using SLOId = EntityId<SloTag>;
using SLOGeneration = Generation<SloTag>;

using CompatibilityProfileId = EntityId<CompatibilityTag>;
using CompatibilityGeneration = Generation<CompatibilityTag>;

using TrustProfileId = EntityId<TrustTag>;
using TrustGeneration = Generation<TrustTag>;

// --- Tenant isolation -----------------------------------------------------

using TenantId = EntityId<TenantTag>;
using NamespaceId = EntityId<NamespaceTag>;

using CorrelationId = EntityId<CorrelationTag>;

/// Process-wide monotonic identity source. Identities are never reused and never
/// wrap: reaching the ceiling is a hard failure rather than silent aliasing.
class IdAllocator {
 public:
  IdAllocator() = delete;

  [[nodiscard]] static std::uint64_t next() noexcept;
  /// Seeds the allocator above a value observed in durable state, so that a
  /// restarted process cannot reissue identities that were already persisted.
  static void observe(std::uint64_t highest_seen) noexcept;
  [[nodiscard]] static std::uint64_t highest_issued() noexcept;
};

template <class Tag>
[[nodiscard]] EntityId<Tag> allocate_id() noexcept {
  return EntityId<Tag>(IdAllocator::next());
}

template <class Tag>
[[nodiscard]] std::string to_string(EntityId<Tag> id) {
  return id.valid() ? std::to_string(id.value()) : std::string("none");
}

template <class Tag>
[[nodiscard]] std::string to_string(Generation<Tag> generation) {
  return generation.valid() ? std::to_string(generation.value()) : std::string("none");
}

}  // namespace model_router

namespace std {

template <class Tag>
struct hash<model_router::EntityId<Tag>> {
  [[nodiscard]] size_t operator()(model_router::EntityId<Tag> id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};

template <class Tag>
struct hash<model_router::Generation<Tag>> {
  [[nodiscard]] size_t operator()(model_router::Generation<Tag> generation) const noexcept {
    return std::hash<std::uint64_t>{}(generation.value());
  }
};

}  // namespace std

#endif  // MODEL_ROUTER_IDS_HPP
