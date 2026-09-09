// Model Router - external evidence consumed through narrow adapters.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_EVIDENCE_HPP
#define MODEL_ROUTER_EVIDENCE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "model_router/capability.hpp"
#include "model_router/clock.hpp"
#include "model_router/ids.hpp"
#include "model_router/outcome.hpp"

namespace model_router {

// ---------------------------------------------------------------------------
// Trust / locality
// ---------------------------------------------------------------------------

/// Explicit trust domain of a dispatch destination. Trust is never inferred from
/// a hostname, address, or provider name: it is supplied as generation-bound
/// evidence and may be UNKNOWN.
enum class TrustDomain : std::uint8_t {
  UNKNOWN = 0,
  PUBLIC_REMOTE = 1,
  TRUSTED_REMOTE = 2,
  PRIVATE_NETWORK = 3,
  LOCAL = 4,
  kCount
};

[[nodiscard]] std::string_view to_string(TrustDomain domain) noexcept;
/// Trust ordering used for hard constraints. UNKNOWN is the least trusted.
[[nodiscard]] std::uint8_t trust_rank(TrustDomain domain) noexcept;

/// A trust profile claim. Trust evidence is always generation-bound.
struct TrustEvidence {
  TrustProfileId profile_id{};
  TrustGeneration generation{};
  BackendId backend_id{};
  TrustDomain domain{TrustDomain::UNKNOWN};
  UnixMillis observed_at_unix_millis{0};
  UnixMillis expires_at_unix_millis{kNoExpiry};
  std::string source;

  [[nodiscard]] bool current(UnixMillis now) const noexcept {
    return generation.valid() && domain != TrustDomain::UNKNOWN &&
           !(expires_at_unix_millis != kNoExpiry && now >= expires_at_unix_millis);
  }
  friend bool operator==(const TrustEvidence&, const TrustEvidence&) = default;
};

/// A deployment/locality identity: a region or domain label used for data
/// residency and locality constraints. Canonical, bounded, validated.
class LocalityKey {
 public:
  LocalityKey() = default;
  explicit LocalityKey(std::string value);

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

  [[nodiscard]] static std::string validate(std::string_view value);
  [[nodiscard]] static std::string canonicalize(std::string_view value);

  friend bool operator==(const LocalityKey&, const LocalityKey&) = default;
  friend std::strong_ordering operator<=>(const LocalityKey&, const LocalityKey&) = default;

 private:
  std::string value_;
};

// ---------------------------------------------------------------------------
// Health / availability / readiness / residency / capacity
//
// These are five distinct facts. AVAILABLE is not HEALTHY, HEALTHY is not
// READY, READY is not CURRENT, CURRENT is not ELIGIBLE. Each is UNKNOWN until
// current evidence proves otherwise.
// ---------------------------------------------------------------------------

enum class HealthState : std::uint8_t {
  UNKNOWN = 0,
  HEALTHY = 1,
  DEGRADED = 2,
  UNHEALTHY = 3,
  DRAINING = 4,
  RETIRED = 5,
  kCount
};

enum class AvailabilityState : std::uint8_t {
  UNKNOWN = 0,
  AVAILABLE = 1,
  UNAVAILABLE = 2,
  kCount
};

enum class ReadinessState : std::uint8_t {
  UNKNOWN = 0,
  READY = 1,
  WARMING = 2,
  NOT_READY = 3,
  kCount
};

enum class ResidencyState : std::uint8_t {
  UNKNOWN = 0,
  RESIDENT = 1,
  LOADING = 2,
  NOT_RESIDENT = 3,
  EVICTING = 4,
  kCount
};

enum class CapacityState : std::uint8_t {
  UNKNOWN = 0,
  AVAILABLE = 1,
  SATURATED = 2,
  kCount
};

[[nodiscard]] std::string_view to_string(HealthState state) noexcept;
[[nodiscard]] std::string_view to_string(AvailabilityState state) noexcept;
[[nodiscard]] std::string_view to_string(ReadinessState state) noexcept;
[[nodiscard]] std::string_view to_string(ResidencyState state) noexcept;
[[nodiscard]] std::string_view to_string(CapacityState state) noexcept;

/// A generation-bound observation about one backend incarnation. The boot
/// binding is mandatory: evidence from a fenced or replaced BackendBootId must
/// never be applied to the current incarnation.
template <class StateType, class GenerationType>
struct BackendEvidence {
  StateType state{};
  GenerationType generation{};
  BackendId backend_id{};
  BackendGeneration backend_generation{};
  BackendBootId backend_boot{};
  EndpointGeneration endpoint_generation{};
  UnixMillis observed_at_unix_millis{0};
  UnixMillis expires_at_unix_millis{kNoExpiry};
  std::string source;
  std::string detail;

