// Model Router - route decisions, route plans, and dispatch records.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_DECISION_HPP
#define MODEL_ROUTER_DECISION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "model_router/authority.hpp"
#include "model_router/candidate.hpp"
#include "model_router/outcome.hpp"
#include "model_router/request.hpp"

namespace model_router {

/// Lifecycle state of a route decision. Only a CURRENT decision may be
/// dispatched, and only after pre-dispatch revalidation succeeds.
enum class RouteStatus : std::uint8_t {
  /// Ranked and currently executable (subject to revalidation).
  CURRENT = 0,
  /// A newer decision replaced this one for the same request.
  SUPERSEDED = 1,
  /// A required authority component advanced; not dispatchable.
  STALE = 2,
  /// Handed to a backend; awaiting completion observation.
  DISPATCHED = 3,
  /// Completion observed successfully.
  COMPLETED = 4,
  /// Completion observed as a failure.
  FAILED = 5,
  /// Caller cancelled the request.
  CANCELLED = 6,
  kCount
};

[[nodiscard]] std::string_view to_string(RouteStatus status) noexcept;

/// Deterministic, canonically ordered explanation of one routing decision.
struct RouteExplanation {
  RouteRequestId request_id{};
  RouteRequestGeneration request_generation{};

  CandidateKey winner{};
  std::uint32_t eligible_candidate_count{0};
  std::uint32_t rejected_candidate_count{0};

  std::vector<RouteRejection> rejections;
  std::vector<RankedCandidate> ranking;

  /// Canonical tie-break narrative for the winner.
  std::string tie_break_reason;

  RouteAuthority authority{};

  CostEvidence cost{};
  SloEvidence slo{};
  BudgetSnapshot budget{};
  PolicySnapshot policy{};

  /// Outcome of policy evaluation: ACCEPTED or REJECT_POLICY/REJECT_TRUST/...
  OutcomeCode policy_result{OutcomeCode::ACCEPTED};

  std::vector<CandidateKey> fallback_order;
  Currentness currentness{Currentness::CURRENT};
  bool revalidation_required{false};
  /// False for a decision reconstructed from durable state, where the
  /// factor-level detail is deliberately not persisted. The identity, winner,
  /// bound authority and digests remain exact.
  bool detail_retained{true};

  std::string requirement_digest;
  std::string semantic_digest;

  /// Content-derived digest of the routing semantics: the normalized
  /// requirement digest, the eligible ranking with its factor values, the typed
  /// rejections, the ordered fallback set, and the bound generation values.
  /// Identity fields (request id, decision id) are deliberately excluded, so
  /// identical canonical state yields an identical value.
  [[nodiscard]] std::string compute_semantic_digest() const;
  /// Canonical multi-line text rendering.
  [[nodiscard]] std::string to_text() const;
  /// Canonical JSON rendering.
  [[nodiscard]] std::string to_json() const;
  /// Canonicalizes every list in this explanation.
  void canonicalize();
};

/// A single authoritative routing decision for one request.
struct RouteDecision {
  RouteDecisionId decision_id{};
  RouteDecisionGeneration decision_generation{};

  RouteRequestId request_id{};
  RouteRequestGeneration request_generation{};

  RouteStatus status{RouteStatus::CURRENT};
  OutcomeCode code{OutcomeCode::ROUTED};

  RouteAuthority authority{};
  RouteExplanation explanation{};

  /// Ordered fallback candidates: an ordered set of alternative singular
  /// routes. This is not an ensemble and implies no simultaneous execution.
  std::vector<CandidateKey> fallbacks;

  DispatchGeneration dispatch_generation{};
  DispatchId dispatch_id{};

  /// Set when a reservation was bound to this route.
  ReservationEvidence reservation{};

  UnixMillis created_at_unix_millis{0};
  UnixMillis superseded_at_unix_millis{0};

