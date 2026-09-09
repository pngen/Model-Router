// Model Router - declarative policy evaluation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/policy.hpp"

#include <algorithm>

namespace model_router {
namespace {

/// Policy lists are canonicalized before evaluation, so membership is a binary
/// search rather than a scan.
template <class List, class Value>
[[nodiscard]] bool contains(const List& list, Value value) {
  return std::binary_search(list.begin(), list.end(), value);
}

template <class List>
[[nodiscard]] bool contains_key(const List& list, const LocalityKey& value) {
  return std::binary_search(list.begin(), list.end(), value);
}

[[nodiscard]] PolicyVerdict reject(OutcomeCode code, std::string detail) {
  PolicyVerdict verdict;
  verdict.code = code;
  verdict.detail = std::move(detail);
  return verdict;
}

}  // namespace

PolicyVerdict PolicyEvaluator::evaluate_request(const PolicySnapshot& policy,
                                                const RouteRequest& request,
                                                UnixMillis now) noexcept {
  if (!policy.generation.valid()) {
    return reject(OutcomeCode::REJECT_STALE_POLICY, "policy generation is not valid");
  }
  if (!policy.current(now)) {
    return reject(OutcomeCode::REJECT_STALE_POLICY, "policy evidence is stale");
  }
  if (policy.policy_id.valid() && policy.policy_id != request.policy_id) {
    return reject(OutcomeCode::REJECT_STALE_POLICY,
                  "request names a different policy identity than the current snapshot");
  }
  if (policy.generation != request.policy_generation) {
    return reject(OutcomeCode::REJECT_STALE_POLICY,
                  "request policy generation is not the current policy generation");
  }
  if (!policy.bound_tenants.empty() && !contains(policy.bound_tenants, request.tenant)) {
    return reject(OutcomeCode::REJECT_CONFLICT,
                  "tenant is not bound to the current policy snapshot");
  }
  PolicyVerdict verdict;
  return verdict;
}

PolicyVerdict PolicyEvaluator::evaluate_candidate(const PolicySnapshot& policy,
                                                  const RouteRequest& request,
                                                  const RouteCandidate& candidate,
                                                  UnixMillis now) noexcept {
  (void)request;
  if (!policy.generation.valid() || !policy.current(now)) {
    return reject(OutcomeCode::REJECT_STALE_POLICY, "policy evidence is not current");
  }

  // Hard identity denials come first: a denylist entry always wins.
  if (contains(policy.denied_models, candidate.key.model_id)) {
    return reject(OutcomeCode::REJECT_POLICY, "policy denies the model identity");
  }
  if (candidate.model.family_id.valid() &&
      contains(policy.denied_model_families, candidate.model.family_id)) {
    return reject(OutcomeCode::REJECT_POLICY, "policy denies the model family");
  }
  if (contains(policy.denied_providers, candidate.backend.provider_id)) {
    return reject(OutcomeCode::REJECT_POLICY, "policy denies the provider");
  }
  if (contains(policy.denied_backends, candidate.key.backend_id)) {
    return reject(OutcomeCode::REJECT_POLICY, "policy denies the backend");
  }
  if (!candidate.backend.locality.empty() &&
      contains_key(policy.denied_localities, candidate.backend.locality)) {
    return reject(OutcomeCode::REJECT_LOCALITY, "policy denies the deployment locality");
  }

  // Allowlists: when a policy allowlist is present, an unlisted identity is
  // denied unless the policy explicitly permits unlisted identities.
  if (!policy.allowed_models.empty() &&
      !contains(policy.allowed_models, candidate.key.model_id)) {
    return reject(OutcomeCode::REJECT_POLICY, "model is not in the policy allowlist");
  }
  if (policy.allowed_models.empty() && !policy.allow_unlisted_models) {
    return reject(OutcomeCode::REJECT_POLICY, "policy denies models that are not explicitly listed");
  }
  if (!policy.allowed_providers.empty() &&
      !contains(policy.allowed_providers, candidate.backend.provider_id)) {
    return reject(OutcomeCode::REJECT_POLICY, "provider is not in the policy allowlist");
  }
  if (policy.allowed_providers.empty() && !policy.allow_unlisted_providers) {
    return reject(OutcomeCode::REJECT_POLICY,
                  "policy denies providers that are not explicitly listed");
  }
  if (!policy.allowed_backends.empty() &&
      !contains(policy.allowed_backends, candidate.key.backend_id)) {
    return reject(OutcomeCode::REJECT_POLICY, "backend is not in the policy allowlist");
  }
  if (policy.allowed_backends.empty() && !policy.allow_unlisted_backends) {
    return reject(OutcomeCode::REJECT_POLICY,
                  "policy denies backends that are not explicitly listed");
  }
  if (!policy.allowed_localities.empty()) {
    if (candidate.backend.locality.empty() ||
        !contains_key(policy.allowed_localities, candidate.backend.locality)) {
      return reject(OutcomeCode::REJECT_LOCALITY, "locality is not in the policy allowlist");
    }
  }

  // Placement.
  const TrustDomain domain = candidate.backend.trust_domain;
  if (policy.local_only && domain != TrustDomain::LOCAL) {
    return reject(OutcomeCode::REJECT_LOCALITY, "policy is local-only");
  }
  if (policy.deny_public_remote && domain == TrustDomain::PUBLIC_REMOTE) {
    return reject(OutcomeCode::REJECT_TRUST, "policy denies public remote placement");
  }
  if (policy.minimum_trust != TrustDomain::UNKNOWN) {
    if (domain == TrustDomain::UNKNOWN) {
      return reject(OutcomeCode::REJECT_UNKNOWN_EVIDENCE,
                    "policy sets a trust floor but candidate trust is UNKNOWN");
    }
    if (trust_rank(domain) < trust_rank(policy.minimum_trust)) {
      return reject(OutcomeCode::REJECT_TRUST, "candidate trust domain is below the policy floor");
    }
  }

  // Floors and ceilings.
  if (policy.quality_floor != kUnclassifiedQuality) {
    if (candidate.quality_class == kUnclassifiedQuality) {
      return reject(OutcomeCode::REJECT_UNKNOWN_EVIDENCE,
                    "policy sets a quality floor but candidate quality is unclassified");
    }
    if (candidate.quality_class < policy.quality_floor) {
      return reject(OutcomeCode::REJECT_QUALITY, "candidate quality is below the policy floor");
    }
  }
  if (policy.minimum_capability_evidence != CapabilityState::UNKNOWN) {
    for (const CapabilityEvidence& evidence : candidate.model.capabilities.entries()) {
      if (!is_satisfying(evidence.state)) {
        continue;
      }
      if (evidence_strength(evidence.state) <
          evidence_strength(policy.minimum_capability_evidence)) {
        return reject(OutcomeCode::REJECT_CAPABILITY,
                      "capability evidence is weaker than the policy minimum");
      }
    }
  }
  if (policy.maximum_request_cost.micros > 0) {
    const CostEvidence& cost = candidate.backend.find_binding(candidate.key.model_id) != nullptr
                                   ? candidate.backend.find_binding(candidate.key.model_id)->cost
                                   : candidate.backend.model_bindings.empty()
                                         ? CostEvidence{}
                                         : candidate.backend.model_bindings.front().cost;
    if (!cost.known()) {
      return reject(OutcomeCode::REJECT_UNKNOWN_EVIDENCE,
                    "policy sets a cost ceiling but candidate price is UNKNOWN");
    }
    if (!cost.unit.compatible_with(policy.maximum_request_cost.unit)) {
      return reject(OutcomeCode::REJECT_COST,
                    "candidate price unit is not compatible with the policy cost ceiling unit");
    }
    if (cost.estimated_total_micros > policy.maximum_request_cost.micros) {
      return reject(OutcomeCode::REJECT_COST, "candidate cost exceeds the policy ceiling");
    }
  }
  if (policy.maximum_latency_micros != SloEvidence::kUnspecified) {
    const LatencyEvidence& latency = candidate.backend.latency;
    if (latency.dispatch_micros == LatencyEvidence::kUnknownLatencyMicros) {
      return reject(OutcomeCode::REJECT_UNKNOWN_EVIDENCE,
                    "policy sets a latency ceiling but candidate latency is UNKNOWN");
    }
    if (latency.dispatch_micros > policy.maximum_latency_micros) {
      return reject(OutcomeCode::REJECT_SLO, "candidate latency exceeds the policy ceiling");
    }
  }

  // Preference: policy can express a soft preference that never overrides a
  // hard decision.
  PolicyVerdict verdict;
  std::int64_t preference = 0;
  if (candidate.model.family_id.valid() &&
      contains(policy.preferred_model_families, candidate.model.family_id)) {
    preference += PolicyVerdict::kPreferenceScale / 2;
  }
  if (domain != TrustDomain::UNKNOWN) {
    preference += static_cast<std::int64_t>(trust_rank(domain)) * 100000;
  }
  verdict.preference = std::min(preference, PolicyVerdict::kPreferenceScale);
  return verdict;
}

bool PolicyEvaluator::fallback_permitted(const PolicySnapshot& policy,
                                         const RouteRequirements& requirements,
                                         OutcomeCode trigger) noexcept {
  if (!requirements.allow_fallbacks) {
    return false;
  }
  if (requirements.retry_policy.max_fallbacks == 0) {
    return false;
  }
  switch (policy.fallback_policy) {
    case FallbackPolicy::FORBIDDEN:
      return false;
    case FallbackPolicy::PERMITTED:
      break;
    case FallbackPolicy::EXPLICIT_ONLY:
      if (std::find(policy.fallback_permitted_outcomes.begin(),
                    policy.fallback_permitted_outcomes.end(),
                    trigger) == policy.fallback_permitted_outcomes.end()) {
        return false;
      }
      break;
    default:
      return false;
  }
  if (is_permanent(classify_failure(trigger))) {
    return false;
  }
  return true;
}

PolicyBuilder::PolicyBuilder(PolicyId policy_id, PolicyGeneration generation) {
  policy_.policy_id = policy_id;
  policy_.generation = generation;
}

PolicyBuilder& PolicyBuilder::deny_model(ModelId model_id) {
  policy_.denied_models.push_back(model_id);
  return *this;
}
PolicyBuilder& PolicyBuilder::allow_model(ModelId model_id) {
  policy_.allowed_models.push_back(model_id);
  return *this;
}
PolicyBuilder& PolicyBuilder::deny_provider(ProviderId provider_id) {
  policy_.denied_providers.push_back(provider_id);
  return *this;
}
PolicyBuilder& PolicyBuilder::allow_provider(ProviderId provider_id) {
  policy_.allowed_providers.push_back(provider_id);
  return *this;
}
PolicyBuilder& PolicyBuilder::deny_backend(BackendId backend_id) {
  policy_.denied_backends.push_back(backend_id);
  return *this;
}
PolicyBuilder& PolicyBuilder::allow_backend(BackendId backend_id) {
  policy_.allowed_backends.push_back(backend_id);
  return *this;
}
PolicyBuilder& PolicyBuilder::deny_family(ModelFamilyId family_id) {
  policy_.denied_model_families.push_back(family_id);
  return *this;
}
PolicyBuilder& PolicyBuilder::prefer_family(ModelFamilyId family_id) {
  policy_.preferred_model_families.push_back(family_id);
  return *this;
}
PolicyBuilder& PolicyBuilder::allow_locality(LocalityKey locality) {
  policy_.allowed_localities.push_back(std::move(locality));
  return *this;
}
PolicyBuilder& PolicyBuilder::deny_locality(LocalityKey locality) {
  policy_.denied_localities.push_back(std::move(locality));
  return *this;
}
PolicyBuilder& PolicyBuilder::bind_tenant(TenantId tenant) {
  policy_.bound_tenants.push_back(tenant);
  return *this;
}
PolicyBuilder& PolicyBuilder::minimum_trust(TrustDomain domain) {
  policy_.minimum_trust = domain;
  return *this;
}
PolicyBuilder& PolicyBuilder::minimum_capability_evidence(CapabilityState state) {
  policy_.minimum_capability_evidence = state;
  return *this;
}
PolicyBuilder& PolicyBuilder::quality_floor(QualityClass quality) {
  policy_.quality_floor = quality;
  return *this;
}
PolicyBuilder& PolicyBuilder::maximum_request_cost(Money cost) {
  policy_.maximum_request_cost = cost;
  return *this;
}
PolicyBuilder& PolicyBuilder::maximum_latency(std::uint32_t micros) {
  policy_.maximum_latency_micros = micros;
  return *this;
}
PolicyBuilder& PolicyBuilder::local_only(bool value) {
  policy_.local_only = value;
  return *this;
}
PolicyBuilder& PolicyBuilder::deny_public_remote(bool value) {
  policy_.deny_public_remote = value;
  return *this;
}
PolicyBuilder& PolicyBuilder::fallback_policy(FallbackPolicy value) {
  policy_.fallback_policy = value;
  return *this;
}
PolicyBuilder& PolicyBuilder::permit_fallback_outcome(OutcomeCode code) {
  policy_.fallback_permitted_outcomes.push_back(code);
  return *this;
}
PolicyBuilder& PolicyBuilder::max_fallback_depth(std::uint32_t depth) {
  policy_.max_fallback_depth = depth;
  return *this;
}
PolicyBuilder& PolicyBuilder::max_provider_concentration_ppm(std::uint32_t ppm) {
  policy_.max_provider_concentration_ppm = ppm;
  return *this;
}
PolicyBuilder& PolicyBuilder::allow_unlisted_models(bool value) {
  policy_.allow_unlisted_models = value;
  return *this;
}
PolicyBuilder& PolicyBuilder::allow_unlisted_providers(bool value) {
  policy_.allow_unlisted_providers = value;
  return *this;
}
PolicyBuilder& PolicyBuilder::allow_unlisted_backends(bool value) {
  policy_.allow_unlisted_backends = value;
  return *this;
}
PolicyBuilder& PolicyBuilder::expires_at(UnixMillis unix_millis) {
  policy_.expires_at_unix_millis = unix_millis;
  return *this;
}

PolicySnapshot PolicyBuilder::build() const {
  PolicySnapshot snapshot = policy_;
  snapshot.canonicalize();
  return snapshot;
}

}  // namespace model_router
