// Model Router - route candidates and deterministic ranking factors.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/candidate.hpp"

#include <algorithm>
#include <array>

#include "model_router/detail/sha256.hpp"

namespace model_router {
namespace {

constexpr std::array<std::string_view, static_cast<std::size_t>(RankingFactor::kCount)>
    kFactorNames = {
        "capability_fit",
        "preferred_capability_coverage",
        "quality_class",
        "cost_total",
        "latency",
        "tail_latency",
        "queue_delay",
        "warmth",
        "residency",
        "locality",
        "network_distance",
        "provider_availability",
        "backend_health",
        "backend_readiness",
        "capacity_headroom",
        "reservation_confidence",
        "slo_headroom",
        "context_headroom",
        "trust_preference",
        "policy_preference",
        "model_affinity",
        "cache_affinity",
        "historical_reliability",
        "failure_domain_diversity",
        "data_movement_cost",
        "backend_startup_cost",
        "route_switch_penalty",
        "continuity_stickiness",
        "caller_preference",
};

}  // namespace

std::string CandidateKey::to_string() const {
  std::string out;
  out += "model=";
  out += std::to_string(model_id.value());
  out.push_back('@');
  out += std::to_string(model_generation.value());
  out += ".artifact=";
  out += std::to_string(artifact_generation.value());
  out += " backend=";
  out += std::to_string(backend_id.value());
  out.push_back('@');
  out += std::to_string(backend_generation.value());
  out += ".boot=";
  out += std::to_string(backend_boot.value());
  out += " endpoint=";
  out += std::to_string(endpoint_id.value());
  out.push_back('@');
  out += std::to_string(endpoint_generation.value());
  return out;
}

std::string CandidateKey::digest() const {
  std::string canonical = to_string();
  return detail::to_hex(detail::sha256(canonical));
}

std::string_view to_string(RankingFactor factor) noexcept {
  const auto index = static_cast<std::size_t>(factor);
  if (index >= kFactorNames.size()) {
    return "unknown";
  }
  return kFactorNames[index];
}

std::vector<RankingFactor> canonical_factor_order() {
  std::vector<RankingFactor> factors;
  factors.reserve(static_cast<std::size_t>(RankingFactor::kCount));
  for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(RankingFactor::kCount); ++index) {
    factors.push_back(static_cast<RankingFactor>(index));
  }
  return factors;
}

std::int64_t RankingWeights::weight(RankingFactor factor) const noexcept {
  switch (factor) {
    case RankingFactor::CAPABILITY_FIT:
      return capability_fit_ppm;
    case RankingFactor::PREFERRED_CAPABILITY_COVERAGE:
      return preferred_capability_coverage_ppm;
    case RankingFactor::QUALITY_CLASS:
      return quality_class_ppm;
    case RankingFactor::COST_TOTAL:
      return cost_total_ppm;
    case RankingFactor::LATENCY:
      return latency_ppm;
    case RankingFactor::TAIL_LATENCY:
      return tail_latency_ppm;
    case RankingFactor::QUEUE_DELAY:
      return queue_delay_ppm;
    case RankingFactor::WARMTH:
      return warmth_ppm;
    case RankingFactor::RESIDENCY:
      return residency_ppm;
    case RankingFactor::LOCALITY:
      return locality_ppm;
    case RankingFactor::NETWORK_DISTANCE:
      return network_distance_ppm;
    case RankingFactor::PROVIDER_AVAILABILITY:
      return provider_availability_ppm;
    case RankingFactor::BACKEND_HEALTH:
      return backend_health_ppm;
    case RankingFactor::BACKEND_READINESS:
      return backend_readiness_ppm;
    case RankingFactor::CAPACITY_HEADROOM:
      return capacity_headroom_ppm;
    case RankingFactor::RESERVATION_CONFIDENCE:
      return reservation_confidence_ppm;
    case RankingFactor::SLO_HEADROOM:
      return slo_headroom_ppm;
    case RankingFactor::CONTEXT_HEADROOM:
      return context_headroom_ppm;
    case RankingFactor::TRUST_PREFERENCE:
      return trust_preference_ppm;
    case RankingFactor::POLICY_PREFERENCE:
      return policy_preference_ppm;
    case RankingFactor::MODEL_AFFINITY:
      return model_affinity_ppm;
    case RankingFactor::CACHE_AFFINITY:
      return cache_affinity_ppm;
    case RankingFactor::HISTORICAL_RELIABILITY:
      return historical_reliability_ppm;
    case RankingFactor::FAILURE_DOMAIN_DIVERSITY:
      return failure_domain_diversity_ppm;
    case RankingFactor::DATA_MOVEMENT_COST:
      return data_movement_cost_ppm;
    case RankingFactor::BACKEND_STARTUP_COST:
      return backend_startup_cost_ppm;
    case RankingFactor::ROUTE_SWITCH_PENALTY:
      return route_switch_penalty_ppm;
    case RankingFactor::CONTINUITY_STICKINESS:
      return continuity_stickiness_ppm;
    case RankingFactor::CALLER_PREFERENCE:
      return caller_preference_ppm;
    default:
      return 0;
  }
}