  [[nodiscard]] bool dispatchable() const noexcept {
    return status == RouteStatus::CURRENT && code == OutcomeCode::ROUTED;
  }
  /// Content-derived digest of the decision's routing semantics: the
  /// requirement digest, the winner, the ordered fallback set, the outcome
  /// code, the factor-level explanation digest, and the bound generation
  /// values. Decision and request identities are deliberately excluded, so
  /// identical canonical state yields an identical value. Use
  /// authority_digest(decision.authority) for the identity-bound binding.
  [[nodiscard]] std::string semantic_digest() const;

  friend bool operator==(const RouteDecision&, const RouteDecision&) = default;
};

/// Dispatch constraints attached to a route plan.
struct DispatchConstraints {
  bool require_revalidation{true};
  bool require_reservation{false};
  std::uint32_t max_attempts{2};
  std::uint32_t max_fallbacks{2};
  /// Absolute deadline for the dispatch handoff. kNoExpiry means none.
  UnixMillis dispatch_deadline_unix_millis{kNoExpiry};

  friend bool operator==(const DispatchConstraints&, const DispatchConstraints&) = default;
};

/// A route plan: the primary candidate, an explicit ordered fallback set, the
/// bound authority, and the rules under which the route may be superseded.
struct RoutePlan {
  RoutePlanId plan_id{};
  RoutePlanGeneration plan_generation{};

  RouteDecisionId decision_id{};
  RouteDecisionGeneration decision_generation{};

  CandidateKey primary{};
  std::vector<CandidateKey> fallbacks;

  RouteAuthority authority{};
  DispatchConstraints constraints{};
  RetryReroutePolicy retry_policy{};
  FallbackPolicy fallback_policy{FallbackPolicy::EXPLICIT_ONLY};

  UnixMillis created_at_unix_millis{0};
  UnixMillis expires_at_unix_millis{kNoExpiry};

  [[nodiscard]] std::string semantic_digest() const;
  friend bool operator==(const RoutePlan&, const RoutePlan&) = default;
};

/// Result of a dispatch handoff attempt.
struct DispatchRecord {
  DispatchId dispatch_id{};
  DispatchGeneration dispatch_generation{};

  RouteDecisionId decision_id{};
  RouteDecisionGeneration decision_generation{};

  CandidateKey target{};
  OutcomeCode code{OutcomeCode::INTERNAL_ERROR};
  FailureClass failure{FailureClass::UNKNOWN};

  /// Authority comparison performed immediately before the handoff.
  AuthorityComparison revalidation{};

  bool revalidated{false};
  bool handed_off{false};

  UnixMillis dispatched_at_unix_millis{0};
  std::string detail;

  friend bool operator==(const DispatchRecord&, const DispatchRecord&) = default;
};

/// Observed completion of a dispatched route.
struct CompletionRecord {
  DispatchId dispatch_id{};
  RouteDecisionId decision_id{};

  OutcomeCode code{OutcomeCode::INTERNAL_ERROR};
  FailureClass failure{FailureClass::UNKNOWN};

  std::uint32_t observed_latency_micros{0};
  std::uint32_t observed_time_to_first_token_micros{0};

  UnixMillis observed_at_unix_millis{0};
  std::string detail;

  friend bool operator==(const CompletionRecord&, const CompletionRecord&) = default;
};

/// Result of a routing operation that may or may not produce a decision.
struct RouteOutcome {
  OutcomeCode code{OutcomeCode::INTERNAL_ERROR};
  /// Operation-level canonical explanation of the outcome itself.
  Explanation explanation;
  /// Decision-level detail: winner, ranking, per-candidate rejections, bound
  /// authority. Present even when no candidate was eligible.
  RouteExplanation route{};
  RouteDecision decision{};
  bool has_decision{false};

  [[nodiscard]] bool routed() const noexcept { return code == OutcomeCode::ROUTED; }
  [[nodiscard]] bool rejected() const noexcept { return is_rejection(code); }

  [[nodiscard]] static RouteOutcome make(OutcomeCode code_value, std::string subject,
                                         std::string message);
};

}  // namespace model_router

#endif  // MODEL_ROUTER_DECISION_HPP