  [[nodiscard]] bool expired(UnixMillis now) const noexcept {
    return expires_at_unix_millis != kNoExpiry && now >= expires_at_unix_millis;
  }
  [[nodiscard]] bool current(UnixMillis now) const noexcept {
    return generation.valid() && backend_boot.valid() && !expired(now);
  }
  friend bool operator==(const BackendEvidence&, const BackendEvidence&) = default;
};

using HealthEvidence = BackendEvidence<HealthState, HealthGeneration>;
using AvailabilityEvidence = BackendEvidence<AvailabilityState, AvailabilityGeneration>;
using ReadinessEvidence = BackendEvidence<ReadinessState, ReadinessGeneration>;
using ResidencyEvidence = BackendEvidence<ResidencyState, ResidencyGeneration>;
using CapacityEvidence = BackendEvidence<CapacityState, CapacityGeneration>;

/// Capacity detail carried alongside capacity state. Values are absolute counts,
/// never inferred: an absent value is UNKNOWN, not zero headroom.
struct CapacityDetail {
  CapacityGeneration generation{};
  BackendId backend_id{};
  BackendBootId backend_boot{};
  /// Remaining concurrent slots. kUnknownCount means unknown.
  std::uint32_t available_slots{kUnknownCount};
  std::uint32_t total_slots{kUnknownCount};
  /// Queue depth in requests. kUnknownCount means unknown.
  std::uint32_t queue_depth{kUnknownCount};
  std::uint32_t max_queue_depth{kUnknownCount};
  UnixMillis observed_at_unix_millis{0};
  UnixMillis expires_at_unix_millis{kNoExpiry};

  static constexpr std::uint32_t kUnknownCount = 0xFFFFFFFFu;

  [[nodiscard]] bool known() const noexcept { return available_slots != kUnknownCount; }
  friend bool operator==(const CapacityDetail&, const CapacityDetail&) = default;
};

/// A reservation lease over backend capacity. A route that requires a
/// reservation is not dispatchable without a current one.
struct ReservationEvidence {
  ReservationId reservation_id{};
  ReservationGeneration generation{};
  BackendId backend_id{};
  BackendBootId backend_boot{};
  RouteRequestId request_id{};
  UnixMillis expires_at_unix_millis{kNoExpiry};

  [[nodiscard]] bool current(UnixMillis now) const noexcept {
    return reservation_id.valid() && generation.valid() && backend_boot.valid() &&
           !(expires_at_unix_millis != kNoExpiry && now >= expires_at_unix_millis);
  }
  friend bool operator==(const ReservationEvidence&, const ReservationEvidence&) = default;
};

// ---------------------------------------------------------------------------
// Latency / performance evidence
// ---------------------------------------------------------------------------

/// Observed latency statistics. These are observations, not guarantees: a
/// guarantee is expressed through SLO evidence. Values are integer microseconds;
/// kUnknownLatencyMicros means the observation is absent.
struct LatencyEvidence {
  static constexpr std::uint32_t kUnknownLatencyMicros = 0xFFFFFFFFu;

  HealthGeneration health_generation{};
  BackendId backend_id{};
  BackendBootId backend_boot{};
  std::uint32_t dispatch_micros{kUnknownLatencyMicros};
  std::uint32_t time_to_first_token_micros{kUnknownLatencyMicros};
  std::uint32_t completed_micros{kUnknownLatencyMicros};
  std::uint32_t tail_micros{kUnknownLatencyMicros};
  std::uint32_t queue_micros{kUnknownLatencyMicros};
  std::uint32_t warm_start_penalty_micros{kUnknownLatencyMicros};
  std::uint32_t cold_start_penalty_micros{kUnknownLatencyMicros};
  std::uint32_t sample_count{0};
  UnixMillis observed_at_unix_millis{0};
  UnixMillis expires_at_unix_millis{kNoExpiry};
  std::string source;

  [[nodiscard]] bool has_observation() const noexcept { return sample_count > 0; }
  [[nodiscard]] bool expired(UnixMillis now) const noexcept {
    return expires_at_unix_millis != kNoExpiry && now >= expires_at_unix_millis;
  }
  friend bool operator==(const LatencyEvidence&, const LatencyEvidence&) = default;
};

// ---------------------------------------------------------------------------
// Cost evidence
// ---------------------------------------------------------------------------

/// Identity of a cost unit. Two costs may only be compared numerically when
/// their units are identical; there is no implicit currency conversion.
struct CostUnit {
  std::string currency;
  std::string basis;

  [[nodiscard]] bool valid() const noexcept { return !currency.empty() && !basis.empty(); }
  [[nodiscard]] bool compatible_with(const CostUnit& other) const noexcept {
    return valid() && other.valid() && currency == other.currency && basis == other.basis;
  }
  friend bool operator==(const CostUnit&, const CostUnit&) = default;
};

/// Integer micro-units of a currency on a stated basis. Floating point is never
/// used for money, so comparisons and sums are exact.
struct Money {
  std::int64_t micros{0};
  CostUnit unit;

