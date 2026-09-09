// Model Router - hard eligibility predicates.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Hard validity before ranking. Every predicate below requires affirmative
// proof: UNKNOWN evidence fails, and a favorable score can never rescue a
// candidate that failed here.

#include "eligibility.hpp"

#include <algorithm>

namespace model_router::detail {
namespace {

/// Requirements and policy lists are canonicalized (sorted and de-duplicated)
/// before evaluation, so membership is a binary search rather than a scan.
template <class List, class Value>
[[nodiscard]] bool contains(const List& list, Value value) {
  return std::binary_search(list.begin(), list.end(), value);
}

[[nodiscard]] bool contains_key(const std::vector<LocalityKey>& list, const LocalityKey& value) {
  return std::binary_search(list.begin(), list.end(), value);
}

[[nodiscard]] EligibilityVerdict fail(OutcomeCode code, std::string detail) {
  EligibilityVerdict verdict;
  verdict.code = code;
  verdict.detail = std::move(detail);
  return verdict;
}

[[nodiscard]] EligibilityVerdict ok() { return EligibilityVerdict{}; }

[[nodiscard]] EligibilityVerdict check_capabilities(const RouteRequest& request,
                                                    const RouteCandidate& candidate,
                                                    UnixMillis now) {
  for (const CapabilityRequirement& requirement : request.requirements.required_capabilities) {
    const CapabilityEvidence* model_evidence = candidate.model.capabilities.find(requirement.key);
    const CapabilityEvidence* backend_evidence =
        candidate.backend.capabilities.find(requirement.key);

    const auto satisfies = [now](const CapabilityEvidence* evidence,
                                 CapabilityState minimum) -> bool {
      if (evidence == nullptr) {
        return false;
      }
      if (!is_satisfying(evidence->state)) {
        return false;
      }
      if (evidence->expired(now)) {
        return false;
      }
      return evidence_strength(evidence->state) >= evidence_strength(minimum);
    };

    const bool model_ok = satisfies(model_evidence, requirement.minimum_state);
    const bool backend_ok = satisfies(backend_evidence, requirement.minimum_state);
    if (!model_ok && !backend_ok) {
      // Distinguish a missing claim from a claim that is present but weak, so
      // the explanation names the real cause.
      if (model_evidence == nullptr && backend_evidence == nullptr) {
        return fail(OutcomeCode::REJECT_CAPABILITY,
                    "capability " + requirement.key.value() + " has no evidence");
      }
      const CapabilityEvidence* present =
          model_evidence != nullptr ? model_evidence : backend_evidence;
      const CapabilityState state = present != nullptr ? present->state : CapabilityState::UNKNOWN;
      if (state == CapabilityState::UNKNOWN) {
        return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE,
                    "capability " + requirement.key.value() + " is UNKNOWN");
      }
      if (!is_satisfying(state)) {
        return fail(OutcomeCode::REJECT_CAPABILITY, "capability " + requirement.key.value() +
                                                        " is " + std::string(to_string(state)));
      }
      if (present != nullptr && present->expired(now)) {
        return fail(OutcomeCode::REJECT_CAPABILITY, "capability " + requirement.key.value() +
                                                        " evidence is stale");
      }
      return fail(OutcomeCode::REJECT_CAPABILITY,
                  "capability " + requirement.key.value() + " evidence is weaker than the "
                  "required minimum evidence state");
    }
  }
  return ok();
}

[[nodiscard]] EligibilityVerdict check_modalities(const RouteRequest& request,
                                                  const RouteCandidate& candidate) {
  if (!candidate.model.input_modalities.covers(request.requirements.required_input_modalities)) {
    return fail(OutcomeCode::REJECT_MODALITY, "model does not support a required input modality");
  }
  if (!candidate.model.output_modalities.covers(request.requirements.required_output_modalities)) {
    return fail(OutcomeCode::REJECT_MODALITY, "model does not support a required output modality");
  }
  return ok();
}

[[nodiscard]] EligibilityVerdict check_shape(const RouteRequest& request,
                                             const RouteCandidate& candidate,
                                             const CandidateEvidence& evidence,
                                             UnixMillis now) {
  const RouteRequirements& requirements = request.requirements;
  const CapabilityProfile& profile = candidate.model.capabilities;

  const auto has_capability = [&profile, now](std::string_view key) {
    const CapabilityEvidence* found = profile.find(CapabilityKey(std::string(key)));
    return found != nullptr && is_satisfying(found->state) && !found->expired(now);
  };

  if (requirements.require_structured_output &&
      !has_capability(capability_keys::structured_output)) {
    return fail(OutcomeCode::REJECT_STRUCTURED_OUTPUT,
                "structured output is required but not proven");
  }
  if (requirements.require_json_schema && !has_capability(capability_keys::json_schema)) {
    return fail(OutcomeCode::REJECT_STRUCTURED_OUTPUT, "JSON schema output is required but not proven");
  }
  if (requirements.require_tool_calling && !has_capability(capability_keys::tool_calling)) {
    return fail(OutcomeCode::REJECT_TOOL_CALLING, "tool calling is required but not proven");
  }
  if (requirements.require_streaming && !has_capability(capability_keys::streaming)) {
    return fail(OutcomeCode::REJECT_STREAMING, "streaming is required but not proven");
  }
  if (requirements.require_logprobs && !has_capability(capability_keys::logprobs)) {
    return fail(OutcomeCode::REJECT_CAPABILITY, "logprobs are required but not proven");
  }
  if (requirements.require_deterministic_seed &&
      !has_capability(capability_keys::deterministic_seed)) {
    return fail(OutcomeCode::REJECT_CAPABILITY, "deterministic seeding is required but not proven");
  }
  if (!requirements.required_protocol.empty()) {
    if (!evidence.compatibility.current(now)) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE,
                  "protocol compatibility is required but UNKNOWN or stale");
    }
    if (evidence.compatibility.protocol != requirements.required_protocol ||
        (requirements.required_protocol_version != 0 &&
         evidence.compatibility.protocol_version != requirements.required_protocol_version)) {
      return fail(OutcomeCode::REJECT_COMPATIBILITY,
                  "protocol identity or version is not compatible");
    }
  }
  return ok();
}

}  // namespace

