// Model Router - pre-dispatch revalidation, dispatch handoff, completion, reroute.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <utility>

#include "router_impl.hpp"

namespace model_router {
namespace {

using detail::CandidateEvidence;
using detail::EligibilityInput;
using detail::RankingContext;

/// Reads the authority currently observable for one candidate identity. Returns
/// false when the identity no longer exists in canonical state.
}  // namespace

bool ModelRouter::Impl::current_authority(const Impl& impl, const RouteAuthority& bound,
                                          RouteAuthority* out) {
  std::shared_lock lock(impl.mutex);
  return detail::build_current_authority(impl.state, bound, out);
}

/// Builds a decision for exactly one freshly validated candidate. Used by the
/// policy-bound fallback path, where the candidate has already been revalidated
/// on its own.
RouteOutcome ModelRouter::Impl::commit_single_candidate(Impl& impl, const RouteRequest& request,
                                                   const DiscoveryContext& context,
                                                   RouteCandidate candidate,
                                                   CandidateEvidence evidence,
                                                   const PolicySnapshot& policy,
                                                   const BudgetSnapshot& budget,
                                                   const SloEvidence& slo,
                                                   const RankingWeights& weights,
                                                   const RankingScales& scales,
                                                   RouterEpoch epoch,
                                                   CoordinatorEpoch coordinator_epoch,
                                                   const std::vector<CandidateKey>& fallbacks,
                                                   std::uint32_t permitted_fallbacks,
                                                   BackendId continuity, bool has_continuity) {
  const UnixMillis now = context.now_unix_millis;
  candidate.required_capabilities_total =
      static_cast<std::uint32_t>(request.requirements.required_capabilities.size());
  candidate.required_capabilities_satisfied = 0;
  for (const CapabilityRequirement& requirement : request.requirements.required_capabilities) {
    const CapabilityEvidence* model_evidence = candidate.model.capabilities.find(requirement.key);
    const CapabilityEvidence* backend_evidence =
        candidate.backend.capabilities.find(requirement.key);
    const auto satisfies = [now](const CapabilityEvidence* entry, CapabilityState minimum) {
      return entry != nullptr && is_satisfying(entry->state) && !entry->expired(now) &&
             evidence_strength(entry->state) >= evidence_strength(minimum);
    };
    if (satisfies(model_evidence, requirement.minimum_state) ||
        satisfies(backend_evidence, requirement.minimum_state)) {
      ++candidate.required_capabilities_satisfied;
    }
  }
  candidate.preferred_capabilities_total =
      static_cast<std::uint32_t>(request.requirements.preferred_capabilities.size());
  candidate.preferred_capabilities_satisfied = 0;
  for (const CapabilityKey& key : request.requirements.preferred_capabilities) {
    const CapabilityEvidence* model_evidence = candidate.model.capabilities.find(key);
    const CapabilityEvidence* backend_evidence = candidate.backend.capabilities.find(key);
    const auto satisfies = [now](const CapabilityEvidence* entry) {
      return entry != nullptr && is_satisfying(entry->state) && !entry->expired(now);
    };
    if (satisfies(model_evidence) || satisfies(backend_evidence)) {
      ++candidate.preferred_capabilities_satisfied;
    }
  }

  EligibilityInput eligibility;
  eligibility.request = &request;
  eligibility.candidate = &candidate;
  eligibility.policy = &policy;
  eligibility.budget = &budget;
  eligibility.evidence = &evidence;
  eligibility.now = now;
  const detail::EligibilityVerdict verdict = detail::evaluate_hard_eligibility(eligibility);
  if (!verdict.eligible()) {
    RouteOutcome outcome = RouteOutcome::make(verdict.code, "reroute", verdict.detail);
    outcome.route.request_id = request.request_id;
    outcome.route.request_generation = request.request_generation;
    return outcome;
  }

  const PolicyVerdict policy_verdict =
      PolicyEvaluator::evaluate_candidate(policy, request, candidate, now);
  RankingContext ranking;
  ranking.request = &request;
  ranking.evidence = &evidence;
  ranking.policy_verdict = &policy_verdict;
  ranking.weights = &weights;
  ranking.scales = &scales;
  ranking.now = now;
  ranking.continuity_backend = continuity;
  ranking.has_continuity_backend = has_continuity;

  RankedCandidate ranked;
  ranked.key = candidate.key;
  ranked.factors = detail::build_factors(candidate, ranking, &ranked.score);
  ranked.rank = 1;

  RouteExplanation explanation;
  explanation.request_id = request.request_id;
  explanation.request_generation = request.request_generation;
  explanation.winner = candidate.key;
  explanation.eligible_candidate_count = 1;
  explanation.rejected_candidate_count = 0;
  explanation.ranking.push_back(ranked);
  explanation.policy = policy;
  explanation.budget = budget;
  explanation.slo = slo;
  explanation.cost = evidence.cost;
  explanation.policy_result = policy_verdict.code;
  explanation.requirement_digest = request.requirements.digest();
  explanation.currentness = Currentness::CURRENT;
  explanation.tie_break_reason = "single freshly revalidated fallback candidate";

  RouteAuthority authority;
  authority.router_id = impl.state.router_id;
  authority.router_epoch = epoch;
  authority.coordinator_epoch = coordinator_epoch;
  authority.request_id = request.request_id;
  authority.request_generation = request.request_generation;
  authority.model_id = candidate.key.model_id;
  authority.model_generation = candidate.key.model_generation;
  authority.artifact_generation = candidate.key.artifact_generation;
  authority.provider_id = candidate.backend.provider_id;
  authority.provider_generation = candidate.backend.provider_generation;
  authority.backend_id = candidate.key.backend_id;
  authority.backend_generation = candidate.key.backend_generation;
  authority.backend_boot = candidate.key.backend_boot;
  authority.backend_registration_generation = candidate.backend.backend_registration_generation;
  authority.endpoint_id = candidate.key.endpoint_id;
  authority.endpoint_generation = candidate.key.endpoint_generation;
  authority.capability_profile_id = candidate.backend.capability_profile_id;
  authority.capability_generation = candidate.backend.capability_generation;
  authority.compatibility_profile_id = candidate.backend.compatibility_profile_id;
  authority.compatibility_generation = candidate.backend.compatibility_generation;
  authority.trust_profile_id = candidate.backend.trust_profile_id;
  authority.trust_generation = candidate.backend.trust_generation;
  authority.health_generation = candidate.backend.health.generation;
  authority.availability_generation = candidate.backend.availability.generation;
  authority.readiness_generation = candidate.backend.readiness.generation;
  authority.residency_generation = candidate.backend.residency.generation;
  authority.capacity_generation = candidate.backend.capacity.generation;
  authority.policy_id = policy.policy_id;
  authority.policy_generation = policy.generation;
  if (request.budget_id.valid()) {
    authority.budget_id = request.budget_id;
    authority.budget_generation = budget.generation;
  }
  authority.slo_id = slo.slo_id;
  authority.slo_generation = slo.generation;
  authority.cost_evidence_id = evidence.cost.evidence_id;
  authority.price_generation = evidence.cost.price_generation;
  authority.tenant = request.tenant;
  authority.name_space = request.name_space;
  authority.issued_at_unix_millis = now;
  authority.expires_at_unix_millis = request.expires_at_unix_millis;
  if (evidence.reservation_present) {
    authority.reservation_id = evidence.reservation.reservation_id;
    authority.reservation_generation = evidence.reservation.generation;
  }
  explanation.authority = authority;

  for (const CandidateKey& key : fallbacks) {
    if (explanation.fallback_order.size() >= permitted_fallbacks) {
      break;
    }
    if (key == candidate.key) {
      continue;
    }
    explanation.fallback_order.push_back(key);
  }

  RouteDecision decision;
  decision.request_id = request.request_id;
  decision.request_generation = request.request_generation;
  decision.status = RouteStatus::CURRENT;
  decision.code = OutcomeCode::ROUTED;
  decision.authority = authority;
  decision.fallbacks = explanation.fallback_order;
  decision.created_at_unix_millis = now;
  decision.explanation = explanation;

  bool committed_current = true;
  {
    std::unique_lock lock(impl.mutex);
    if (impl.state.shutting_down) {
      return RouteOutcome::make(OutcomeCode::SHUTTING_DOWN, "reroute",
                                "router is shutting down");
    }
    if (!(impl.state.router_epoch == epoch) ||
        !(impl.state.coordinator_epoch == coordinator_epoch)) {
      return RouteOutcome::make(OutcomeCode::REJECT_STALE_ROUTER_EPOCH, "reroute",
                                "router epoch advanced before the reroute could be committed");
    }
    while (impl.state.route_order.size() >= impl.state.limits.max_route_history &&
           !impl.state.route_order.empty()) {
      const RouteDecisionId victim = impl.state.route_order.front();
      impl.state.route_order.pop_front();
      impl.state.routes.erase(victim);
    }
    decision.decision_id = allocate_id<RouteDecisionTag>();
    decision.decision_generation = Generation<RouteDecisionTag>(1);
    decision.authority.decision_id = decision.decision_id;
    decision.authority.decision_generation = decision.decision_generation;
    decision.explanation.authority = decision.authority;
    {
      RouteAuthority observed;
      const bool identity_current =
          detail::build_current_authority(impl.state, decision.authority, &observed);
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
    record.plan.primary = candidate.key;
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
    impl.state.routes.emplace(decision.decision_id, std::move(record));
    impl.state.route_order.push_back(decision.decision_id);
    const auto inserted = impl.state.routes.find(decision.decision_id);
    if (inserted != impl.state.routes.end()) {
      impl.state.stale_decision_if_not_dispatchable(inserted->second, now);
      if (inserted->second.decision.status != RouteStatus::CURRENT) {
        committed_current = false;
      }
    }
    ++impl.state.counters.route_decisions;
    ++impl.state.counters.reroutes;
    ++impl.state.counters.fallbacks_taken;
    impl.state.last_eligible = 1;
    impl.state.last_rejected = 0;
    impl.state.last_winner_by_tenant[request.tenant] = candidate.key.backend_id;
    impl.state.note_tenant(request.tenant);
  }

  RouteOutcome outcome;
  outcome.code = committed_current ? OutcomeCode::ROUTED : OutcomeCode::REVALIDATION_REQUIRED;
  outcome.decision = decision;
  outcome.route = decision.explanation;
  outcome.has_decision = true;
  ExplanationBuilder builder(outcome.code, "reroute");
  builder.add_u64("decision_id", decision.decision_id.value());
  builder.add_u64("backend_id", candidate.key.backend_id.value());
  builder.add_u64("backend_boot", candidate.key.backend_boot.value());
  builder.add_bool("committed_current", committed_current);
  builder.set_message(committed_current
                          ? "fallback candidate freshly revalidated and committed"
                          : "fallback committed stale: authority advanced during validation");
  outcome.explanation = builder.build();
  return outcome;
}

MutationResult ModelRouter::Impl::revalidate(RouteDecisionId decision_id, TenantId caller_tenant,
                                             NamespaceId caller_namespace) {
  detail::RouteLookup record;
  bool fenced = false;
  {
    std::shared_lock lock(mutex);
    if (state.shutting_down) {
      return reject(OutcomeCode::SHUTTING_DOWN, "revalidate",
                    "router is shutting down and cannot re-authorize a route");
    }
    const auto found = state.routes.find(decision_id);
    if (found == state.routes.end()) {
      return reject(OutcomeCode::REJECT_INVALID, "revalidate", "route decision is not retained");
    }
    record = detail::RouteLookup::from(found->second);
    // The fence check reads canonical state, so it is taken under the same lock.
    fenced = is_fenced(record.authority.backend_id, record.authority.backend_boot);
  }
  if (caller_tenant.valid() && !(caller_tenant == record.tenant)) {
    return reject(OutcomeCode::REJECT_CONFLICT, "revalidate",
                  "route decision belongs to a different tenant");
  }
  if (caller_namespace.valid() && record.name_space.valid() &&
      !(caller_namespace == record.name_space)) {
    return reject(OutcomeCode::REJECT_CONFLICT, "revalidate",
                  "route decision belongs to a different namespace");
  }

  const detail::RouteLookup& decision = record;
  if (decision.status == RouteStatus::SUPERSEDED || decision.status == RouteStatus::CANCELLED) {
    return reject(OutcomeCode::SUPERSEDED, "revalidate", "route decision is no longer current");
  }
  if (decision.status == RouteStatus::COMPLETED || decision.status == RouteStatus::FAILED) {
    return reject(OutcomeCode::REJECT_INVALID, "revalidate",
                  "route decision has already reached a terminal state");
  }

  const UnixMillis now = this->now();
  if (decision.authority.expired(now)) {
    std::unique_lock lock(mutex);
    const auto found = state.routes.find(decision_id);
    if (found != state.routes.end()) {
      found->second.decision.status = RouteStatus::STALE;
      found->second.decision.explanation.currentness = Currentness::STALE;
      found->second.decision.explanation.revalidation_required = true;
    }
    return reject(OutcomeCode::REJECT_STALE_REQUEST, "revalidate", "route decision has expired");
  }

  if (fenced) {
    std::unique_lock lock(mutex);
    const auto found = state.routes.find(decision_id);
    if (found != state.routes.end()) {
      found->second.decision.status = RouteStatus::STALE;
      found->second.decision.explanation.currentness = Currentness::STALE;
      found->second.decision.explanation.revalidation_required = true;
    }
    ExplanationBuilder builder(OutcomeCode::REJECT_STALE_BACKEND_BOOT, "revalidate");
    builder.add_u64("backend_id", decision.authority.backend_id.value());
    builder.add_u64("backend_boot", decision.authority.backend_boot.value());
    builder.set_message("backend incarnation is fenced");
    return MutationResult(OutcomeCode::REJECT_STALE_BACKEND_BOOT, builder.build());
  }

  RouteAuthority observed;
  if (!current_authority(*this, decision.authority, &observed)) {
    std::unique_lock lock(mutex);
    const auto found = state.routes.find(decision_id);
    if (found != state.routes.end()) {
      found->second.decision.status = RouteStatus::STALE;
      found->second.decision.explanation.currentness = Currentness::STALE;
      found->second.decision.explanation.revalidation_required = true;
    }
    return reject(OutcomeCode::REJECT_STALE_BACKEND, "revalidate",
                  "bound model or backend identity no longer exists");
  }

  const AuthorityComparison comparison =
      compare_authority(decision.authority, observed, decision.authority.required_mask());

  std::unique_lock lock(mutex);
  const auto found = state.routes.find(decision_id);
  if (found == state.routes.end()) {
    return reject(OutcomeCode::REJECT_INVALID, "revalidate", "route decision is not retained");
  }
  // Lifecycle and availability facts can change while every generation stays
  // the same, so the full dispatchability predicate decides, not the
  // generation comparison alone.
  if (comparison.current()) {
    const OutcomeCode reason = detail::dispatchability_reason(state, decision.authority, now);
    if (reason != OutcomeCode::ACCEPTED) {
      found->second.decision.status = RouteStatus::STALE;
      found->second.decision.explanation.currentness = Currentness::STALE;
      found->second.decision.explanation.revalidation_required = true;
      ExplanationBuilder builder(reason, "revalidate");
      builder.add_u64("decision_id", decision_id.value());
      builder.set_message("bound model or backend is no longer dispatchable");
      return MutationResult(reason, builder.build());
    }
  }
  if (!comparison.current()) {
    found->second.decision.status = RouteStatus::STALE;
    found->second.decision.explanation.currentness = Currentness::STALE;
    found->second.decision.explanation.revalidation_required = true;
    ExplanationBuilder builder(comparison.code, "revalidate");
    builder.add_u64("decision_id", decision_id.value());
    builder.add_u64("differences", comparison.differences.size());
    for (const AuthorityDifference& difference : comparison.differences) {
      builder.add(std::string(to_string(difference.component)),
                  std::to_string(difference.bound) + "->" + std::to_string(difference.current));
    }
    builder.set_message("route authority is no longer current");
    return MutationResult(comparison.code, builder.build());
  }

  found->second.decision.status = RouteStatus::CURRENT;
  found->second.decision.explanation.currentness = Currentness::CURRENT;
  found->second.decision.explanation.revalidation_required = false;
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "revalidate");
  builder.add_u64("decision_id", decision_id.value());
  builder.add("authority_digest", authority_digest(observed));
  builder.set_message("every required authority component is current");
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

DispatchRecord ModelRouter::Impl::dispatch(RouteDecisionId decision_id, TenantId caller_tenant,
                                           NamespaceId caller_namespace) {
  DispatchRecord record;
  record.decision_id = decision_id;
  record.code = OutcomeCode::INTERNAL_ERROR;

  detail::RouteRecord stored;
  {
    std::shared_lock lock(mutex);
    if (state.shutting_down) {
      record.code = OutcomeCode::SHUTTING_DOWN;
      record.detail = "router is shutting down and cannot dispatch";
      return record;
    }
    const auto found = state.routes.find(decision_id);
    if (found == state.routes.end()) {
      record.code = OutcomeCode::REJECT_INVALID;
      record.detail = "route decision is not retained";
      return record;
    }
    stored = found->second;
  }
  record.decision_generation = stored.decision.decision_generation;
  record.target = stored.plan.primary;

  if (caller_tenant.valid() && !(caller_tenant == stored.tenant)) {
    record.code = OutcomeCode::REJECT_CONFLICT;
    record.detail = "route decision belongs to a different tenant";
    return record;
  }
  if (caller_namespace.valid() && stored.name_space.valid() &&
      !(caller_namespace == stored.name_space)) {
    record.code = OutcomeCode::REJECT_CONFLICT;
    record.detail = "route decision belongs to a different namespace";
    return record;
  }

  // Pre-dispatch revalidation of every hard-current authority component.
  const MutationResult revalidated = revalidate(decision_id, caller_tenant, caller_namespace);
  record.revalidated = true;
  if (!revalidated.accepted()) {
    record.code = revalidated.code;
    record.failure = classify_failure(revalidated.code);
    record.detail = revalidated.explanation.message();
    record.dispatched_at_unix_millis = now();
    return record;
  }

  // The stored verdict is not reused: the target is re-resolved and every hard
  // predicate is re-evaluated against current evidence.
  RouteRequest request;
  PolicySnapshot policy;
  BudgetSnapshot budget;
  SloEvidence slo;
  RankingWeights weights;
  RankingScales scales;
  DiscoveryContext context;
  ResourceLimits limits;
  RouterEpoch epoch;
  CoordinatorEpoch coordinator_epoch;
  EvidenceSnapshot evidence_snapshot;
  {
    std::shared_lock lock(mutex);
    limits = state.limits;
    weights = state.weights;
    scales = state.scales;
    policy = state.policy;
    budget = state.budget;
    slo = state.slo;
    epoch = state.router_epoch;
    coordinator_epoch = state.coordinator_epoch;
    evidence_snapshot = EvidenceSnapshot::capture(state);
    context.router_id = state.router_id;
    context.router_epoch = epoch;
    context.coordinator_epoch = coordinator_epoch;
    context.tenant = stored.tenant;
    context.name_space = stored.name_space;
    context.now_unix_millis = now();
    context.limits = &limits;
    request.request_id = stored.decision.request_id;
    request.request_generation = stored.decision.request_generation;
    request.tenant = stored.tenant;
    request.name_space = stored.name_space;
    request.policy_id = stored.decision.authority.policy_id;
    request.policy_generation = stored.decision.authority.policy_generation;
    request.budget_id = stored.decision.authority.budget_id;
    request.budget_generation = stored.decision.authority.budget_generation;
    request.slo_id = stored.decision.authority.slo_id;
    request.slo_generation = stored.decision.authority.slo_generation;
    request.expires_at_unix_millis = stored.decision.authority.expires_at_unix_millis;
  }

  RouteCandidate candidate;
  if (!build_candidate_from_catalog(request, record.target.model_id, record.target.backend_id,
                                    &candidate)) {
    record.code = OutcomeCode::REJECT_STALE_BACKEND;
    record.failure = FailureClass::STALE_ROUTE;
    record.detail = "bound candidate identity no longer exists";
    return record;
  }
  if (!(candidate.key == record.target)) {
    record.code = OutcomeCode::REJECT_STALE_BACKEND_BOOT;
    record.failure = FailureClass::BACKEND_RESTARTED;
    record.detail = "bound candidate identity is not the current incarnation";
    return record;
  }

  CandidateEvidence evidence;
  resolve_candidate(request, context, evidence_snapshot, &candidate, &evidence);
  EligibilityInput eligibility;
  eligibility.request = &request;
  eligibility.candidate = &candidate;
  eligibility.policy = &policy;
  eligibility.budget = &budget;
  eligibility.evidence = &evidence;
  eligibility.now = context.now_unix_millis;
  const detail::EligibilityVerdict verdict = detail::evaluate_hard_eligibility(eligibility);
  if (!verdict.eligible()) {
    record.code = verdict.code;
    record.failure = classify_failure(verdict.code);
    record.detail = verdict.detail;
    record.dispatched_at_unix_millis = now();
    return record;
  }

  if (options.providers.dispatcher == nullptr) {
    record.code = OutcomeCode::REJECT_INVALID;
    record.failure = FailureClass::UNKNOWN;
    record.detail = "no dispatch handler is configured";
    record.dispatched_at_unix_millis = now();
    return record;
  }

  // Handoff happens outside every canonical lock.
  const DispatchRecord handler_record =
      options.providers.dispatcher->handoff(stored.decision, stored.plan, context);
  record = handler_record;
  record.decision_id = decision_id;
  record.decision_generation = stored.decision.decision_generation;
  record.target = candidate.key;
  record.revalidated = true;
  if (record.dispatched_at_unix_millis == 0) {
    record.dispatched_at_unix_millis = now();
  }
  if (record.code != OutcomeCode::DISPATCHED && record.code != OutcomeCode::ACCEPTED) {
    record.handed_off = false;
    record.failure = record.failure == FailureClass::UNKNOWN ? classify_failure(record.code)
                                                             : record.failure;
    return record;
  }
  record.code = OutcomeCode::DISPATCHED;
  record.handed_off = true;

  std::unique_lock lock(mutex);
  const auto found = state.routes.find(decision_id);
  if (found == state.routes.end()) {
    record.code = OutcomeCode::REJECT_INVALID;
    record.detail = "route decision disappeared before the dispatch could be recorded";
    return record;
  }
  if (!(state.router_epoch == epoch) || !(state.coordinator_epoch == coordinator_epoch)) {
    record.code = OutcomeCode::REJECT_STALE_ROUTER_EPOCH;
    record.failure = FailureClass::STALE_ROUTE;
    record.handed_off = false;
    record.detail = "router epoch advanced during the dispatch handoff";
    return record;
  }
  record.dispatch_id = allocate_id<DispatchTag>();
  record.dispatch_generation = Generation<DispatchTag>(1);
  found->second.decision.status = RouteStatus::DISPATCHED;
  found->second.decision.dispatch_id = record.dispatch_id;
  found->second.decision.dispatch_generation = record.dispatch_generation;
  state.dispatches[decision_id] = record;
  state.dispatch_index[record.dispatch_id] = decision_id;
  ++state.counters.dispatches;
  return record;
}

MutationResult ModelRouter::Impl::record_completion(const CompletionRecord& completion) {
  std::unique_lock lock(mutex);
  RouteDecisionId decision_id = completion.decision_id;
  if (completion.dispatch_id.valid()) {
    const auto mapped = state.dispatch_index.find(completion.dispatch_id);
    if (mapped == state.dispatch_index.end()) {
      return reject(OutcomeCode::REJECT_INVALID, "record_completion",
                    "dispatch identity is not retained");
    }
    decision_id = mapped->second;
  }
  const auto found = state.routes.find(decision_id);
  if (found == state.routes.end()) {
    return reject(OutcomeCode::REJECT_INVALID, "record_completion",
                  "route decision is not retained");
  }
  detail::RouteRecord& record = found->second;
  if (record.decision.status != RouteStatus::DISPATCHED) {
    return reject(OutcomeCode::REJECT_CONFLICT, "record_completion",
                  "route decision was not dispatched");
  }
  const bool success = completion.code == OutcomeCode::COMPLETED ||
                       completion.code == OutcomeCode::ACCEPTED;
  record.decision.status = success ? RouteStatus::COMPLETED : RouteStatus::FAILED;
  const auto dispatch = state.dispatches.find(decision_id);
  if (dispatch != state.dispatches.end()) {
    dispatch->second.code = completion.code;
    dispatch->second.failure = completion.failure;
  }
  ExplanationBuilder builder(record.decision.status == RouteStatus::COMPLETED
                                 ? OutcomeCode::COMPLETED
                                 : OutcomeCode::FAILED,
                             "record_completion");
  builder.add_u64("decision_id", decision_id.value());
  builder.add("failure", std::string(to_string(completion.failure)));
  builder.add_u64("observed_latency_micros", completion.observed_latency_micros);
  return MutationResult(record.decision.status == RouteStatus::COMPLETED
                            ? OutcomeCode::COMPLETED
                            : OutcomeCode::FAILED,
                        builder.build());
}

RouteOutcome ModelRouter::Impl::reroute(RouteDecisionId decision_id, FailureClass failure,
                                        RouteRequest request) {
  request.requirements.canonicalize();
  if (!request.request_id.valid()) {
    request.request_id = allocate_id<RouteRequestTag>();
  }
  if (!request.request_generation.valid()) {
    request.request_generation = Generation<RouteRequestTag>(1);
  }

  detail::RouteRecord stored;
  ResourceLimits limits;
  DiscoveryContext context;
  PolicySnapshot policy;
  BudgetSnapshot budget;
  SloEvidence slo;
  RankingWeights weights;
  RankingScales scales;
  RouterEpoch epoch;
  CoordinatorEpoch coordinator_epoch;
  EvidenceSnapshot snapshot;
  BackendId continuity;
  bool has_continuity = false;
  {
    std::shared_lock lock(mutex);
    const auto found = state.routes.find(decision_id);
    if (found == state.routes.end()) {
      return RouteOutcome::make(OutcomeCode::REJECT_INVALID, "reroute",
                                "route decision is not retained");
    }
    stored = found->second;
    if (state.shutting_down) {
      return RouteOutcome::make(OutcomeCode::SHUTTING_DOWN, "reroute", "router is shutting down");
    }
    limits = state.limits;
    weights = state.weights;
    scales = state.scales;
    policy = state.policy;
    budget = state.budget;
    slo = state.slo;
    epoch = state.router_epoch;
    coordinator_epoch = state.coordinator_epoch;
    context.router_id = state.router_id;
    context.router_epoch = epoch;
    context.coordinator_epoch = coordinator_epoch;
    context.tenant = stored.tenant;
    context.name_space = stored.name_space;
    context.now_unix_millis = now();
    context.limits = &limits;
    snapshot = EvidenceSnapshot::capture(state);
    const auto continuity_iter = state.last_winner_by_tenant.find(stored.tenant);
    if (continuity_iter != state.last_winner_by_tenant.end()) {
      continuity = continuity_iter->second;
      has_continuity = true;
    }
  }

  if (stored.decision.status == RouteStatus::COMPLETED ||
      stored.decision.status == RouteStatus::CANCELLED) {
    return RouteOutcome::make(OutcomeCode::REJECT_INVALID, "reroute",
                              "route decision has reached a terminal state");
  }
  if (!(request.tenant == stored.tenant)) {
    return RouteOutcome::make(OutcomeCode::REJECT_CONFLICT, "reroute",
                              "reroute request tenant does not match the route decision tenant");
  }
  if (request.expired(context.now_unix_millis)) {
    return RouteOutcome::make(OutcomeCode::REJECT_STALE_REQUEST, "reroute",
                              "reroute request has expired");
  }

  if (options.providers.policy != nullptr) {
    const EvidenceResult<PolicySnapshot> result = options.providers.policy->fetch(
        request.policy_id, request.policy_generation, context);
    if (result.ok()) {
      policy = result.value;
    } else {
      return RouteOutcome::make(result.code, "reroute", result.detail);
    }
  }
  policy.canonicalize();
  const PolicyVerdict request_policy = PolicyEvaluator::evaluate_request(policy, request,
                                                                        context.now_unix_millis);
  if (!request_policy.allowed()) {
    return RouteOutcome::make(request_policy.code, "reroute", request_policy.detail);
  }
  if (!PolicyEvaluator::fallback_permitted(policy, request.requirements, stored.decision.code)) {
    return RouteOutcome::make(OutcomeCode::REJECT_POLICY, "reroute",
                              "fallback is not permitted for this failure by policy");
  }
  if (is_permanent(failure)) {
    return RouteOutcome::make(OutcomeCode::REJECT_POLICY, "reroute",
                              "the failure class is permanent and may not be rerouted");
  }

  const std::uint32_t permitted =
      policy.fallback_policy == FallbackPolicy::FORBIDDEN
          ? 0u
          : std::min(request.requirements.retry_policy.max_fallbacks,
                     std::min(policy.max_fallback_depth, limits.max_fallback_candidates));

  // Walk the ordered fallback set: each candidate must pass its own fresh
  // validation before it may be used.
  for (const CandidateKey& key : stored.plan.fallbacks) {
    RouteCandidate candidate;
    if (!build_candidate_from_catalog(request, key.model_id, key.backend_id, &candidate)) {
      continue;
    }
    if (!(candidate.key == key)) {
      continue;
    }
    CandidateEvidence evidence;
    resolve_candidate(request, context, snapshot, &candidate, &evidence);
    RouteOutcome outcome = commit_single_candidate(
        *this, request, context, candidate, evidence, policy, budget, slo, weights, scales, epoch,
        coordinator_epoch, stored.plan.fallbacks, permitted, continuity, has_continuity);
    if (outcome.code == OutcomeCode::ROUTED) {
      std::unique_lock lock(mutex);
      const auto found = state.routes.find(decision_id);
      if (found != state.routes.end()) {
        found->second.decision.status = RouteStatus::SUPERSEDED;
        found->second.decision.superseded_at_unix_millis = context.now_unix_millis;
      }
      return outcome;
    }
  }

  // No fallback remained valid: a fresh full routing pass is the only legal
  // option, and it is still policy-bound.
  RouteOutcome fresh = route(request);
  if (fresh.code == OutcomeCode::ROUTED) {
    std::unique_lock lock(mutex);
    const auto found = state.routes.find(decision_id);
    if (found != state.routes.end()) {
      found->second.decision.status = RouteStatus::SUPERSEDED;
      found->second.decision.superseded_at_unix_millis = context.now_unix_millis;
    }
    ++state.counters.reroutes;
    return fresh;
  }
  return fresh;
}

}  // namespace model_router
