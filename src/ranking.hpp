// Model Router - deterministic factor construction. Internal.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_SRC_RANKING_HPP
#define MODEL_ROUTER_SRC_RANKING_HPP

#include <cstdint>
#include <vector>

#include "eligibility.hpp"
#include "model_router/candidate.hpp"
#include "model_router/policy.hpp"

namespace model_router::detail {

/// Inputs for deterministic factor construction. Every reference here is owned
/// by the caller and outlives the call.
struct RankingContext {
  const RouteRequest* request{nullptr};
  const CandidateEvidence* evidence{nullptr};
  const PolicyVerdict* policy_verdict{nullptr};
  const RankingWeights* weights{nullptr};
  const RankingScales* scales{nullptr};
  UnixMillis now{0};

  /// Continuity reference: the backend that most recently won a route for this
  /// tenant, when known. Used by cache affinity and route-switch penalty.
  BackendId continuity_backend{};
  bool has_continuity_backend{false};

  /// When set, failure-domain diversity is scored against this provider.
  ProviderId diversity_reference_provider{};
  bool has_diversity_reference{false};
};

/// Builds the complete, canonically ordered factor vector for one candidate and
/// returns its integer score. Factors whose evidence is absent are marked
/// UNKNOWN and contribute exactly zero.
[[nodiscard]] std::vector<FactorValue> build_factors(const RouteCandidate& candidate,
                                                     const RankingContext& context,
                                                     std::int64_t* out_score) noexcept;

/// Integer normalization helpers with saturation. Both never overflow.
[[nodiscard]] std::int64_t normalize_higher_better(std::int64_t raw, std::int64_t reference) noexcept;
[[nodiscard]] std::int64_t normalize_lower_better(std::int64_t raw, std::int64_t reference) noexcept;

}  // namespace model_router::detail

#endif  // MODEL_ROUTER_SRC_RANKING_HPP
