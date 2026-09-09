// Model Router - the routing pipeline.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Admission, requirement normalization, candidate discovery, hard eligibility,
// external evidence validation, deterministic factor construction, deterministic
// ranking, route-plan creation, route authority binding, and route-decision
// commit. No canonical lock is held across an adapter call.

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "router_impl.hpp"

namespace model_router {
namespace {

using detail::CandidateEvidence;
using detail::EligibilityInput;
using detail::RankingContext;

[[nodiscard]] SloVerdict derive_slo_feasibility(const RouteCandidate& candidate,
                                                const SloEvidence& slo, UnixMillis now) {
  const std::uint32_t target = slo.latency_target_micros != SloEvidence::kUnspecified
                                   ? slo.latency_target_micros
                                   : SloEvidence::kUnspecified;
  if (target == SloEvidence::kUnspecified || target == 0) {
    return SloVerdict::UNKNOWN;
  }
  const LatencyEvidence& latency = candidate.backend.latency;
  if (latency.expired(now) || latency.dispatch_micros == LatencyEvidence::kUnknownLatencyMicros) {
    return SloVerdict::UNKNOWN;
  }
  return latency.dispatch_micros <= target ? SloVerdict::FEASIBLE : SloVerdict::INFEASIBLE;
}

}  // namespace

bool ModelRouter::Impl::build_candidate_from_catalog(const RouteRequest& request, ModelId model_id,
                                                     BackendId backend_id,
                                                     RouteCandidate* out) const {
  (void)request;
  std::shared_lock lock(mutex);
  const auto model = state.models.find(model_id);
  const auto backend = state.backends.find(backend_id);
  if (model == state.models.end() || backend == state.backends.end()) {
    return false;
  }
  const ModelBinding* binding = backend->second.find_binding(model_id);
  if (binding == nullptr) {
    return false;
  }
  RouteCandidate candidate;
  candidate.key.model_id = model->second.model_id;
  candidate.key.model_generation = model->second.model_generation;
  candidate.key.artifact_generation = model->second.artifact_generation;
  candidate.key.backend_id = backend->second.backend_id;
  candidate.key.backend_generation = backend->second.backend_generation;
  candidate.key.backend_boot = backend->second.backend_boot;
  candidate.key.endpoint_id = backend->second.endpoint.endpoint_id;
  candidate.key.endpoint_generation = backend->second.endpoint.endpoint_generation;
  candidate.model = model->second;
  candidate.backend = backend->second;
  candidate.endpoint = backend->second.endpoint;
  candidate.context_limit_tokens = binding->context_limit_tokens != 0
                                       ? binding->context_limit_tokens
                                       : model->second.context_limit_tokens;
  candidate.quality_class = binding->quality_class != kUnclassifiedQuality
                                ? binding->quality_class
                                : model->second.quality_class;
  candidate.discovery_source = "catalog";
  *out = std::move(candidate);
  return true;
}

ModelRouter::Impl::EvidenceSnapshot ModelRouter::Impl::EvidenceSnapshot::capture(
    const detail::CanonicalState& state) {
  EvidenceSnapshot snapshot;
  snapshot.compatibility = state.compatibility;
  snapshot.trust = state.trust;
  snapshot.reservations = state.reservations;
  snapshot.slo = state.slo;
  return snapshot;
}

std::vector<RouteCandidate> ModelRouter::Impl::discover(const RouteRequest& request,
                                                        const DiscoveryContext& context) {
  if (options.providers.candidates != nullptr) {
    const EvidenceResult<std::vector<RouteCandidate>> result =
        options.providers.candidates->discover(request, context);
    if (!result.ok()) {
      return {};
    }
    return result.value;
  }
  (void)request;

  // Built-in catalog discovery under a single shared lock. Discovery filters
  // only impossible identity cases; every semantic predicate belongs to hard
  // eligibility.
  std::vector<RouteCandidate> candidates;
  std::shared_lock lock(mutex);
  candidates.reserve(state.index_backends_by_model.size() * 2);
  for (const auto& [model_id, backend_ids] : state.index_backends_by_model) {
    const auto model = state.models.find(model_id);
    if (model == state.models.end()) {
      continue;
    }
    for (const BackendId backend_id : backend_ids) {
      const auto backend = state.backends.find(backend_id);
      if (backend == state.backends.end()) {
        continue;
      }
      const ModelBinding* binding = backend->second.find_binding(model_id);
      if (binding == nullptr) {
        continue;
      }
      RouteCandidate candidate;
      candidate.key.model_id = model->second.model_id;
      candidate.key.model_generation = model->second.model_generation;
      candidate.key.artifact_generation = model->second.artifact_generation;
      candidate.key.backend_id = backend->second.backend_id;
      candidate.key.backend_generation = backend->second.backend_generation;
      candidate.key.backend_boot = backend->second.backend_boot;
      candidate.key.endpoint_id = backend->second.endpoint.endpoint_id;
      candidate.key.endpoint_generation = backend->second.endpoint.endpoint_generation;
      candidate.model = model->second;
      candidate.backend = backend->second;
      candidate.endpoint = backend->second.endpoint;
      candidate.context_limit_tokens = binding->context_limit_tokens != 0
                                           ? binding->context_limit_tokens
                                           : model->second.context_limit_tokens;
      candidate.quality_class = binding->quality_class != kUnclassifiedQuality
                                    ? binding->quality_class
                                    : model->second.quality_class;
      candidate.discovery_source = "catalog";
      candidates.push_back(std::move(candidate));
    }
  }
  return candidates;
}

void ModelRouter::Impl::resolve_candidate(const RouteRequest& request,
                                          const DiscoveryContext& context,
                                          const EvidenceSnapshot& snapshot,
                                          RouteCandidate* candidate,
                                          CandidateEvidence* evidence) const {

  // Cost evidence.
  const ModelBinding* binding = candidate->backend.find_binding(candidate->key.model_id);
  if (options.providers.cost != nullptr) {
    const EvidenceResult<CostEvidence> result =
        options.providers.cost->fetch(*candidate, request, context);
    if (result.ok()) {
      evidence->cost = result.value;
    } else {
      evidence->cost = CostEvidence{};
    }
  } else if (binding != nullptr) {
    evidence->cost = binding->cost;
  }
  if (!evidence->cost.backend_id.valid()) {
    evidence->cost.backend_id = candidate->key.backend_id;
    evidence->cost.model_id = candidate->key.model_id;
  }

  // Compatibility evidence.
  if (options.providers.compatibility != nullptr) {
    const EvidenceResult<CompatibilityEvidence> result =
        options.providers.compatibility->fetch(*candidate, request, context);
    if (result.ok()) {
      evidence->compatibility = result.value;
    } else {
      evidence->compatibility = CompatibilityEvidence{};
    }
  } else {
    const auto found = snapshot.compatibility.find(candidate->key.backend_id);
    if (found != snapshot.compatibility.end()) {
      evidence->compatibility = found->second;
    }
  }

  // Trust evidence: explicit, generation-bound, never inferred from a hostname.
  if (options.providers.trust != nullptr) {
    const EvidenceResult<TrustEvidence> result =
        options.providers.trust->fetch(*candidate, context);
    if (result.ok()) {
      evidence->trust = result.value;
    } else {
      evidence->trust = TrustEvidence{};
    }
  } else {
    evidence->trust.profile_id = candidate->backend.trust_profile_id;
    evidence->trust.generation = candidate->backend.trust_generation;
    evidence->trust.backend_id = candidate->key.backend_id;
    evidence->trust.domain = candidate->backend.trust_domain;
  }

  // SLO evidence.
  evidence->slo = snapshot.slo;
  if (options.providers.slo != nullptr && request.slo_id.valid()) {
    const EvidenceResult<SloEvidence> result = options.providers.slo->fetch(
        request.slo_id, request.slo_generation, request, context);
    if (result.ok()) {
      evidence->slo = result.value;
    } else {
      evidence->slo = SloEvidence{};
    }
  }
  if (options.providers.slo != nullptr) {
    const EvidenceResult<SloVerdict> feasibility =
        options.providers.slo->feasibility(*candidate, evidence->slo, request, context);
    evidence->slo_verdict_known = feasibility.ok();
    evidence->slo_verdict = feasibility.ok() ? feasibility.value : SloVerdict::UNKNOWN;
  } else {
    const SloVerdict derived =
        derive_slo_feasibility(*candidate, evidence->slo, context.now_unix_millis);
    evidence->slo_verdict_known = derived != SloVerdict::UNKNOWN;
    evidence->slo_verdict = derived;
  }

  // Reservation.
  if (options.providers.reservation != nullptr) {
    const EvidenceResult<ReservationEvidence> result =
        options.providers.reservation->reserve(*candidate, request, context);
    evidence->reservation_present = result.ok();
    if (result.ok()) {
      evidence->reservation = result.value;
    }
  } else {
    const auto found = snapshot.reservations.find(candidate->key.backend_id);
    if (found != snapshot.reservations.end()) {
      evidence->reservation_present = true;
      evidence->reservation = found->second;
    }
  }
}

RouteOutcome ModelRouter::Impl::route(RouteRequest request) {
  request.requirements.canonicalize();
  if (!request.request_id.valid()) {
    request.request_id = allocate_id<RouteRequestTag>();
  }
  if (!request.request_generation.valid()) {
    request.request_generation = Generation<RouteRequestTag>(1);
  }
  const UnixMillis now = this->now();
  if (request.created_at_unix_millis == 0) {
    request.created_at_unix_millis = now;
  }

  ResourceLimits limits;
  DiscoveryContext context;
  PolicySnapshot policy;
  BudgetSnapshot budget;
  SloEvidence slo;
  RouterEpoch epoch;
  CoordinatorEpoch coordinator_epoch;
  RouterId router_id;
  BackendId continuity;
  bool has_continuity = false;
  RankingWeights weights;
  RankingScales scales;
  EvidenceSnapshot snapshot_evidence;
  bool recovered = false;

  {
    std::shared_lock lock(mutex);
    if (state.shutting_down) {
      return RouteOutcome::make(OutcomeCode::SHUTTING_DOWN, "route",
                                "router is shutting down and cannot admit a route request");
    }
    if (!state.running) {
      return RouteOutcome::make(OutcomeCode::REJECT_INVALID, "route",
                                "router is not running");
    }
    limits = state.limits;
    weights = state.weights;
    scales = state.scales;
    policy = state.policy;
    budget = state.budget;
    slo = state.slo;
    epoch = state.router_epoch;
    coordinator_epoch = state.coordinator_epoch;
    router_id = state.router_id;
    recovered = state.recovered;
    context.router_id = router_id;
    context.router_epoch = epoch;
    context.coordinator_epoch = coordinator_epoch;
    context.tenant = request.tenant;
    context.name_space = request.name_space;
    context.now_unix_millis = now;
    context.limits = &limits;
    const auto continuity_iter = state.last_winner_by_tenant.find(request.tenant);
    if (continuity_iter != state.last_winner_by_tenant.end()) {
      continuity = continuity_iter->second;
      has_continuity = true;
    }
    snapshot_evidence = EvidenceSnapshot::capture(state);
  }
  (void)recovered;

  // --- admission and requirement normalization ---------------------------
  if (const std::string error = request.validate(limits); !error.empty()) {
    return RouteOutcome::make(OutcomeCode::REJECT_INVALID, "route", error);
  }
  if (request.expired(now)) {
    return RouteOutcome::make(OutcomeCode::REJECT_STALE_REQUEST, "route",
                              "route request has expired");
  }

  // --- external policy validation ----------------------------------------
  if (options.providers.policy != nullptr) {
    const EvidenceResult<PolicySnapshot> result = options.providers.policy->fetch(
        request.policy_id, request.policy_generation, context);
    if (!result.ok()) {
      return RouteOutcome::make(result.code, "route",
                                result.detail.empty() ? "policy evidence is unavailable"
                                                      : result.detail);
    }
    policy = result.value;
  }
  policy.canonicalize();
  if (!policy.generation.valid()) {
    return RouteOutcome::make(OutcomeCode::REJECT_STALE_POLICY, "route",
                              "no current policy evidence is available");
  }
  const PolicyVerdict request_policy = PolicyEvaluator::evaluate_request(policy, request, now);
  if (!request_policy.allowed()) {
    return RouteOutcome::make(request_policy.code, "route", request_policy.detail);
  }

  // --- external budget validation ----------------------------------------
  if (request.budget_id.valid()) {
    if (options.providers.budget != nullptr) {
      const EvidenceResult<BudgetSnapshot> result = options.providers.budget->fetch(
          request.budget_id, request.budget_generation, request, context);
      if (!result.ok()) {
        return RouteOutcome::make(result.code, "route",
                                  result.detail.empty() ? "budget evidence is unavailable"
                                                        : result.detail);
      }
      budget = result.value;
    }
    if (!budget.generation.valid() || budget.verdict == BudgetVerdict::UNKNOWN) {
      return RouteOutcome::make(OutcomeCode::REJECT_UNKNOWN_EVIDENCE, "route",
                                "budget evidence is UNKNOWN");
    }
    if (budget.generation != request.budget_generation) {
      return RouteOutcome::make(OutcomeCode::REJECT_STALE_BUDGET, "route",
                                "budget generation is not the request budget generation");
    }
    if (budget.verdict == BudgetVerdict::DENIED) {
      return RouteOutcome::make(OutcomeCode::REJECT_BUDGET, "route",
                                "budget verdict denies the request");
    }
  }

  // --- external SLO validation -------------------------------------------
  if (request.requirements.require_slo) {
    if (options.providers.slo != nullptr) {
      const EvidenceResult<SloEvidence> result = options.providers.slo->fetch(
          request.slo_id, request.slo_generation, request, context);
      if (!result.ok()) {
        return RouteOutcome::make(result.code, "route",
                                  result.detail.empty() ? "SLO evidence is unavailable"
                                                        : result.detail);
      }
      slo = result.value;
    }
    if (!slo.generation.valid() || slo.verdict == SloVerdict::UNKNOWN) {
      return RouteOutcome::make(OutcomeCode::REJECT_UNKNOWN_EVIDENCE, "route",
                                "SLO evidence is UNKNOWN");
    }
    if (slo.generation != request.slo_generation) {
      return RouteOutcome::make(OutcomeCode::REJECT_STALE_SLO, "route",
                                "SLO generation is not the request SLO generation");
    }
    if (slo.verdict == SloVerdict::INFEASIBLE) {
      return RouteOutcome::make(OutcomeCode::REJECT_SLO, "route",
                                "SLO verdict is infeasible");
    }
  }

  // --- candidate discovery -----------------------------------------------
  std::vector<RouteCandidate> candidates = discover(request, context);
  if (candidates.size() > limits.max_candidates_per_request) {
    return RouteOutcome::make(OutcomeCode::REJECT_LIMIT, "route",
                              "candidate discovery exceeded the configured limit");
  }
  // Canonicalize: identical semantic state must produce an identical result
  // regardless of discovery order.
  std::sort(candidates.begin(), candidates.end(),
            [](const RouteCandidate& lhs, const RouteCandidate& rhs) {
              return lhs.key < rhs.key;
            });
  candidates.erase(std::unique(candidates.begin(), candidates.end(),
                               [](const RouteCandidate& lhs, const RouteCandidate& rhs) {
                                 return lhs.key == rhs.key;
                               }),
                   candidates.end());

  // --- hard eligibility, then deterministic ranking ----------------------
  RouteExplanation explanation;
  explanation.request_id = request.request_id;
  explanation.request_generation = request.request_generation;
  explanation.policy = policy;
  explanation.budget = budget;
  explanation.slo = slo;
  explanation.requirement_digest = request.requirements.digest();
  explanation.policy_result = request_policy.code;

  std::vector<RankedCandidate> ranked;
  std::vector<RouteRejection> rejections;

  for (const RouteCandidate& candidate : candidates) {
    CandidateEvidence evidence;
    resolve_candidate(request, context, snapshot_evidence,
                      const_cast<RouteCandidate*>(&candidate), &evidence);

    RouteCandidate resolved = candidate;
    resolved.required_capabilities_total =
        static_cast<std::uint32_t>(request.requirements.required_capabilities.size());
    resolved.required_capabilities_satisfied = 0;
    for (const CapabilityRequirement& requirement : request.requirements.required_capabilities) {
      const CapabilityEvidence* model_evidence = resolved.model.capabilities.find(requirement.key);
      const CapabilityEvidence* backend_evidence =
          resolved.backend.capabilities.find(requirement.key);
      const auto satisfies = [now](const CapabilityEvidence* entry, CapabilityState minimum) {
        return entry != nullptr && is_satisfying(entry->state) && !entry->expired(now) &&
               evidence_strength(entry->state) >= evidence_strength(minimum);
      };
      if (satisfies(model_evidence, requirement.minimum_state) ||
          satisfies(backend_evidence, requirement.minimum_state)) {
        ++resolved.required_capabilities_satisfied;
      }
    }
    resolved.preferred_capabilities_total =
        static_cast<std::uint32_t>(request.requirements.preferred_capabilities.size());
    resolved.preferred_capabilities_satisfied = 0;
    for (const CapabilityKey& key : request.requirements.preferred_capabilities) {
      const CapabilityEvidence* model_evidence = resolved.model.capabilities.find(key);
      const CapabilityEvidence* backend_evidence = resolved.backend.capabilities.find(key);
      const auto satisfies = [now](const CapabilityEvidence* entry) {
        return entry != nullptr && is_satisfying(entry->state) && !entry->expired(now);
      };
      if (satisfies(model_evidence) || satisfies(backend_evidence)) {
        ++resolved.preferred_capabilities_satisfied;
      }
    }

    EligibilityInput eligibility;
    eligibility.request = &request;
    eligibility.candidate = &resolved;
    eligibility.policy = &policy;
    eligibility.budget = &budget;
    eligibility.evidence = &evidence;
    eligibility.now = now;

    const detail::EligibilityVerdict verdict = detail::evaluate_hard_eligibility(eligibility);
    if (!verdict.eligible()) {
      RouteRejection rejection;
      rejection.model_id = resolved.key.model_id.value();
      rejection.model_generation = resolved.key.model_generation;
      rejection.backend_id = resolved.key.backend_id.value();
      rejection.backend_generation = resolved.key.backend_generation;
      rejection.backend_boot = resolved.key.backend_boot;
      rejection.endpoint_id = resolved.key.endpoint_id.value();
      rejection.code = verdict.code;
      rejection.detail = verdict.detail;
      rejections.push_back(std::move(rejection));
      continue;
    }

    const PolicyVerdict policy_verdict =
        PolicyEvaluator::evaluate_candidate(policy, request, resolved, now);

    RankingContext ranking;
    ranking.request = &request;
    ranking.evidence = &evidence;
    ranking.policy_verdict = &policy_verdict;
    ranking.weights = &weights;
    ranking.scales = &scales;
    ranking.now = now;
    ranking.continuity_backend = continuity;
    ranking.has_continuity_backend = has_continuity;

    RankedCandidate ranked_candidate;
    ranked_candidate.key = resolved.key;
    ranked_candidate.factors = detail::build_factors(resolved, ranking, &ranked_candidate.score);
    ranked.push_back(std::move(ranked_candidate));
  }

  if (ranked.size() > limits.max_ranked_candidates) {
    return RouteOutcome::make(OutcomeCode::REJECT_LIMIT, "route",
                              "eligible candidate count exceeds the configured ranking limit");
  }
  if (rejections.size() > limits.max_rejection_records) {
    rejections.resize(limits.max_rejection_records);
  }

  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const RankedCandidate& lhs, const RankedCandidate& rhs) {
                     return candidate_ranks_before(lhs, rhs);
                   });
  for (std::size_t index = 0; index < ranked.size(); ++index) {
    ranked[index].rank = static_cast<std::uint32_t>(index + 1);
  }

  explanation.eligible_candidate_count = static_cast<std::uint32_t>(ranked.size());
  explanation.rejected_candidate_count = static_cast<std::uint32_t>(rejections.size());
  explanation.rejections = std::move(rejections);
  explanation.ranking = std::move(ranked);

  if (explanation.ranking.empty()) {
    RouteOutcome outcome =
        RouteOutcome::make(OutcomeCode::NO_ELIGIBLE_CANDIDATE, "route",
                           "no candidate satisfies every current hard requirement");
    outcome.route = explanation;
    outcome.route.eligible_candidate_count = 0;
    {
      std::unique_lock lock(mutex);
      ++state.counters.route_requests;
      ++state.counters.route_rejections;
      state.last_eligible = 0;
      state.last_rejected = outcome.route.rejected_candidate_count;
      state.note_tenant(request.tenant);
    }
    return outcome;
  }

  const RankedCandidate& winner = explanation.ranking.front();

  // --- route authority binding -------------------------------------------
  RouteAuthority authority;
  authority.router_id = router_id;
  authority.router_epoch = epoch;
  authority.coordinator_epoch = coordinator_epoch;
  authority.request_id = request.request_id;
  authority.request_generation = request.request_generation;
  authority.model_id = winner.key.model_id;
  authority.model_generation = winner.key.model_generation;
  authority.artifact_generation = winner.key.artifact_generation;
  authority.backend_id = winner.key.backend_id;
  authority.backend_generation = winner.key.backend_generation;
  authority.backend_boot = winner.key.backend_boot;
  authority.endpoint_id = winner.key.endpoint_id;
  authority.endpoint_generation = winner.key.endpoint_generation;
  authority.tenant = request.tenant;
  authority.name_space = request.name_space;
  authority.issued_at_unix_millis = now;
  authority.expires_at_unix_millis = request.expires_at_unix_millis;

  {
    std::shared_lock lock(mutex);
    const auto backend = state.backends.find(winner.key.backend_id);
    if (backend != state.backends.end()) {
      authority.provider_id = backend->second.provider_id;
      authority.provider_generation = backend->second.provider_generation;
      authority.backend_registration_generation = backend->second.backend_registration_generation;
      authority.capability_profile_id = backend->second.capability_profile_id;
      authority.capability_generation = backend->second.capability_generation;
      authority.compatibility_profile_id = backend->second.compatibility_profile_id;
      authority.compatibility_generation = backend->second.compatibility_generation;
      authority.trust_profile_id = backend->second.trust_profile_id;
      authority.trust_generation = backend->second.trust_generation;
      authority.health_generation = backend->second.health.generation;
      authority.availability_generation = backend->second.availability.generation;
      authority.readiness_generation = backend->second.readiness.generation;
      authority.residency_generation = backend->second.residency.generation;
      authority.capacity_generation = backend->second.capacity.generation;
    }
    authority.policy_id = policy.policy_id;
    authority.policy_generation = policy.generation;
    if (request.budget_id.valid()) {
      authority.budget_id = request.budget_id;
      authority.budget_generation = budget.generation;
    }
    authority.slo_id = slo.slo_id;
    authority.slo_generation = slo.generation;
  }

  // Cost, price, and reservation generations for the winner.
  {
    const auto winner_candidate =
        std::lower_bound(candidates.begin(), candidates.end(), winner.key,
                         [](const RouteCandidate& candidate, const CandidateKey& key) {
                           return candidate.key < key;
                         });
    if (winner_candidate != candidates.end() && winner_candidate->key == winner.key) {
      RouteCandidate copy = *winner_candidate;
      CandidateEvidence winner_evidence;
      resolve_candidate(request, context, snapshot_evidence, &copy, &winner_evidence);
      authority.cost_evidence_id = winner_evidence.cost.evidence_id;
      authority.price_generation = winner_evidence.cost.price_generation;
      explanation.cost = winner_evidence.cost;
      if (winner_evidence.reservation_present) {
        authority.reservation_id = winner_evidence.reservation.reservation_id;
        authority.reservation_generation = winner_evidence.reservation.generation;
      }
    }
  }

  explanation.winner = winner.key;
  explanation.authority = authority;
  explanation.currentness = Currentness::CURRENT;
  explanation.revalidation_required = false;
  explanation.tie_break_reason =
      explanation.ranking.size() > 1 && explanation.ranking[0].score == explanation.ranking[1].score
          ? "canonical factor sequence then canonical candidate identity"
          : "higher integer score";

  // --- ordered fallback set ----------------------------------------------
  const std::uint32_t requested_fallbacks =
      std::min(request.requirements.retry_policy.max_fallbacks, limits.max_fallback_candidates);
  const std::uint32_t permitted_fallbacks =
      policy.fallback_policy == FallbackPolicy::FORBIDDEN
          ? 0u
          : std::min(requested_fallbacks, policy.max_fallback_depth);
  std::vector<RankedCandidate> tail(explanation.ranking.begin() + 1, explanation.ranking.end());
  if (permitted_fallbacks > 0) {
    // Prefer a different failure domain after the primary, then rank order.
    std::stable_sort(tail.begin(), tail.end(),
                     [&winner](const RankedCandidate& lhs, const RankedCandidate& rhs) {
                       const bool left_same = lhs.key.backend_id == winner.key.backend_id;
                       const bool right_same = rhs.key.backend_id == winner.key.backend_id;
                       if (left_same != right_same) {
                         return !left_same;
                       }
                       return false;
                     });
    for (std::size_t index = 0; index < tail.size() && index < permitted_fallbacks; ++index) {
      explanation.fallback_order.push_back(tail[index].key);
    }
  }

  // --- commit -------------------------------------------------------------
  RouteDecision decision;
  decision.request_id = request.request_id;
  decision.request_generation = request.request_generation;
  decision.status = RouteStatus::CURRENT;
  decision.code = OutcomeCode::ROUTED;
  decision.authority = authority;
  decision.fallbacks = explanation.fallback_order;
  decision.created_at_unix_millis = now;
  decision.explanation = explanation;
  decision.explanation.semantic_digest = "";

  bool committed_current = true;
  {
    std::unique_lock lock(mutex);
    if (state.shutting_down) {
      return RouteOutcome::make(OutcomeCode::SHUTTING_DOWN, "route",
                                "router began shutting down before the route could be committed");
    }
    if (!(state.router_epoch == epoch) || !(state.coordinator_epoch == coordinator_epoch)) {
      return RouteOutcome::make(OutcomeCode::REJECT_STALE_ROUTER_EPOCH, "route",
                                "router epoch advanced before the route could be committed");
    }
    if (state.routes.size() >= state.limits.max_route_history) {
      while (state.route_order.size() >= state.limits.max_route_history && !state.route_order.empty()) {
        const RouteDecisionId victim = state.route_order.front();
        state.route_order.pop_front();
        state.routes.erase(victim);
      }
    }
    decision.decision_id = allocate_id<RouteDecisionTag>();
    decision.decision_generation = Generation<RouteDecisionTag>(1);
    // The bound authority names the decision it authorizes.
    decision.authority.decision_id = decision.decision_id;
    decision.authority.decision_generation = decision.decision_generation;
    decision.explanation.authority = decision.authority;
    // A concurrent authority change must not produce a decision that is
    // already stale the instant it is committed.
    {
      RouteAuthority observed;
      const bool identity_current =
          detail::build_current_authority(state, decision.authority, &observed);
      const AuthorityComparison comparison =
          identity_current
              ? compare_authority(decision.authority, observed,
                                  decision.authority.required_mask())
              : AuthorityComparison{OutcomeCode::REJECT_STALE_BACKEND, {}};
      if (!comparison.current()) {
        decision.status = RouteStatus::STALE;
        decision.explanation.currentness = Currentness::STALE;
        decision.explanation.revalidation_required = true;
        committed_current = false;
      }
    }
    decision.explanation.semantic_digest = decision.explanation.compute_semantic_digest();

    detail::RouteRecord record;
    record.decision = decision;
    record.tenant = request.tenant;
    record.name_space = request.name_space;
    record.plan.plan_id = allocate_id<RoutePlanTag>();
    record.plan.plan_generation = Generation<RoutePlanTag>(1);
    record.plan.decision_id = decision.decision_id;
    record.plan.decision_generation = decision.decision_generation;
    record.plan.primary = winner.key;
    record.plan.fallbacks = decision.fallbacks;
    record.plan.authority = authority;
    record.plan.constraints.require_revalidation = true;
    record.plan.constraints.require_reservation = request.requirements.require_reservation;
    record.plan.constraints.max_attempts = request.requirements.retry_policy.max_attempts;
    record.plan.constraints.max_fallbacks = permitted_fallbacks;
    record.plan.retry_policy = request.requirements.retry_policy;
    record.plan.fallback_policy = policy.fallback_policy;
    record.plan.created_at_unix_millis = now;
    record.plan.expires_at_unix_millis = request.expires_at_unix_millis;

    state.routes.emplace(decision.decision_id, std::move(record));
    state.route_order.push_back(decision.decision_id);
    // The decision is inserted as CURRENT; a mutation that landed during the
    // pass must not leave it current. Only this record is re-checked, so the
    // commit path stays independent of the retained history size.
    const auto inserted = state.routes.find(decision.decision_id);
    if (inserted != state.routes.end()) {
      state.stale_decision_if_not_dispatchable(inserted->second, now);
      if (inserted->second.decision.status != RouteStatus::CURRENT) {
        committed_current = false;
      }
    }
    ++state.counters.route_requests;
    ++state.counters.route_decisions;
    state.last_eligible = explanation.eligible_candidate_count;
    state.last_rejected = explanation.rejected_candidate_count;
    state.last_winner_by_tenant[request.tenant] = winner.key.backend_id;
    state.note_tenant(request.tenant);
  }

  RouteOutcome outcome;
  outcome.code = committed_current ? OutcomeCode::ROUTED : OutcomeCode::REVALIDATION_REQUIRED;
  outcome.decision = decision;
  outcome.route = decision.explanation;
  outcome.has_decision = true;
  ExplanationBuilder builder(outcome.code, "route");
  builder.add_u64("decision_id", decision.decision_id.value());
  builder.add_u64("eligible", explanation.eligible_candidate_count);
  builder.add_u64("rejected", explanation.rejected_candidate_count);
  builder.add_u64("model_id", winner.key.model_id.value());
  builder.add_u64("backend_id", winner.key.backend_id.value());
  builder.add_u64("backend_boot", winner.key.backend_boot.value());
  builder.add_u64("score", winner.score);
  builder.add_bool("committed_current", committed_current);
  builder.add("authority_digest", authority_digest(decision.authority));
  builder.set_message(committed_current
                          ? "route decision committed"
                          : "route decision committed stale: authority advanced during the pass");
  outcome.explanation = builder.build();
  return outcome;
}

}  // namespace model_router
