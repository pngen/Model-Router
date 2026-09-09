// Model Router - route candidates and deterministic ranking factors.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_CANDIDATE_HPP
#define MODEL_ROUTER_CANDIDATE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "model_router/catalog.hpp"
#include "model_router/evidence.hpp"
#include "model_router/ids.hpp"
#include "model_router/outcome.hpp"

namespace model_router {

/// Canonical identity of one routing candidate: a (model generation, artifact
/// generation, backend incarnation, endpoint generation) tuple. Ordering is
/// total and content-derived, so ranking never depends on container layout.
struct CandidateKey {
  ModelId model_id{};
  ModelGeneration model_generation{};
  ArtifactGeneration artifact_generation{};
  BackendId backend_id{};
  BackendGeneration backend_generation{};
  BackendBootId backend_boot{};
  EndpointId endpoint_id{};
  EndpointGeneration endpoint_generation{};

  [[nodiscard]] bool complete() const noexcept {
    return model_id.valid() && model_generation.valid() && artifact_generation.valid() &&
           backend_id.valid() && backend_generation.valid() && backend_boot.valid() &&
           endpoint_id.valid() && endpoint_generation.valid();
  }

  friend bool operator==(const CandidateKey&, const CandidateKey&) = default;
  friend std::strong_ordering operator<=>(const CandidateKey&, const CandidateKey&) = default;

  /// Canonical, human-stable rendering used in explanations.
  [[nodiscard]] std::string to_string() const;
  /// Content-derived digest; identical canonical state yields an identical value.
  [[nodiscard]] std::string digest() const;
};

/// Named ranking factors. Values are integers; there is no floating-point
/// arithmetic in the ranking path.
enum class RankingFactor : std::uint8_t {
  CAPABILITY_FIT = 0,
  PREFERRED_CAPABILITY_COVERAGE,
  QUALITY_CLASS,
  COST_TOTAL,
  LATENCY,
  TAIL_LATENCY,
  QUEUE_DELAY,
  WARMTH,
  RESIDENCY,
  LOCALITY,
  NETWORK_DISTANCE,
  PROVIDER_AVAILABILITY,
  BACKEND_HEALTH,
  BACKEND_READINESS,
  CAPACITY_HEADROOM,
  RESERVATION_CONFIDENCE,
  SLO_HEADROOM,
  CONTEXT_HEADROOM,
  TRUST_PREFERENCE,
  POLICY_PREFERENCE,
  MODEL_AFFINITY,
  CACHE_AFFINITY,
  HISTORICAL_RELIABILITY,
  FAILURE_DOMAIN_DIVERSITY,
  DATA_MOVEMENT_COST,
  BACKEND_STARTUP_COST,
  ROUTE_SWITCH_PENALTY,
  CONTINUITY_STICKINESS,
  CALLER_PREFERENCE,
  kCount
};

[[nodiscard]] std::string_view to_string(RankingFactor factor) noexcept;
/// Canonical factor order used for iteration, explanation, and tie-breaks.
[[nodiscard]] std::vector<RankingFactor> canonical_factor_order();

/// One factor's contribution for one candidate. A factor whose evidence is
/// absent is UNKNOWN: it contributes exactly zero and is marked visible in the
/// explanation. Missing evidence never becomes a favorable score.
struct FactorValue {
  RankingFactor factor{RankingFactor::CAPABILITY_FIT};
  /// False when the evidence required to compute this factor is absent/stale.
  bool known{false};
  /// Normalized integer value in [0, kFactorScale], or 0 when unknown.
  std::int64_t normalized{0};
  /// Raw evidence value in its own unit, for explanation only.
  std::int64_t raw{0};
  /// Applied weight in parts per 10000.
  std::int64_t weight_ppm{0};
  /// normalized * weight_ppm / kFactorScale, computed with checked arithmetic.
  std::int64_t contribution{0};
  /// Generation of the evidence this factor was derived from.
  std::uint64_t evidence_generation{0};
  /// Stable identifier of the evidence source.
  std::string source;

  static constexpr std::int64_t kFactorScale = 1000000;

