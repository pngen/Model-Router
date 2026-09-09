// Model Router - deterministic factor construction.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// There is no floating-point arithmetic in this file. Every factor is an
// integer value in [0, kFactorScale] multiplied by an integer weight; a factor
// with no evidence is UNKNOWN and contributes exactly zero.

#include "ranking.hpp"

#include <algorithm>
#include <limits>

namespace model_router::detail {
namespace {

constexpr std::int64_t kScale = FactorValue::kFactorScale;
constexpr std::int64_t kMaxScore = std::numeric_limits<std::int64_t>::max();

[[nodiscard]] std::int64_t saturating_add(std::int64_t lhs, std::int64_t rhs) noexcept {
  if (rhs > 0 && lhs > kMaxScore - rhs) {
    return kMaxScore;
  }
  if (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return lhs + rhs;
}

[[nodiscard]] std::int64_t mul_div(std::int64_t value, std::int64_t numerator,
                                   std::int64_t denominator) noexcept {
  if (denominator <= 0) {
    return 0;
  }
  if (value == 0 || numerator == 0) {
    return 0;
  }
  // value <= kScale in every current use, so the product is bounded well inside
  // int64 for any in-range weight. The guards keep the function total even for
  // a pathological weight.
  if (value > 0 && numerator > 0 && value > kMaxScore / numerator) {
    return kMaxScore;
  }
  if (value > 0 && numerator < 0 && numerator < std::numeric_limits<std::int64_t>::min() / value) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return (value * numerator) / denominator;
}

[[nodiscard]] FactorValue unknown(RankingFactor factor, std::int64_t weight) noexcept {
  FactorValue value;
  value.factor = factor;
  value.known = false;
  value.normalized = 0;
  value.raw = 0;
  value.weight_ppm = weight;
  value.contribution = 0;
  return value;
}

[[nodiscard]] FactorValue known_value(RankingFactor factor, std::int64_t raw,
                                      std::int64_t normalized, std::int64_t weight) noexcept {
  FactorValue value;
  value.factor = factor;
  value.known = true;
  value.raw = raw;
  value.normalized = std::clamp<std::int64_t>(normalized, 0, kScale);
  value.weight_ppm = weight;
  value.contribution = mul_div(value.normalized, weight, kScale);
  return value;
}

[[nodiscard]] std::int64_t reference(std::int64_t value) noexcept {
  return value > 0 ? value : 1;
}

}  // namespace

std::int64_t normalize_higher_better(std::int64_t raw, std::int64_t ref) noexcept {
  if (raw <= 0) {
    return 0;
  }
  return std::clamp<std::int64_t>(mul_div(raw, kScale, reference(ref)), 0, kScale);
}

std::int64_t normalize_lower_better(std::int64_t raw, std::int64_t ref) noexcept {
  if (raw <= 0) {
    return kScale;
  }
  const std::int64_t cost = mul_div(raw, kScale, reference(ref));
  if (cost >= kScale) {
    return 0;
  }
  return kScale - cost;
}

std::vector<FactorValue> build_factors(const RouteCandidate& candidate,
                                       const RankingContext& context,
                                       std::int64_t* out_score) noexcept {
  std::vector<FactorValue> factors;
  factors.reserve(static_cast<std::size_t>(RankingFactor::kCount));

  const RouteRequest& request = *context.request;
  const CandidateEvidence& evidence = *context.evidence;
  const RankingWeights& weights = *context.weights;
  const RankingScales& scales = *context.scales;
  const UnixMillis now = context.now;

  const auto weight_of = [&weights](RankingFactor factor) { return weights.weight(factor); };
  const auto push = [&factors](FactorValue value) { factors.push_back(value); };

  // 1. Capability fit: all required capabilities were proven by hard eligibility.
  if (candidate.required_capabilities_total > 0) {
    const std::int64_t total = candidate.required_capabilities_total;
    const std::int64_t satisfied = candidate.required_capabilities_satisfied;
    push(known_value(RankingFactor::CAPABILITY_FIT, satisfied,
                     mul_div(satisfied, kScale, total), weight_of(RankingFactor::CAPABILITY_FIT)));
  } else {
    push(unknown(RankingFactor::CAPABILITY_FIT, weight_of(RankingFactor::CAPABILITY_FIT)));
  }

  // 2. Preferred capability coverage.
  if (candidate.preferred_capabilities_total > 0) {
    const std::int64_t total = candidate.preferred_capabilities_total;
    const std::int64_t satisfied = candidate.preferred_capabilities_satisfied;
    push(known_value(RankingFactor::PREFERRED_CAPABILITY_COVERAGE, satisfied,
                     mul_div(satisfied, kScale, total),
                     weight_of(RankingFactor::PREFERRED_CAPABILITY_COVERAGE)));
  } else {
    push(unknown(RankingFactor::PREFERRED_CAPABILITY_COVERAGE,
                 weight_of(RankingFactor::PREFERRED_CAPABILITY_COVERAGE)));
  }

  // 3. Quality class: evidence only. Never benchmark folklore.
  if (candidate.quality_class != kUnclassifiedQuality) {
    push(known_value(RankingFactor::QUALITY_CLASS,
                     static_cast<std::int64_t>(candidate.quality_class),
                     normalize_higher_better(static_cast<std::int64_t>(candidate.quality_class),
                                             scales.quality_reference),
                     weight_of(RankingFactor::QUALITY_CLASS)));
  } else {
    push(unknown(RankingFactor::QUALITY_CLASS, weight_of(RankingFactor::QUALITY_CLASS)));
  }

  // 4. Cost: unknown price is never free.
  if (evidence.cost.known() && evidence.cost.current(now) &&
      evidence.cost.unit.compatible_with(evidence.cost.unit)) {
    FactorValue value = known_value(RankingFactor::COST_TOTAL, evidence.cost.estimated_total_micros,
                                    normalize_lower_better(evidence.cost.estimated_total_micros,
                                                           scales.cost_reference_micros),
                                    weight_of(RankingFactor::COST_TOTAL));
    value.evidence_generation = evidence.cost.price_generation.value();
    value.source = evidence.cost.source;
    push(value);
  } else {
    push(unknown(RankingFactor::COST_TOTAL, weight_of(RankingFactor::COST_TOTAL)));
  }

  // 5-7. Latency, tail latency, queue delay: observations, never guarantees.
  const LatencyEvidence& latency = candidate.backend.latency;
  const bool latency_current = !latency.expired(now);
  if (latency_current && latency.dispatch_micros != LatencyEvidence::kUnknownLatencyMicros) {
    FactorValue value = known_value(RankingFactor::LATENCY, latency.dispatch_micros,
                                    normalize_lower_better(latency.dispatch_micros,
                                                           scales.latency_reference_micros),
                                    weight_of(RankingFactor::LATENCY));
    value.evidence_generation = latency.health_generation.value();
    push(value);
  } else {
    push(unknown(RankingFactor::LATENCY, weight_of(RankingFactor::LATENCY)));
  }
  if (latency_current && latency.tail_micros != LatencyEvidence::kUnknownLatencyMicros) {
    push(known_value(RankingFactor::TAIL_LATENCY, latency.tail_micros,
                     normalize_lower_better(latency.tail_micros,
                                            scales.tail_latency_reference_micros),
                     weight_of(RankingFactor::TAIL_LATENCY)));
  } else {
    push(unknown(RankingFactor::TAIL_LATENCY, weight_of(RankingFactor::TAIL_LATENCY)));
  }
  if (latency_current && latency.queue_micros != LatencyEvidence::kUnknownLatencyMicros) {
    push(known_value(RankingFactor::QUEUE_DELAY, latency.queue_micros,
                     normalize_lower_better(latency.queue_micros, scales.queue_reference_micros),
                     weight_of(RankingFactor::QUEUE_DELAY)));
  } else {
    push(unknown(RankingFactor::QUEUE_DELAY, weight_of(RankingFactor::QUEUE_DELAY)));
  }

  // 8. Warmth: readiness plus a cold-start penalty when one is observed.
  {
    const ReadinessEvidence& readiness = candidate.backend.readiness;
    if (readiness.generation.valid() && !readiness.expired(now) &&
        readiness.state != ReadinessState::UNKNOWN) {
      std::int64_t normalized = 0;
      switch (readiness.state) {
        case ReadinessState::READY:
          normalized = kScale;
          break;
        case ReadinessState::WARMING:
          normalized = kScale / 2;
          break;
        default:
          normalized = 0;
          break;
      }
      if (latency_current && latency.cold_start_penalty_micros !=
                                 LatencyEvidence::kUnknownLatencyMicros) {
        normalized -= mul_div(latency.cold_start_penalty_micros, kScale,
                              reference(scales.startup_cost_reference_micros));
      }
      FactorValue value = known_value(RankingFactor::WARMTH,
                                      static_cast<std::int64_t>(readiness.state), normalized,
                                      weight_of(RankingFactor::WARMTH));
      value.evidence_generation = readiness.generation.value();
      push(value);
    } else {
      push(unknown(RankingFactor::WARMTH, weight_of(RankingFactor::WARMTH)));
    }
  }

  // 9. Residency.
  {
    const ResidencyEvidence& residency = candidate.backend.residency;
    if (residency.generation.valid() && !residency.expired(now) &&
        residency.state != ResidencyState::UNKNOWN) {
      std::int64_t normalized = 0;
      if (residency.state == ResidencyState::RESIDENT) {
        normalized = kScale;
      } else if (residency.state == ResidencyState::LOADING) {
        normalized = kScale / 2;
      }
      FactorValue value = known_value(RankingFactor::RESIDENCY,
                                      static_cast<std::int64_t>(residency.state), normalized,
                                      weight_of(RankingFactor::RESIDENCY));
      value.evidence_generation = residency.generation.value();
      push(value);
    } else {
      push(unknown(RankingFactor::RESIDENCY, weight_of(RankingFactor::RESIDENCY)));
    }
  }

  // 10. Locality: the explicit trust domain, never a hostname.
  if (candidate.backend.trust_domain != TrustDomain::UNKNOWN) {
    const std::int64_t rank = trust_rank(candidate.backend.trust_domain);
    push(known_value(RankingFactor::LOCALITY, rank, mul_div(rank, kScale, 4),
                     weight_of(RankingFactor::LOCALITY)));
  } else {
    push(unknown(RankingFactor::LOCALITY, weight_of(RankingFactor::LOCALITY)));
  }

  // 11. Network distance.
  if (candidate.backend.network_distance != 0) {
    push(known_value(RankingFactor::NETWORK_DISTANCE, candidate.backend.network_distance,
                     normalize_lower_better(candidate.backend.network_distance,
                                            scales.network_distance_reference),
                     weight_of(RankingFactor::NETWORK_DISTANCE)));
  } else {
    push(unknown(RankingFactor::NETWORK_DISTANCE, weight_of(RankingFactor::NETWORK_DISTANCE)));
  }

  // 12. Provider availability.
  {
    const AvailabilityEvidence& availability = candidate.backend.availability;
    if (availability.generation.valid() && !availability.expired(now) &&
        availability.state != AvailabilityState::UNKNOWN) {
      const std::int64_t normalized =
          availability.state == AvailabilityState::AVAILABLE ? kScale : 0;
      FactorValue value = known_value(RankingFactor::PROVIDER_AVAILABILITY,
                                      static_cast<std::int64_t>(availability.state), normalized,
                                      weight_of(RankingFactor::PROVIDER_AVAILABILITY));
      value.evidence_generation = availability.generation.value();
      push(value);
    } else {
      push(unknown(RankingFactor::PROVIDER_AVAILABILITY,
                   weight_of(RankingFactor::PROVIDER_AVAILABILITY)));
    }
  }

  // 13. Backend health.
  {
    const HealthEvidence& health = candidate.backend.health;
    if (health.generation.valid() && !health.expired(now) &&
        health.state != HealthState::UNKNOWN) {
      std::int64_t normalized = 0;
      if (health.state == HealthState::HEALTHY) {
        normalized = kScale;
      } else if (health.state == HealthState::DEGRADED) {
        normalized = kScale / 2;
      }
      FactorValue value = known_value(RankingFactor::BACKEND_HEALTH,
                                      static_cast<std::int64_t>(health.state), normalized,
                                      weight_of(RankingFactor::BACKEND_HEALTH));
      value.evidence_generation = health.generation.value();
      push(value);
    } else {
      push(unknown(RankingFactor::BACKEND_HEALTH, weight_of(RankingFactor::BACKEND_HEALTH)));
    }
  }

  // 14. Backend readiness.
  {
    const ReadinessEvidence& readiness = candidate.backend.readiness;
    if (readiness.generation.valid() && !readiness.expired(now) &&
        readiness.state != ReadinessState::UNKNOWN) {
      std::int64_t normalized = 0;
      if (readiness.state == ReadinessState::READY) {
        normalized = kScale;
      } else if (readiness.state == ReadinessState::WARMING) {
        normalized = kScale / 2;
      }
      FactorValue value = known_value(RankingFactor::BACKEND_READINESS,
                                      static_cast<std::int64_t>(readiness.state), normalized,
                                      weight_of(RankingFactor::BACKEND_READINESS));
      value.evidence_generation = readiness.generation.value();
      push(value);
    } else {
      push(unknown(RankingFactor::BACKEND_READINESS, weight_of(RankingFactor::BACKEND_READINESS)));
    }
  }

  // 15. Capacity headroom.
  {
    const CapacityDetail& detail = candidate.backend.capacity_detail;
    if (detail.known() && detail.total_slots != CapacityDetail::kUnknownCount &&
        detail.total_slots > 0) {
      const std::int64_t normalized =
          mul_div(detail.available_slots, kScale, detail.total_slots);
      FactorValue value = known_value(RankingFactor::CAPACITY_HEADROOM,
                                      detail.available_slots, normalized,
                                      weight_of(RankingFactor::CAPACITY_HEADROOM));
      value.evidence_generation = candidate.backend.capacity.generation.value();
      push(value);
    } else {
      push(unknown(RankingFactor::CAPACITY_HEADROOM,
                   weight_of(RankingFactor::CAPACITY_HEADROOM)));
    }
  }

  // 16. Reservation confidence.
  if (evidence.reservation_present && evidence.reservation.current(now)) {
    push(known_value(RankingFactor::RESERVATION_CONFIDENCE, 1, kScale,
                     weight_of(RankingFactor::RESERVATION_CONFIDENCE)));
  } else {
    push(unknown(RankingFactor::RESERVATION_CONFIDENCE,
                 weight_of(RankingFactor::RESERVATION_CONFIDENCE)));
  }

  // 17. SLO headroom: target versus observation.
  {
    const SloEvidence& slo = evidence.slo;
    const std::uint32_t target = slo.latency_target_micros != SloEvidence::kUnspecified
                                     ? slo.latency_target_micros
                                     : request.requirements.max_latency_micros;
    if (target != SloEvidence::kUnspecified && target > 0 && latency_current &&
        latency.dispatch_micros != LatencyEvidence::kUnknownLatencyMicros) {
      push(known_value(RankingFactor::SLO_HEADROOM,
                       static_cast<std::int64_t>(latency.dispatch_micros),
                       normalize_lower_better(latency.dispatch_micros, target),
                       weight_of(RankingFactor::SLO_HEADROOM)));
    } else {
      push(unknown(RankingFactor::SLO_HEADROOM, weight_of(RankingFactor::SLO_HEADROOM)));
    }
  }

  // 18. Context headroom.
  if (request.requirements.min_context_tokens > 0 &&
      candidate.context_limit_tokens >= request.requirements.min_context_tokens) {
    const std::int64_t headroom = static_cast<std::int64_t>(candidate.context_limit_tokens) -
                                  request.requirements.min_context_tokens;
    push(known_value(RankingFactor::CONTEXT_HEADROOM, headroom,
                     normalize_higher_better(headroom, scales.context_reference_tokens),
                     weight_of(RankingFactor::CONTEXT_HEADROOM)));
  } else {
    push(unknown(RankingFactor::CONTEXT_HEADROOM, weight_of(RankingFactor::CONTEXT_HEADROOM)));
  }

  // 19. Trust preference.
  if (candidate.backend.trust_domain != TrustDomain::UNKNOWN) {
    const std::int64_t rank = trust_rank(candidate.backend.trust_domain);
    FactorValue value = known_value(RankingFactor::TRUST_PREFERENCE, rank, mul_div(rank, kScale, 4),
                                    weight_of(RankingFactor::TRUST_PREFERENCE));
    value.evidence_generation = candidate.backend.trust_generation.value();
    push(value);
  } else {
    push(unknown(RankingFactor::TRUST_PREFERENCE, weight_of(RankingFactor::TRUST_PREFERENCE)));
  }

  // 20. Policy preference: policy's own soft preference, never a hard override.
  if (context.policy_verdict != nullptr) {
    FactorValue value = known_value(RankingFactor::POLICY_PREFERENCE,
                                    context.policy_verdict->preference,
                                    context.policy_verdict->preference,
                                    weight_of(RankingFactor::POLICY_PREFERENCE));
    value.evidence_generation = request.policy_generation.value();
    push(value);
  } else {
    push(unknown(RankingFactor::POLICY_PREFERENCE, weight_of(RankingFactor::POLICY_PREFERENCE)));
  }

  // 21. Model affinity.
  if (request.requirements.affinity_model.valid()) {
    const bool matches = request.requirements.affinity_model == candidate.key.model_id;
    push(known_value(RankingFactor::MODEL_AFFINITY, matches ? 1 : 0, matches ? kScale : 0,
                     weight_of(RankingFactor::MODEL_AFFINITY)));
  } else {
    push(unknown(RankingFactor::MODEL_AFFINITY, weight_of(RankingFactor::MODEL_AFFINITY)));
  }

  // 22. Cache affinity: continuity with the tenant's last successful route.
  if (context.has_continuity_backend) {
    const bool matches = context.continuity_backend == candidate.key.backend_id;
    push(known_value(RankingFactor::CACHE_AFFINITY, matches ? 1 : 0, matches ? kScale : 0,
                     weight_of(RankingFactor::CACHE_AFFINITY)));
  } else {
    push(unknown(RankingFactor::CACHE_AFFINITY, weight_of(RankingFactor::CACHE_AFFINITY)));
  }

  // 23. Historical reliability, from authoritative evidence only.
  if (candidate.backend.reliability_ppm != 0) {
    push(known_value(RankingFactor::HISTORICAL_RELIABILITY,
                     candidate.backend.reliability_ppm,
                     mul_div(candidate.backend.reliability_ppm, kScale, 10000),
                     weight_of(RankingFactor::HISTORICAL_RELIABILITY)));
  } else {
    push(unknown(RankingFactor::HISTORICAL_RELIABILITY,
                 weight_of(RankingFactor::HISTORICAL_RELIABILITY)));
  }

  // 24. Failure-domain diversity, only when a reference provider is supplied.
  if (context.has_diversity_reference) {
    const bool different = !(context.diversity_reference_provider == candidate.backend.provider_id);
    push(known_value(RankingFactor::FAILURE_DOMAIN_DIVERSITY, different ? 1 : 0,
                     different ? kScale : 0,
                     weight_of(RankingFactor::FAILURE_DOMAIN_DIVERSITY)));
  } else {
    push(unknown(RankingFactor::FAILURE_DOMAIN_DIVERSITY,
                 weight_of(RankingFactor::FAILURE_DOMAIN_DIVERSITY)));
  }

  // 25. Data movement cost: derived from explicit locality evidence only.
  {
    const bool local = candidate.backend.trust_domain == TrustDomain::LOCAL;
    if (local || candidate.backend.network_distance != 0) {
      const std::int64_t raw = local ? 0 : candidate.backend.network_distance;
      push(known_value(RankingFactor::DATA_MOVEMENT_COST, raw,
                       normalize_lower_better(raw, scales.network_distance_reference),
                       weight_of(RankingFactor::DATA_MOVEMENT_COST)));
    } else {
      push(unknown(RankingFactor::DATA_MOVEMENT_COST,
                   weight_of(RankingFactor::DATA_MOVEMENT_COST)));
    }
  }

  // 26. Backend startup cost.
  if (latency_current && latency.cold_start_penalty_micros !=
                             LatencyEvidence::kUnknownLatencyMicros) {
    push(known_value(RankingFactor::BACKEND_STARTUP_COST, latency.cold_start_penalty_micros,
                     normalize_lower_better(latency.cold_start_penalty_micros,
                                            scales.startup_cost_reference_micros),
                     weight_of(RankingFactor::BACKEND_STARTUP_COST)));
  } else {
    push(unknown(RankingFactor::BACKEND_STARTUP_COST,
                 weight_of(RankingFactor::BACKEND_STARTUP_COST)));
  }

  // 27. Route switch penalty: only meaningful once a prior route exists.
  if (context.has_continuity_backend) {
    const bool same = context.continuity_backend == candidate.key.backend_id;
    push(known_value(RankingFactor::ROUTE_SWITCH_PENALTY, same ? 0 : 1, same ? kScale : 0,
                     weight_of(RankingFactor::ROUTE_SWITCH_PENALTY)));
  } else {
    push(unknown(RankingFactor::ROUTE_SWITCH_PENALTY,
                 weight_of(RankingFactor::ROUTE_SWITCH_PENALTY)));
  }

  // 28. Continuity stickiness: explicit caller continuity preferences.
  {
    const Stickiness& stickiness = request.requirements.stickiness;
    std::int64_t requested = 0;
    std::int64_t matched = 0;
    if (stickiness.prefer_same_model_family) {
      ++requested;
      if (stickiness.sticky_family.valid() && stickiness.sticky_family == candidate.model.family_id) {
        ++matched;
      }
    }
    if (stickiness.prefer_same_provider) {
      ++requested;
      if (stickiness.sticky_provider.valid() &&
          stickiness.sticky_provider == candidate.backend.provider_id) {
        ++matched;
      }
    }
    if (stickiness.prefer_same_backend) {
      ++requested;
      if (stickiness.sticky_backend.valid() &&
          stickiness.sticky_backend == candidate.key.backend_id) {
        ++matched;
      }
    }
    if (stickiness.prefer_context_locality) {
      ++requested;
      if (candidate.backend.trust_domain == TrustDomain::LOCAL) {
        ++matched;
      }
    }
    if (requested > 0) {
      push(known_value(RankingFactor::CONTINUITY_STICKINESS, matched,
                       mul_div(matched, kScale, requested),
                       weight_of(RankingFactor::CONTINUITY_STICKINESS)));
    } else {
      push(unknown(RankingFactor::CONTINUITY_STICKINESS,
                   weight_of(RankingFactor::CONTINUITY_STICKINESS)));
    }
  }

  // 29. Explicit caller preference.
  {
    const RouteRequirements& requirements = request.requirements;
    std::int64_t requested = 0;
    std::int64_t matched = 0;
    if (requirements.affinity_model.valid()) {
      ++requested;
      if (requirements.affinity_model == candidate.key.model_id) {
        ++matched;
      }
    }
    if (requirements.affinity_provider.valid()) {
      ++requested;
      if (requirements.affinity_provider == candidate.backend.provider_id) {
        ++matched;
      }
    }
    if (requirements.affinity_backend.valid()) {
      ++requested;
      if (requirements.affinity_backend == candidate.key.backend_id) {
        ++matched;
      }
    }
    if (requested > 0) {
      push(known_value(RankingFactor::CALLER_PREFERENCE, matched,
                       mul_div(matched, kScale, requested),
                       weight_of(RankingFactor::CALLER_PREFERENCE)));
    } else {
      push(unknown(RankingFactor::CALLER_PREFERENCE, weight_of(RankingFactor::CALLER_PREFERENCE)));
    }
  }

  // Canonical order and integer score.
  std::sort(factors.begin(), factors.end(), [](const FactorValue& lhs, const FactorValue& rhs) {
    return lhs.factor < rhs.factor;
  });

  std::int64_t score = 0;
  for (const FactorValue& factor : factors) {
    score = saturating_add(score, factor.contribution);
  }
  if (out_score != nullptr) {
    *out_score = score;
  }
  return factors;
}

}  // namespace model_router::detail
