// Model Router - route requests and normalized requirements.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_REQUEST_HPP
#define MODEL_ROUTER_REQUEST_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "model_router/catalog.hpp"
#include "model_router/evidence.hpp"
#include "model_router/ids.hpp"
#include "model_router/limits.hpp"

namespace model_router {

/// What a retry or reroute is permitted to do. The caller remains responsible
/// for higher-level agent/tool side-effect semantics.
struct RetryReroutePolicy {
  bool allow_retry{true};
  bool allow_reroute{true};
  bool allow_model_switch{true};
  bool allow_provider_switch{true};
  /// Maximum attempts including the first.
  std::uint32_t max_attempts{2};
  /// Maximum number of fallback candidates the plan may contain.
  std::uint32_t max_fallbacks{2};

  friend bool operator==(const RetryReroutePolicy&, const RetryReroutePolicy&) = default;
};

/// Continuity preferences. Stickiness is a ranking preference only: it can never
/// rescue a hard-invalid candidate.
struct Stickiness {
  bool prefer_same_model_family{false};
  bool prefer_same_provider{false};
  bool prefer_same_backend{false};
  bool prefer_context_locality{false};

  ModelFamilyId sticky_family{};
  ProviderId sticky_provider{};
  BackendId sticky_backend{};

  friend bool operator==(const Stickiness&, const Stickiness&) = default;
};

/// Normalized, deterministic routing requirements. Validation rejects
/// self-contradictory combinations before any candidate is considered.
struct RouteRequirements {
  // Capability
  std::vector<CapabilityRequirement> required_capabilities;
  std::vector<CapabilityKey> preferred_capabilities;
  /// Minimum evidence state applied to every required capability unless the
  /// requirement states its own.
  CapabilityState minimum_capability_evidence{CapabilityState::DECLARED};

  // Shape
  std::uint32_t min_context_tokens{0};
  std::uint32_t max_output_tokens{0};
  ModalitySet required_input_modalities{};
  ModalitySet required_output_modalities{};
  bool require_structured_output{false};
  bool require_json_schema{false};
  bool require_tool_calling{false};
  bool require_streaming{false};
  bool require_logprobs{false};
  bool require_deterministic_seed{false};

  // Quality
  QualityClass quality_floor{kUnclassifiedQuality};

  // Identity filters
  std::vector<ModelId> model_allowlist;
  std::vector<ModelId> model_denylist;
  std::vector<ModelFamilyId> family_allowlist;
  std::vector<ModelFamilyId> family_denylist;
  std::vector<ProviderId> provider_allowlist;
  std::vector<ProviderId> provider_denylist;
  std::vector<BackendId> backend_allowlist;
  std::vector<BackendId> backend_denylist;

  // Placement
  bool local_only{false};
  bool offline_only{false};
  bool remote_allowed{true};
  std::vector<LocalityKey> required_localities;
  std::vector<LocalityKey> denied_localities;
  TrustDomain minimum_trust{TrustDomain::UNKNOWN};

  // Economics
  bool require_known_cost{false};
  bool enforce_cost_ceiling{false};
  Money maximum_cost{};

  // Latency / SLO
  std::uint32_t max_latency_micros{SloEvidence::kUnspecified};
  std::uint32_t deadline_micros{SloEvidence::kUnspecified};
  bool require_slo{false};

  // Live-state requirements
  bool require_available{true};
  bool require_healthy{true};
  bool require_ready{true};
  bool require_resident{false};
  bool require_capacity{false};
  bool require_reservation{false};
  bool require_known_latency{false};
  bool require_known_compatibility{false};

  // Compatibility
  std::string required_protocol;
  std::uint32_t required_protocol_version{0};

  // Affinity
  ModelId affinity_model{};
  ProviderId affinity_provider{};
  BackendId affinity_backend{};
  Stickiness stickiness{};

  // Fallback
  bool allow_fallbacks{true};
  FallbackPolicy fallback_policy{FallbackPolicy::EXPLICIT_ONLY};
  RetryReroutePolicy retry_policy{};

  /// Returns an empty string when the requirements are coherent, otherwise a
  /// deterministic description of the first violation.
  [[nodiscard]] std::string validate(const ResourceLimits& limits) const;
  /// Canonicalizes every list so evaluation never depends on input order.
  void canonicalize();
  /// Returns a stable digest of the normalized requirement set.
  [[nodiscard]] std::string digest() const;
};

/// Caller authority supplied by an external runtime such as Agent Runtime.
/// Model Router binds these identities into its decisions but never owns the
/// lifecycle they describe.
struct CallerAuthority {
  std::uint64_t agent_runtime_id{0};
  std::uint64_t agent_run_id{0};
  std::uint64_t action_id{0};
  std::uint64_t action_generation{0};
  std::uint64_t attempt_generation{0};
  std::uint64_t work_id{0};
  std::uint64_t work_generation{0};

  friend bool operator==(const CallerAuthority&, const CallerAuthority&) = default;
};

/// A normalized routing request.
struct RouteRequest {
  RouteRequestId request_id{};
  RouteRequestGeneration request_generation{};

  RouteRequirements requirements{};
  CallerAuthority caller{};

  TenantId tenant{};
  NamespaceId name_space{};

  PolicyId policy_id{};
  PolicyGeneration policy_generation{};

  BudgetId budget_id{};
  BudgetGeneration budget_generation{};

  SLOId slo_id{};
  SLOGeneration slo_generation{};

  CorrelationId correlation_id{};

  std::uint32_t estimated_input_tokens{0};
  std::uint32_t estimated_output_tokens{0};

  UnixMillis created_at_unix_millis{0};
  /// Request currentness window. kNoExpiry means the request does not expire.
  UnixMillis expires_at_unix_millis{kNoExpiry};

  [[nodiscard]] bool expired(UnixMillis now) const noexcept {
    return expires_at_unix_millis != kNoExpiry && now >= expires_at_unix_millis;
  }
  /// Returns an empty string when the request is coherent.
  [[nodiscard]] std::string validate(const ResourceLimits& limits) const;
  /// Canonical digest of the request identity and normalized requirements.
  [[nodiscard]] std::string digest() const;

  friend bool operator==(const RouteRequest&, const RouteRequest&) = default;
};

/// Failure classification used to decide whether a fallback or reroute is legal.
enum class FailureClass : std::uint8_t {
  TRANSIENT_BACKEND_FAILURE = 0,
  BACKEND_UNAVAILABLE,
  BACKEND_RESTARTED,
  MODEL_UNAVAILABLE,
  STALE_ROUTE,
  POLICY_CHANGED,
  BUDGET_CHANGED,
  PRICE_CHANGED,
  SLO_CHANGED,
  CAPACITY_CHANGED,
  COMPATIBILITY_CHANGED,
  PERMANENT_REJECTION,
  CALLER_CANCELLED,
  UNKNOWN,
  kCount
};

[[nodiscard]] std::string_view to_string(FailureClass failure) noexcept;
/// Maps a typed outcome to the failure class used by fallback governance.
[[nodiscard]] FailureClass classify_failure(OutcomeCode code) noexcept;
/// True when the failure class may never be answered with an automatic reroute.
[[nodiscard]] bool is_permanent(FailureClass failure) noexcept;

}  // namespace model_router

#endif  // MODEL_ROUTER_REQUEST_HPP
