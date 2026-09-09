// Model Router - hard eligibility predicates. Internal.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_SRC_ELIGIBILITY_HPP
#define MODEL_ROUTER_SRC_ELIGIBILITY_HPP

#include <string>

#include "model_router/candidate.hpp"
#include "model_router/evidence.hpp"
#include "model_router/policy.hpp"
#include "model_router/request.hpp"

namespace model_router::detail {

/// Evidence resolved for one candidate for one request. Every field is explicit:
/// a missing value is reported by its own knownness flag rather than by a
/// sentinel that could be mistaken for a favorable value.
struct CandidateEvidence {
  CostEvidence cost{};
  CompatibilityEvidence compatibility{};
  TrustEvidence trust{};
  SloEvidence slo{};
  SloVerdict slo_verdict{SloVerdict::UNKNOWN};
  bool slo_verdict_known{false};
  ReservationEvidence reservation{};
  bool reservation_present{false};
};

/// Everything a hard-eligibility decision depends on.
struct EligibilityInput {
  const RouteRequest* request{nullptr};
  const RouteCandidate* candidate{nullptr};
  const PolicySnapshot* policy{nullptr};
  const BudgetSnapshot* budget{nullptr};
  const CandidateEvidence* evidence{nullptr};
  UnixMillis now{0};
};

/// Result of hard eligibility. ACCEPTED means every hard predicate passed; any
/// other code is a typed hard-rejection reason. UNKNOWN evidence never passes a
/// predicate that requires affirmative proof.
struct EligibilityVerdict {
  OutcomeCode code{OutcomeCode::ACCEPTED};
  std::string detail;

  [[nodiscard]] bool eligible() const noexcept { return code == OutcomeCode::ACCEPTED; }
};

/// Runs every hard predicate in a fixed order and returns the first failure, so
/// the rejection reason for identical state is always identical.
[[nodiscard]] EligibilityVerdict evaluate_hard_eligibility(const EligibilityInput& input) noexcept;

}  // namespace model_router::detail

#endif  // MODEL_ROUTER_SRC_ELIGIBILITY_HPP