  friend bool operator==(const Money&, const Money&) = default;
};

/// How well a price is known. UNKNOWN price never becomes zero.
enum class CostKnownness : std::uint8_t {
  UNKNOWN = 0,
  ESTIMATED = 1,
  QUOTED = 2,
  CONTRACTED = 3,
  kCount
};

[[nodiscard]] std::string_view to_string(CostKnownness knownness) noexcept;

/// Price and cost estimate evidence supplied by a cost adapter. Model Router
/// never becomes a billing engine: it consumes this evidence as a routing factor
/// or hard constraint and records the generations needed to explain a route.
struct CostEvidence {
  CostEvidenceId evidence_id{};
  PriceGeneration price_generation{};
  BackendId backend_id{};
  ModelId model_id{};
  CostUnit unit{};
  std::int64_t input_micros_per_unit{0};
  std::int64_t output_micros_per_unit{0};
  std::int64_t request_minimum_micros{0};
  std::int64_t cache_read_micros_per_unit{0};
  std::int64_t cache_write_micros_per_unit{0};
  std::int64_t estimated_total_micros{0};
  CostKnownness knownness{CostKnownness::UNKNOWN};
  UnixMillis effective_at_unix_millis{0};
  UnixMillis expires_at_unix_millis{kNoExpiry};
  std::string source;

  [[nodiscard]] bool known() const noexcept { return knownness != CostKnownness::UNKNOWN; }
  [[nodiscard]] bool current(UnixMillis now) const noexcept {
    return known() && price_generation.valid() && unit.valid() &&
           !(expires_at_unix_millis != kNoExpiry && now >= expires_at_unix_millis);
  }
  friend bool operator==(const CostEvidence&, const CostEvidence&) = default;
};

// ---------------------------------------------------------------------------
// Budget evidence
// ---------------------------------------------------------------------------

enum class BudgetVerdict : std::uint8_t {
  UNKNOWN = 0,
  ALLOWED = 1,
  DENIED = 2,
  kCount
};

[[nodiscard]] std::string_view to_string(BudgetVerdict verdict) noexcept;

/// Budget feasibility supplied by the Cost Governor boundary. Model Router does
/// not maintain the accounting model; it consumes the verdict and identity.
struct BudgetSnapshot {
  BudgetId budget_id{};
  BudgetGeneration generation{};
  BudgetVerdict verdict{BudgetVerdict::UNKNOWN};
  CostUnit unit{};
  std::int64_t limit_micros{0};
  std::int64_t consumed_micros{0};
  std::int64_t remaining_micros{0};
  UnixMillis observed_at_unix_millis{0};
  UnixMillis expires_at_unix_millis{kNoExpiry};

  [[nodiscard]] bool expired(UnixMillis now) const noexcept {
    return expires_at_unix_millis != kNoExpiry && now >= expires_at_unix_millis;
  }
  [[nodiscard]] bool current(UnixMillis now) const noexcept {
    return generation.valid() && verdict != BudgetVerdict::UNKNOWN && !expired(now);
  }
  friend bool operator==(const BudgetSnapshot&, const BudgetSnapshot&) = default;
};

// ---------------------------------------------------------------------------
// SLO evidence
// ---------------------------------------------------------------------------

enum class SloVerdict : std::uint8_t {
  UNKNOWN = 0,
  FEASIBLE = 1,
  INFEASIBLE = 2,
  kCount
};

[[nodiscard]] std::string_view to_string(SloVerdict verdict) noexcept;

/// Service-level requirement and verdict consumed from the SLO boundary.
struct SloEvidence {
  static constexpr std::uint32_t kUnspecified = 0xFFFFFFFFu;

  SLOId slo_id{};
  SLOGeneration generation{};
  SloVerdict verdict{SloVerdict::UNKNOWN};
  std::uint32_t latency_target_micros{kUnspecified};
  std::uint32_t tail_latency_target_micros{kUnspecified};
  std::uint32_t deadline_micros{kUnspecified};
  std::uint32_t availability_class{0};
  std::uint32_t throughput_class{0};
  std::uint32_t quality_floor{0};
  UnixMillis observed_at_unix_millis{0};
  UnixMillis expires_at_unix_millis{kNoExpiry};
  std::string source;