  friend bool operator==(const FactorValue&, const FactorValue&) = default;
};

/// Weights applied to each factor, in parts per 10000. All weights are integers
/// so that ranking is exactly reproducible.
struct RankingWeights {
  std::int64_t capability_fit_ppm{300000};
  std::int64_t preferred_capability_coverage_ppm{40000};
  std::int64_t quality_class_ppm{120000};
  std::int64_t cost_total_ppm{150000};
  std::int64_t latency_ppm{80000};
  std::int64_t tail_latency_ppm{40000};
  std::int64_t queue_delay_ppm{30000};
  std::int64_t warmth_ppm{60000};
  std::int64_t residency_ppm{40000};
  std::int64_t locality_ppm{40000};
  std::int64_t network_distance_ppm{20000};
  std::int64_t provider_availability_ppm{60000};
  std::int64_t backend_health_ppm{100000};
  std::int64_t backend_readiness_ppm{80000};
  std::int64_t capacity_headroom_ppm{50000};
  std::int64_t reservation_confidence_ppm{30000};
  std::int64_t slo_headroom_ppm{70000};
  std::int64_t context_headroom_ppm{20000};
  std::int64_t trust_preference_ppm{60000};
  std::int64_t policy_preference_ppm{60000};
  std::int64_t model_affinity_ppm{50000};
  std::int64_t cache_affinity_ppm{30000};
  std::int64_t historical_reliability_ppm{40000};
  std::int64_t failure_domain_diversity_ppm{20000};
  std::int64_t data_movement_cost_ppm{20000};
  std::int64_t backend_startup_cost_ppm{20000};
  std::int64_t route_switch_penalty_ppm{30000};
  std::int64_t continuity_stickiness_ppm{50000};
  std::int64_t caller_preference_ppm{100000};

  /// Returns the weight for one factor.
  [[nodiscard]] std::int64_t weight(RankingFactor factor) const noexcept;
  /// Returns an empty string when the weights are coherent.
  [[nodiscard]] std::string validate() const;

  friend bool operator==(const RankingWeights&, const RankingWeights&) = default;
};

/// Fixed reference scales used to normalize factor evidence into integers.
/// Scales are absolute, not relative to the peer set, so a factor value and the
/// resulting explanation do not change when unrelated candidates appear.
struct RankingScales {
  std::int64_t cost_reference_micros{1000000};
  std::int64_t latency_reference_micros{5000000};
  std::int64_t tail_latency_reference_micros{20000000};
  std::int64_t queue_reference_micros{1000000};
  std::int64_t context_reference_tokens{1000000};
  std::int64_t network_distance_reference{100000};
  std::int64_t startup_cost_reference_micros{10000000};
  std::int64_t quality_reference{1000};

  /// Returns an empty string when every scale is positive.
  [[nodiscard]] std::string validate() const;
  friend bool operator==(const RankingScales&, const RankingScales&) = default;
};

/// A discovered candidate: semantic model identity plus the concrete serving
/// backend incarnation and the evidence observed for it.
struct RouteCandidate {
  CandidateKey key{};

  ModelDescriptor model{};
  BackendDescriptor backend{};
  EndpointDescriptor endpoint{};

  /// Effective per-candidate values after model/binding resolution.
  std::uint32_t context_limit_tokens{0};
  QualityClass quality_class{kUnclassifiedQuality};

  /// Precomputed capability-fit counts used by the ranking factor.
  std::uint32_t required_capabilities_satisfied{0};
  std::uint32_t required_capabilities_total{0};
  std::uint32_t preferred_capabilities_satisfied{0};
  std::uint32_t preferred_capabilities_total{0};

  /// Discovery provenance, for explanation only. Discovery never ranks.
  std::string discovery_source;

  [[nodiscard]] bool complete() const noexcept { return key.complete(); }
  friend bool operator==(const RouteCandidate&, const RouteCandidate&) = default;
};

/// A ranked, hard-eligible candidate with its full factor vector.
struct RankedCandidate {
  CandidateKey key{};
  std::int64_t score{0};
  std::vector<FactorValue> factors;
  std::uint32_t rank{0};

  /// True when the candidate carries no UNKNOWN factor at all.
  [[nodiscard]] bool fully_known() const noexcept;
  [[nodiscard]] const FactorValue* find(RankingFactor factor) const noexcept;

  friend bool operator==(const RankedCandidate&, const RankedCandidate&) = default;
};

/// Deterministic ranking comparison. Higher score wins; ties are broken by the
/// canonical factor sequence, then by the canonical candidate key. This is the
/// single definition of "better" used everywhere.
[[nodiscard]] bool candidate_ranks_before(const RankedCandidate& lhs,
                                          const RankedCandidate& rhs) noexcept;

}  // namespace model_router

#endif  // MODEL_ROUTER_CANDIDATE_HPP