EligibilityVerdict evaluate_hard_eligibility(const EligibilityInput& input) noexcept {
  if (input.request == nullptr || input.candidate == nullptr || input.policy == nullptr ||
      input.evidence == nullptr) {
    return fail(OutcomeCode::REJECT_INVALID, "eligibility input is incomplete");
  }
  const RouteRequest& request = *input.request;
  const RouteCandidate& candidate = *input.candidate;
  const RouteRequirements& requirements = request.requirements;
  const CandidateEvidence& evidence = *input.evidence;
  const UnixMillis now = input.now;

  if (!candidate.key.complete()) {
    return fail(OutcomeCode::REJECT_INVALID, "candidate identity is incomplete");
  }

  // --- model lifecycle ---------------------------------------------------
  if (candidate.model.lifecycle == ModelLifecycle::RETIRED) {
    return fail(OutcomeCode::REJECT_RETIRED, "model generation is retired");
  }
  if (!candidate.model.current(now)) {
    return fail(OutcomeCode::REJECT_STALE_MODEL,
                "model generation is not the current lifecycle state");
  }
  if (!candidate.key.model_generation.valid() || !candidate.key.artifact_generation.valid()) {
    return fail(OutcomeCode::REJECT_STALE_ARTIFACT, "model or artifact generation is not valid");
  }
  if (candidate.key.model_generation != candidate.model.model_generation ||
      candidate.key.artifact_generation != candidate.model.artifact_generation) {
    return fail(OutcomeCode::REJECT_STALE_MODEL,
                "candidate identity does not match the current model generation");
  }
  const ModelBinding* binding = candidate.backend.find_binding(candidate.key.model_id);
  if (binding == nullptr) {
    return fail(OutcomeCode::REJECT_STALE_MODEL, "backend no longer binds this model");
  }
  if (binding->model_generation != candidate.key.model_generation ||
      binding->artifact_generation != candidate.key.artifact_generation) {
    return fail(OutcomeCode::REJECT_STALE_MODEL,
                "backend binding is not for the current model generation");
  }

  // --- backend incarnation and lifecycle ---------------------------------
  if (candidate.backend.backend_boot != candidate.key.backend_boot ||
      candidate.backend.backend_generation != candidate.key.backend_generation) {
    return fail(OutcomeCode::REJECT_STALE_BACKEND_BOOT,
                "candidate identity does not match the current backend incarnation");
  }
  if (candidate.backend.retired()) {
    return fail(OutcomeCode::REJECT_RETIRED, "backend is retired");
  }
  if (candidate.backend.draining()) {
    return fail(OutcomeCode::REJECT_DRAINING, "backend is draining");
  }
  if (candidate.endpoint.endpoint_generation != candidate.key.endpoint_generation ||
      candidate.endpoint.endpoint_id != candidate.key.endpoint_id) {
    return fail(OutcomeCode::REJECT_STALE_ENDPOINT,
                "endpoint generation is not the current endpoint generation");
  }

  // --- availability / health / readiness (three distinct facts) ----------
  if (requirements.require_available) {
    const AvailabilityEvidence& availability = candidate.backend.availability;
    if (!availability.generation.valid() || availability.state == AvailabilityState::UNKNOWN) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE, "backend availability is UNKNOWN");
    }
    if (availability.expired(now)) {
      return fail(OutcomeCode::REJECT_STALE_AVAILABILITY, "backend availability evidence is stale");
    }
    if (availability.backend_boot != candidate.key.backend_boot) {
      return fail(OutcomeCode::REJECT_STALE_BACKEND_BOOT,
                  "backend availability evidence belongs to a different incarnation");
    }
    if (availability.state != AvailabilityState::AVAILABLE) {
      return fail(OutcomeCode::REJECT_UNAVAILABLE, "backend is not available");
    }
  }
  if (requirements.require_healthy) {
    const HealthEvidence& health = candidate.backend.health;
    if (!health.generation.valid() || health.state == HealthState::UNKNOWN) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE, "backend health is UNKNOWN");
    }
    if (health.expired(now)) {
      return fail(OutcomeCode::REJECT_STALE_HEALTH, "backend health evidence is stale");
    }
    if (health.backend_boot != candidate.key.backend_boot) {
      return fail(OutcomeCode::REJECT_STALE_BACKEND_BOOT,
                  "backend health evidence belongs to a different incarnation");
    }
    if (health.state != HealthState::HEALTHY) {
      return fail(OutcomeCode::REJECT_UNHEALTHY, "backend is not healthy");
    }
  }
  if (requirements.require_ready) {
    const ReadinessEvidence& readiness = candidate.backend.readiness;
    if (!readiness.generation.valid() || readiness.state == ReadinessState::UNKNOWN) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE, "backend readiness is UNKNOWN");
    }
    if (readiness.expired(now)) {
      return fail(OutcomeCode::REJECT_STALE_READINESS, "backend readiness evidence is stale");
    }
    if (readiness.backend_boot != candidate.key.backend_boot) {
      return fail(OutcomeCode::REJECT_STALE_BACKEND_BOOT,
                  "backend readiness evidence belongs to a different incarnation");
    }
    if (readiness.state != ReadinessState::READY) {
      return fail(OutcomeCode::REJECT_NOT_READY, "backend is not ready");
    }
  }
  if (requirements.require_resident) {
    const ResidencyEvidence& residency = candidate.backend.residency;
    if (!residency.generation.valid() || residency.state == ResidencyState::UNKNOWN) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE, "model residency is UNKNOWN");
    }
    if (residency.expired(now)) {
      return fail(OutcomeCode::REJECT_STALE_RESIDENCY, "model residency evidence is stale");
    }
    if (residency.backend_boot != candidate.key.backend_boot) {
      return fail(OutcomeCode::REJECT_STALE_BACKEND_BOOT,
                  "model residency evidence belongs to a different incarnation");
    }
    if (residency.state != ResidencyState::RESIDENT) {
      return fail(OutcomeCode::REJECT_RESIDENCY, "model is not resident");
    }
  }
  if (requirements.require_capacity) {
    const CapacityEvidence& capacity = candidate.backend.capacity;
    if (!capacity.generation.valid() || capacity.state == CapacityState::UNKNOWN) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE, "backend capacity is UNKNOWN");
    }
    if (capacity.expired(now)) {
      return fail(OutcomeCode::REJECT_STALE_CAPACITY, "backend capacity evidence is stale");
    }
    if (capacity.backend_boot != candidate.key.backend_boot) {
      return fail(OutcomeCode::REJECT_STALE_BACKEND_BOOT,
                  "backend capacity evidence belongs to a different incarnation");
    }
    if (capacity.state != CapacityState::AVAILABLE) {
      return fail(OutcomeCode::REJECT_CAPACITY, "backend capacity is saturated");
    }
  }
  if (requirements.require_reservation) {
    if (!evidence.reservation_present) {
      return fail(OutcomeCode::REJECT_RESERVATION, "a current reservation is required but absent");
    }
    if (!evidence.reservation.current(now)) {
      return fail(OutcomeCode::REJECT_STALE_RESERVATION, "reservation is stale or expired");
    }
    if (evidence.reservation.backend_id != candidate.key.backend_id ||
        evidence.reservation.backend_boot != candidate.key.backend_boot) {
      return fail(OutcomeCode::REJECT_RESERVATION,
                  "reservation is not bound to this backend incarnation");
    }
  }

  // --- capability, shape, context ----------------------------------------
  if (const EligibilityVerdict verdict = check_capabilities(request, candidate, now);
      !verdict.eligible()) {
    return verdict;
  }
  if (const EligibilityVerdict verdict = check_modalities(request, candidate); !verdict.eligible()) {
    return verdict;
  }
  if (const EligibilityVerdict verdict = check_shape(request, candidate, evidence, now);
      !verdict.eligible()) {
    return verdict;
  }
  if (requirements.min_context_tokens > candidate.context_limit_tokens) {
    return fail(OutcomeCode::REJECT_CONTEXT_LIMIT, "context limit is insufficient");
  }
  if (requirements.max_output_tokens > candidate.model.max_output_tokens) {
    return fail(OutcomeCode::REJECT_CONTEXT_LIMIT, "maximum output length exceeds the model limit");
  }

  // --- quality -----------------------------------------------------------
  if (requirements.quality_floor != kUnclassifiedQuality) {
    if (candidate.quality_class == kUnclassifiedQuality) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE,
                  "a quality floor is required but candidate quality is unclassified");
    }
    if (candidate.quality_class < requirements.quality_floor) {
      return fail(OutcomeCode::REJECT_QUALITY, "candidate quality is below the required floor");
    }
  }

  // --- identity filters --------------------------------------------------
  if (!requirements.model_allowlist.empty() &&
      !contains(requirements.model_allowlist, candidate.key.model_id)) {
    return fail(OutcomeCode::REJECT_AFFINITY, "model is not in the request allowlist");
  }
  if (contains(requirements.model_denylist, candidate.key.model_id)) {
    return fail(OutcomeCode::REJECT_AFFINITY, "model is in the request denylist");
  }
  if (!requirements.family_allowlist.empty() &&
      !contains(requirements.family_allowlist, candidate.model.family_id)) {
    return fail(OutcomeCode::REJECT_AFFINITY, "model family is not in the request allowlist");
  }
  if (contains(requirements.family_denylist, candidate.model.family_id)) {
    return fail(OutcomeCode::REJECT_AFFINITY, "model family is in the request denylist");
  }
  if (!requirements.provider_allowlist.empty() &&
      !contains(requirements.provider_allowlist, candidate.backend.provider_id)) {
    return fail(OutcomeCode::REJECT_AFFINITY, "provider is not in the request allowlist");
  }
  if (contains(requirements.provider_denylist, candidate.backend.provider_id)) {
    return fail(OutcomeCode::REJECT_AFFINITY, "provider is in the request denylist");
  }
  if (!requirements.backend_allowlist.empty() &&
      !contains(requirements.backend_allowlist, candidate.key.backend_id)) {
    return fail(OutcomeCode::REJECT_AFFINITY, "backend is not in the request allowlist");
  }
  if (contains(requirements.backend_denylist, candidate.key.backend_id)) {
    return fail(OutcomeCode::REJECT_AFFINITY, "backend is in the request denylist");
  }

  // --- placement, locality, trust ----------------------------------------
  const TrustDomain domain = candidate.backend.trust_domain;
  if (requirements.local_only && domain != TrustDomain::LOCAL) {
    return fail(OutcomeCode::REJECT_LOCALITY, "request is local-only");
  }
  if (!requirements.remote_allowed && domain != TrustDomain::LOCAL) {
    return fail(OutcomeCode::REJECT_LOCALITY, "request forbids remote placement");
  }
  if (requirements.offline_only && !candidate.backend.offline_capable) {
    return fail(OutcomeCode::REJECT_LOCALITY, "request is offline-only");
  }
  if (!requirements.required_localities.empty()) {
    if (candidate.backend.locality.empty() ||
        !contains_key(requirements.required_localities, candidate.backend.locality)) {
      return fail(OutcomeCode::REJECT_RESIDENCY, "backend locality is not an allowed locality");
    }
  }
  if (!candidate.backend.locality.empty() &&
      contains_key(requirements.denied_localities, candidate.backend.locality)) {
    return fail(OutcomeCode::REJECT_RESIDENCY, "backend locality is denied");
  }
  if (requirements.minimum_trust != TrustDomain::UNKNOWN) {
    if (domain == TrustDomain::UNKNOWN) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE,
                  "a trust floor is required but candidate trust is UNKNOWN");
    }
    if (trust_rank(domain) < trust_rank(requirements.minimum_trust)) {
      return fail(OutcomeCode::REJECT_TRUST, "candidate trust domain is below the required floor");
    }
  }

  // --- policy ------------------------------------------------------------
  const PolicyVerdict policy_verdict =
      PolicyEvaluator::evaluate_candidate(*input.policy, request, candidate, now);
  if (!policy_verdict.allowed()) {
    return fail(policy_verdict.code,
                policy_verdict.detail.empty() ? std::string("policy denied the candidate")
                                              : policy_verdict.detail);
  }

  // --- budget ------------------------------------------------------------
  if (input.budget != nullptr && request.budget_id.valid()) {
    const BudgetSnapshot& budget = *input.budget;
    if (!budget.generation.valid() || budget.verdict == BudgetVerdict::UNKNOWN) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE, "budget verdict is UNKNOWN");
    }
    if (budget.expired(now)) {
      return fail(OutcomeCode::REJECT_STALE_BUDGET, "budget evidence is stale");
    }
    if (budget.budget_id.valid() && budget.budget_id != request.budget_id) {
      return fail(OutcomeCode::REJECT_STALE_BUDGET, "budget identity does not match the request");
    }
    if (budget.generation != request.budget_generation) {
      return fail(OutcomeCode::REJECT_STALE_BUDGET,
                  "budget generation is not the request budget generation");
    }
    if (budget.verdict == BudgetVerdict::DENIED) {
      return fail(OutcomeCode::REJECT_BUDGET, "budget verdict denies the request");
    }
  }

  // --- cost --------------------------------------------------------------
  const CostEvidence& cost = evidence.cost;
  if (requirements.require_known_cost || requirements.enforce_cost_ceiling) {
    if (!cost.known()) {
      return fail(OutcomeCode::REJECT_COST_UNKNOWN,
                  "cost evidence is UNKNOWN and the request requires a known cost");
    }
    if (!cost.current(now)) {
      return fail(OutcomeCode::REJECT_STALE_PRICE, "cost evidence is stale or has no unit identity");
    }
  }
  if (requirements.enforce_cost_ceiling) {
    if (!cost.unit.compatible_with(requirements.maximum_cost.unit)) {
      return fail(OutcomeCode::REJECT_COST,
                  "candidate cost unit is not compatible with the request ceiling unit");
    }
    if (cost.estimated_total_micros > requirements.maximum_cost.micros) {
      return fail(OutcomeCode::REJECT_COST, "estimated request cost exceeds the ceiling");
    }
  }

  // --- SLO ---------------------------------------------------------------
  if (requirements.require_slo) {
    const SloEvidence& slo = evidence.slo;
    if (!slo.generation.valid() || slo.verdict == SloVerdict::UNKNOWN) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE, "SLO verdict is UNKNOWN");
    }
    if (slo.expired(now)) {
      return fail(OutcomeCode::REJECT_STALE_SLO, "SLO evidence is stale");
    }
    if (slo.slo_id.valid() && slo.slo_id != request.slo_id) {
      return fail(OutcomeCode::REJECT_STALE_SLO, "SLO identity does not match the request");
    }
    if (slo.generation != request.slo_generation) {
      return fail(OutcomeCode::REJECT_STALE_SLO,
                  "SLO generation is not the request SLO generation");
    }
    if (slo.verdict == SloVerdict::INFEASIBLE) {
      return fail(OutcomeCode::REJECT_SLO, "SLO is infeasible for this candidate");
    }
    if (!evidence.slo_verdict_known) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE,
                  "per-candidate SLO feasibility is UNKNOWN");
    }
    if (evidence.slo_verdict == SloVerdict::INFEASIBLE) {
      return fail(OutcomeCode::REJECT_SLO, "per-candidate SLO feasibility is infeasible");
    }
    if (evidence.slo_verdict == SloVerdict::UNKNOWN) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE,
                  "per-candidate SLO feasibility is UNKNOWN");
    }
  }

  // --- latency -----------------------------------------------------------
  if (requirements.require_known_latency &&
      candidate.backend.latency.dispatch_micros == LatencyEvidence::kUnknownLatencyMicros) {
    return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE,
                "latency evidence is required but UNKNOWN");
  }
  if (requirements.max_latency_micros != SloEvidence::kUnspecified) {
    if (candidate.backend.latency.dispatch_micros == LatencyEvidence::kUnknownLatencyMicros) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE,
                  "a latency ceiling is required but latency is UNKNOWN");
    }
    if (candidate.backend.latency.expired(now)) {
      return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE, "latency evidence is stale");
    }
    if (candidate.backend.latency.dispatch_micros > requirements.max_latency_micros) {
      return fail(OutcomeCode::REJECT_SLO, "observed latency exceeds the request ceiling");
    }
  }

  // --- compatibility -----------------------------------------------------
  if (requirements.require_known_compatibility && !evidence.compatibility.current(now)) {
    return fail(OutcomeCode::REJECT_UNKNOWN_EVIDENCE,
                "compatibility evidence is required but UNKNOWN or stale");
  }
  if (requirements.required_protocol.empty() && evidence.compatibility.generation.valid() &&
      evidence.compatibility.verdict == CompatibilityVerdict::INCOMPATIBLE) {
    return fail(OutcomeCode::REJECT_COMPATIBILITY, "compatibility evidence is incompatible");
  }

  // --- quality/cost semantics guard --------------------------------------
  // A cheaper candidate never defeats a hard capability requirement because the
  // capability predicates above already ran. Nothing here may override them.
  return ok();
}

}  // namespace model_router::detail