  [[nodiscard]] bool expired(UnixMillis now) const noexcept {
    return expires_at_unix_millis != kNoExpiry && now >= expires_at_unix_millis;
  }
  [[nodiscard]] bool current(UnixMillis now) const noexcept {
    return generation.valid() && verdict != SloVerdict::UNKNOWN && !expired(now);
  }
  friend bool operator==(const SloEvidence&, const SloEvidence&) = default;
};

// ---------------------------------------------------------------------------
// Compatibility evidence
// ---------------------------------------------------------------------------

enum class CompatibilityVerdict : std::uint8_t {
  UNKNOWN = 0,
  COMPATIBLE = 1,
  INCOMPATIBLE = 2,
  kCount
};

[[nodiscard]] std::string_view to_string(CompatibilityVerdict verdict) noexcept;

/// Protocol/feature compatibility supplied by a compatibility adapter. A route
/// whose compatibility is UNKNOWN cannot satisfy a mandatory compatibility
/// requirement.
struct CompatibilityEvidence {
  CompatibilityProfileId profile_id{};
  CompatibilityGeneration generation{};
  BackendId backend_id{};
  CompatibilityVerdict verdict{CompatibilityVerdict::UNKNOWN};
  std::string protocol;
  std::uint32_t protocol_version{0};
  bool structured_output{false};
  bool tool_calling{false};
  bool streaming{false};
  std::uint32_t max_context_tokens{0};
  UnixMillis observed_at_unix_millis{0};
  UnixMillis expires_at_unix_millis{kNoExpiry};
  std::string source;

  [[nodiscard]] bool current(UnixMillis now) const noexcept {
    return generation.valid() && verdict == CompatibilityVerdict::COMPATIBLE &&
           !(expires_at_unix_millis != kNoExpiry && now >= expires_at_unix_millis);
  }
  friend bool operator==(const CompatibilityEvidence&, const CompatibilityEvidence&) = default;
};

// ---------------------------------------------------------------------------
// Quality / capability class
// ---------------------------------------------------------------------------

/// Operator- or externally-supplied capability tier. Model Router never invents
/// benchmark folklore: the tier is evidence, and 0 means unclassified.
using QualityClass = std::uint32_t;

inline constexpr QualityClass kUnclassifiedQuality = 0;

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

enum class FallbackPolicy : std::uint8_t {
  /// No fallback may ever be selected.
  FORBIDDEN = 0,
  /// A fallback may be selected only for explicitly listed transient outcomes.
  EXPLICIT_ONLY = 1,
  /// A fallback may be selected for any non-permanent outcome.
  PERMITTED = 2,
  kCount
};

[[nodiscard]] std::string_view to_string(FallbackPolicy policy) noexcept;

/// Generation-bound declarative routing policy. This is a typed structure, not a
/// scripting engine: it expresses allow/deny, floors, ceilings, and preferences.
struct PolicySnapshot {
  PolicyId policy_id{};
  PolicyGeneration generation{};

  std::vector<ModelId> allowed_models;
  std::vector<ModelId> denied_models;
  std::vector<ProviderId> allowed_providers;
  std::vector<ProviderId> denied_providers;
  std::vector<BackendId> allowed_backends;
  std::vector<BackendId> denied_backends;
  std::vector<ModelFamilyId> preferred_model_families;
  std::vector<ModelFamilyId> denied_model_families;
  std::vector<LocalityKey> allowed_localities;
  std::vector<LocalityKey> denied_localities;
  std::vector<TenantId> bound_tenants;

  /// Hard floors and ceilings. UNKNOWN/unset means "no policy constraint".
  TrustDomain minimum_trust{TrustDomain::UNKNOWN};
  CapabilityState minimum_capability_evidence{CapabilityState::UNKNOWN};
  QualityClass quality_floor{kUnclassifiedQuality};
  Money maximum_request_cost{};
  std::uint32_t maximum_latency_micros{SloEvidence::kUnspecified};

  bool local_only{false};
  bool deny_public_remote{false};
  bool allow_unlisted_models{true};
  bool allow_unlisted_providers{true};
  bool allow_unlisted_backends{true};

  /// Fallback governance.
  FallbackPolicy fallback_policy{FallbackPolicy::EXPLICIT_ONLY};
  std::vector<OutcomeCode> fallback_permitted_outcomes;
  std::uint32_t max_fallback_depth{1};

  /// Maximum share of candidates from one provider, in parts per 10000.
  /// 0 means unconstrained.
  std::uint32_t max_provider_concentration_ppm{0};

  UnixMillis observed_at_unix_millis{0};
  UnixMillis expires_at_unix_millis{kNoExpiry};

  [[nodiscard]] bool current(UnixMillis now) const noexcept {
    return generation.valid() &&
           !(expires_at_unix_millis != kNoExpiry && now >= expires_at_unix_millis);
  }
  /// Canonicalizes every list so policy evaluation never depends on input order.
  void canonicalize();
  /// Returns an empty string when the policy is coherent, otherwise the reason.
  [[nodiscard]] std::string validate() const;

  friend bool operator==(const PolicySnapshot&, const PolicySnapshot&) = default;
};

}  // namespace model_router

#endif  // MODEL_ROUTER_EVIDENCE_HPP