std::string RankingWeights::validate() const {
  for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(RankingFactor::kCount); ++index) {
    const auto factor = static_cast<RankingFactor>(index);
    const std::int64_t value = weight(factor);
    if (value < 0) {
      return "ranking weight " + std::string(to_string(factor)) + " is negative";
    }
    if (value > 10000000) {
      return "ranking weight " + std::string(to_string(factor)) +
             " exceeds 10000000 parts per 10000";
    }
  }
  return {};
}

std::string RankingScales::validate() const {
  if (cost_reference_micros <= 0 || latency_reference_micros <= 0 ||
      tail_latency_reference_micros <= 0 || queue_reference_micros <= 0 ||
      context_reference_tokens <= 0 || network_distance_reference <= 0 ||
      startup_cost_reference_micros <= 0 || quality_reference <= 0) {
    return "every ranking reference scale must be positive";
  }
  return {};
}

bool RankedCandidate::fully_known() const noexcept {
  return std::all_of(factors.begin(), factors.end(),
                     [](const FactorValue& factor) { return factor.known; });
}

const FactorValue* RankedCandidate::find(RankingFactor factor) const noexcept {
  const auto iter = std::lower_bound(
      factors.begin(), factors.end(), factor,
      [](const FactorValue& value, RankingFactor key) { return value.factor < key; });
  if (iter == factors.end() || iter->factor != factor) {
    return nullptr;
  }
  return &*iter;
}

bool candidate_ranks_before(const RankedCandidate& lhs, const RankedCandidate& rhs) noexcept {
  if (lhs.score != rhs.score) {
    return lhs.score > rhs.score;
  }
  // Canonical tie-break: compare the ordered factor sequence. A factor present
  // in one candidate and absent in the other is treated as zero contribution.
  const std::size_t count = std::min(lhs.factors.size(), rhs.factors.size());
  for (std::size_t index = 0; index < count; ++index) {
    const FactorValue& left = lhs.factors[index];
    const FactorValue& right = rhs.factors[index];
    if (left.factor != right.factor) {
      return left.factor < right.factor;
    }
    if (left.contribution != right.contribution) {
      return left.contribution > right.contribution;
    }
    if (left.normalized != right.normalized) {
      return left.normalized > right.normalized;
    }
    if (left.known != right.known) {
      return left.known;
    }
  }
  if (lhs.factors.size() != rhs.factors.size()) {
    return lhs.factors.size() > rhs.factors.size();
  }
  // Final tie-break: canonical candidate identity, ascending. This makes the
  // ordering total and independent of container layout.
  return lhs.key < rhs.key;
}

}  // namespace model_router
