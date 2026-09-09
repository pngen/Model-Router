// Model Router - route requests and normalized requirements.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/request.hpp"

#include <algorithm>

#include "model_router/detail/sha256.hpp"

namespace model_router {
namespace {

[[nodiscard]] std::string id_list(const auto& list) {
  std::string out;
  for (const auto value : list) {
    out += std::to_string(value.value());
    out.push_back(',');
  }
  return out;
}

[[nodiscard]] std::string key_list(const std::vector<LocalityKey>& list) {
  std::string out;
  for (const LocalityKey& key : list) {
    out += key.value();
    out.push_back(',');
  }
  return out;
}

[[nodiscard]] std::string capability_list(const std::vector<CapabilityRequirement>& list) {
  std::string out;
  for (const CapabilityRequirement& requirement : list) {
    out += requirement.key.value();
    out.push_back('@');
    out += std::string(to_string(requirement.minimum_state));
    out.push_back(',');
  }
  return out;
}

[[nodiscard]] std::string capability_key_list(const std::vector<CapabilityKey>& list) {
  std::string out;
  for (const CapabilityKey& key : list) {
    out += key.value();
    out.push_back(',');
  }
  return out;
}

template <class IdList>
[[nodiscard]] std::string check_id_list(const IdList& list, const char* name) {
  for (const auto value : list) {
    if (!value.valid()) {
      return std::string(name) + " contains the invalid identity";
    }
  }
  return {};
}

}  // namespace

std::string RouteRequirements::validate(const ResourceLimits& limits) const {
  if (required_capabilities.size() > limits.max_requirement_entries) {
    return "required capability count exceeds the configured limit";
  }
  if (preferred_capabilities.size() > limits.max_requirement_entries) {
    return "preferred capability count exceeds the configured limit";
  }
  if (model_allowlist.size() > limits.max_allowlist_entries ||
      model_denylist.size() > limits.max_allowlist_entries ||
      provider_allowlist.size() > limits.max_allowlist_entries ||
      provider_denylist.size() > limits.max_allowlist_entries ||
      backend_allowlist.size() > limits.max_allowlist_entries ||
      backend_denylist.size() > limits.max_allowlist_entries) {
    return "identity allowlist or denylist exceeds the configured limit";
  }
  if (required_localities.size() > limits.max_allowlist_entries ||
      denied_localities.size() > limits.max_allowlist_entries) {
    return "locality list exceeds the configured limit";
  }

  for (const CapabilityRequirement& requirement : required_capabilities) {
    const std::string key_error = CapabilityKey::validate(requirement.key.value());
    if (!key_error.empty()) {
      return "required capability: " + key_error;
    }
    if (requirement.minimum_state == CapabilityState::UNKNOWN) {
      return "required capability " + requirement.key.value() +
             " declares UNKNOWN as its minimum evidence state";
    }
    if (!is_satisfying(requirement.minimum_state)) {
      return "required capability " + requirement.key.value() +
             " declares a non-satisfying minimum evidence state";
    }
  }
  for (const CapabilityKey& key : preferred_capabilities) {
    const std::string key_error = CapabilityKey::validate(key.value());
    if (!key_error.empty()) {
      return "preferred capability: " + key_error;
    }
  }
  if (minimum_capability_evidence == CapabilityState::UNKNOWN ||
      !is_satisfying(minimum_capability_evidence)) {
    return "minimum capability evidence state is not a satisfying state";
  }

  if (max_output_tokens > 0 && min_context_tokens > 0 &&
      max_output_tokens > min_context_tokens) {
    return "max output tokens exceed the minimum context length";
  }
  if (required_input_modalities.empty() && required_output_modalities.empty() &&
      required_capabilities.empty()) {
    return "request declares no input, output, or capability requirement";
  }

  if (local_only && !remote_allowed) {
    // Consistent: local-only already excludes remote.
  }
  if (local_only && offline_only && !remote_allowed) {
    // Consistent.
  }
  if (!remote_allowed && !local_only && !offline_only) {
    return "request forbids remote placement without permitting local placement";
  }
  if (offline_only && !remote_allowed) {
    // Offline-only implies no remote placement.
  }
  if (local_only && !denied_localities.empty()) {
    return "request is local-only but also denies specific localities";
  }

  if (enforce_cost_ceiling) {
    if (maximum_cost.micros <= 0) {
      return "cost ceiling is enforced but no positive ceiling is set";
    }
    if (!maximum_cost.unit.valid()) {
      return "cost ceiling has no unit identity";
    }
  }
  if (minimum_trust != TrustDomain::UNKNOWN && trust_rank(minimum_trust) == 0) {
    return "minimum trust domain is not a known domain";
  }
  if (retry_policy.max_fallbacks > limits.max_fallback_candidates) {
    return "requested fallback count exceeds the configured limit";
  }
  if (!allow_fallbacks && retry_policy.max_fallbacks != 0) {
    return "fallbacks are disabled but a nonzero fallback count is requested";
  }

  if (const std::string error = check_id_list(model_allowlist, "model allowlist"); !error.empty()) {
    return error;
  }
  if (const std::string error = check_id_list(model_denylist, "model denylist"); !error.empty()) {
    return error;
  }
  if (const std::string error = check_id_list(provider_allowlist, "provider allowlist");
      !error.empty()) {
    return error;
  }
  if (const std::string error = check_id_list(provider_denylist, "provider denylist");
      !error.empty()) {
    return error;
  }
  if (const std::string error = check_id_list(backend_allowlist, "backend allowlist");
      !error.empty()) {
    return error;
  }
  if (const std::string error = check_id_list(backend_denylist, "backend denylist");
      !error.empty()) {
    return error;
  }
  for (const LocalityKey& key : required_localities) {
    const std::string error = LocalityKey::validate(key.value());
    if (!error.empty()) {
      return "required locality: " + error;
    }
  }
  for (const LocalityKey& key : denied_localities) {
    const std::string error = LocalityKey::validate(key.value());
    if (!error.empty()) {
      return "denied locality: " + error;
    }
  }
  return {};
}

void RouteRequirements::canonicalize() {
  std::stable_sort(required_capabilities.begin(), required_capabilities.end(),
                   [](const CapabilityRequirement& lhs, const CapabilityRequirement& rhs) {
                     return lhs.key < rhs.key;
                   });
  required_capabilities.erase(
      std::unique(required_capabilities.begin(), required_capabilities.end(),
                  [](const CapabilityRequirement& lhs, const CapabilityRequirement& rhs) {
                    return lhs.key == rhs.key;
                  }),
      required_capabilities.end());
  std::stable_sort(preferred_capabilities.begin(), preferred_capabilities.end());
  preferred_capabilities.erase(
      std::unique(preferred_capabilities.begin(), preferred_capabilities.end()),
      preferred_capabilities.end());

  const auto sort_ids = [](auto& list) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
  };
  sort_ids(model_allowlist);
  sort_ids(model_denylist);
  sort_ids(family_allowlist);
  sort_ids(family_denylist);
  sort_ids(provider_allowlist);
  sort_ids(provider_denylist);
  sort_ids(backend_allowlist);
  sort_ids(backend_denylist);
  std::sort(required_localities.begin(), required_localities.end());
  required_localities.erase(std::unique(required_localities.begin(), required_localities.end()),
                            required_localities.end());
  std::sort(denied_localities.begin(), denied_localities.end());
  denied_localities.erase(std::unique(denied_localities.begin(), denied_localities.end()),
                          denied_localities.end());
}

std::string RouteRequirements::digest() const {
  std::string canonical;
  canonical += "cap=";
  canonical += capability_list(required_capabilities);
  canonical += ";pref=";
  canonical += capability_key_list(preferred_capabilities);
  canonical += ";minstate=";
  canonical += std::string(to_string(minimum_capability_evidence));
  canonical += ";ctx=";
  canonical += std::to_string(min_context_tokens);
  canonical += ";out=";
  canonical += std::to_string(max_output_tokens);
  canonical += ";inmod=";
  canonical += std::to_string(required_input_modalities.bits());
  canonical += ";outmod=";
  canonical += std::to_string(required_output_modalities.bits());
  canonical += ";flags=";
  canonical += std::to_string(static_cast<int>(require_structured_output));
  canonical += std::to_string(static_cast<int>(require_json_schema));
  canonical += std::to_string(static_cast<int>(require_tool_calling));
  canonical += std::to_string(static_cast<int>(require_streaming));
  canonical += std::to_string(static_cast<int>(require_logprobs));
  canonical += std::to_string(static_cast<int>(require_deterministic_seed));
  canonical += ";quality=";
  canonical += std::to_string(quality_floor);
  canonical += ";ma=";
  canonical += id_list(model_allowlist);
  canonical += ";md=";
  canonical += id_list(model_denylist);
  canonical += ";fa=";
  canonical += id_list(family_allowlist);
  canonical += ";fd=";
  canonical += id_list(family_denylist);
  canonical += ";pa=";
  canonical += id_list(provider_allowlist);
  canonical += ";pd=";
  canonical += id_list(provider_denylist);
  canonical += ";ba=";
  canonical += id_list(backend_allowlist);
  canonical += ";bd=";
  canonical += id_list(backend_denylist);
  canonical += ";place=";
  canonical += std::to_string(static_cast<int>(local_only));
  canonical += std::to_string(static_cast<int>(offline_only));
  canonical += std::to_string(static_cast<int>(remote_allowed));
  canonical += ";loc=";
  canonical += key_list(required_localities);
  canonical += ";denyloc=";
  canonical += key_list(denied_localities);
  canonical += ";trust=";
  canonical += std::string(to_string(minimum_trust));
  canonical += ";cost=";
  canonical += std::to_string(static_cast<int>(require_known_cost));
  canonical += std::to_string(static_cast<int>(enforce_cost_ceiling));
  canonical += maximum_cost.unit.currency;
  canonical.push_back('/');
  canonical += maximum_cost.unit.basis;
  canonical.push_back('/');
  canonical += std::to_string(maximum_cost.micros);
  canonical += ";lat=";
  canonical += std::to_string(max_latency_micros);
  canonical += ";deadline=";
  canonical += std::to_string(deadline_micros);
  canonical += ";slo=";
  canonical += std::to_string(static_cast<int>(require_slo));
  canonical += ";live=";
  canonical += std::to_string(static_cast<int>(require_available));
  canonical += std::to_string(static_cast<int>(require_healthy));
  canonical += std::to_string(static_cast<int>(require_ready));
  canonical += std::to_string(static_cast<int>(require_resident));
  canonical += std::to_string(static_cast<int>(require_capacity));
  canonical += std::to_string(static_cast<int>(require_reservation));
  canonical += std::to_string(static_cast<int>(require_known_latency));
  canonical += std::to_string(static_cast<int>(require_known_compatibility));
  canonical += ";proto=";
  canonical += required_protocol;
  canonical.push_back('/');
  canonical += std::to_string(required_protocol_version);
  canonical += ";aff=";
  canonical += std::to_string(affinity_model.value());
  canonical.push_back('/');
  canonical += std::to_string(affinity_provider.value());
  canonical.push_back('/');
  canonical += std::to_string(affinity_backend.value());
  canonical += ";stick=";
  canonical += std::to_string(static_cast<int>(stickiness.prefer_same_model_family));
  canonical += std::to_string(static_cast<int>(stickiness.prefer_same_provider));
  canonical += std::to_string(static_cast<int>(stickiness.prefer_same_backend));
  canonical += std::to_string(static_cast<int>(stickiness.prefer_context_locality));
  canonical += std::to_string(stickiness.sticky_family.value());
  canonical.push_back('/');
  canonical += std::to_string(stickiness.sticky_provider.value());
  canonical.push_back('/');
  canonical += std::to_string(stickiness.sticky_backend.value());
  canonical += ";fb=";
  canonical += std::to_string(static_cast<int>(allow_fallbacks));
  canonical += std::string(to_string(fallback_policy));
  canonical += std::to_string(retry_policy.allow_retry);
  canonical += std::to_string(retry_policy.allow_reroute);
  canonical += std::to_string(retry_policy.allow_model_switch);
  canonical += std::to_string(retry_policy.allow_provider_switch);
  canonical += std::to_string(retry_policy.max_attempts);
  canonical += std::to_string(retry_policy.max_fallbacks);
  return detail::to_hex(detail::sha256(canonical));
}

std::string RouteRequest::validate(const ResourceLimits& limits) const {
  if (!request_id.valid()) {
    return "route request identity is not valid";
  }
  if (!request_generation.valid()) {
    return "route request generation is not valid";
  }
  if (!policy_id.valid() || !policy_generation.valid()) {
    return "route request has no valid policy identity and generation";
  }
  if (budget_id.valid() != budget_generation.valid()) {
    return "route request budget identity and generation disagree";
  }
  if (slo_id.valid() != slo_generation.valid()) {
    return "route request SLO identity and generation disagree";
  }
  if (!tenant.valid()) {
    return "route request has no valid tenant identity";
  }
  if (requirements.require_slo && (!slo_id.valid() || !slo_generation.valid())) {
    return "SLO is required but no valid SLO identity and generation are set";
  }
  if (expires_at_unix_millis != kNoExpiry && expires_at_unix_millis < created_at_unix_millis) {
    return "route request expires before it was created";
  }
  return requirements.validate(limits);
}

std::string RouteRequest::digest() const {
  std::string canonical;
  canonical += std::to_string(request_id.value());
  canonical.push_back('/');
  canonical += std::to_string(request_generation.value());
  canonical.push_back('|');
  canonical += std::to_string(tenant.value());
  canonical.push_back('/');
  canonical += std::to_string(name_space.value());
  canonical.push_back('|');
  canonical += std::to_string(policy_id.value());
  canonical.push_back('/');
  canonical += std::to_string(policy_generation.value());
  canonical.push_back('|');
  canonical += std::to_string(budget_id.value());
  canonical.push_back('/');
  canonical += std::to_string(budget_generation.value());
  canonical.push_back('|');
  canonical += std::to_string(slo_id.value());
  canonical.push_back('/');
  canonical += std::to_string(slo_generation.value());
  canonical.push_back('|');
  canonical += std::to_string(caller.agent_runtime_id);
  canonical.push_back('/');
  canonical += std::to_string(caller.agent_run_id);
  canonical.push_back('/');
  canonical += std::to_string(caller.action_id);
  canonical.push_back('/');
  canonical += std::to_string(caller.action_generation);
  canonical.push_back('/');
  canonical += std::to_string(caller.attempt_generation);
  canonical.push_back('|');
  canonical += std::to_string(estimated_input_tokens);
  canonical.push_back('/');
  canonical += std::to_string(estimated_output_tokens);
  canonical.push_back('|');
  canonical += requirements.digest();
  return detail::to_hex(detail::sha256(canonical));
}

std::string_view to_string(FailureClass failure) noexcept {
  switch (failure) {
    case FailureClass::TRANSIENT_BACKEND_FAILURE:
      return "TRANSIENT_BACKEND_FAILURE";
    case FailureClass::BACKEND_UNAVAILABLE:
      return "BACKEND_UNAVAILABLE";
    case FailureClass::BACKEND_RESTARTED:
      return "BACKEND_RESTARTED";
    case FailureClass::MODEL_UNAVAILABLE:
      return "MODEL_UNAVAILABLE";
    case FailureClass::STALE_ROUTE:
      return "STALE_ROUTE";
    case FailureClass::POLICY_CHANGED:
      return "POLICY_CHANGED";
    case FailureClass::BUDGET_CHANGED:
      return "BUDGET_CHANGED";
    case FailureClass::PRICE_CHANGED:
      return "PRICE_CHANGED";
    case FailureClass::SLO_CHANGED:
      return "SLO_CHANGED";
    case FailureClass::CAPACITY_CHANGED:
      return "CAPACITY_CHANGED";
    case FailureClass::COMPATIBILITY_CHANGED:
      return "COMPATIBILITY_CHANGED";
    case FailureClass::PERMANENT_REJECTION:
      return "PERMANENT_REJECTION";
    case FailureClass::CALLER_CANCELLED:
      return "CALLER_CANCELLED";
    case FailureClass::UNKNOWN:
      return "UNKNOWN";
    default:
      return "INVALID";
  }
}

FailureClass classify_failure(OutcomeCode code) noexcept {
  switch (code) {
    case OutcomeCode::REJECT_UNAVAILABLE:
    case OutcomeCode::REJECT_UNHEALTHY:
    case OutcomeCode::REJECT_NOT_READY:
      return FailureClass::BACKEND_UNAVAILABLE;
    case OutcomeCode::REJECT_STALE_BACKEND_BOOT:
      return FailureClass::BACKEND_RESTARTED;
    case OutcomeCode::REJECT_RETIRED:
    case OutcomeCode::REJECT_STALE_MODEL:
    case OutcomeCode::REJECT_STALE_ARTIFACT:
      return FailureClass::MODEL_UNAVAILABLE;
    case OutcomeCode::REJECT_STALE_ROUTER_EPOCH:
    case OutcomeCode::REJECT_STALE_COORDINATOR_EPOCH:
    case OutcomeCode::REJECT_STALE_REQUEST:
    case OutcomeCode::REVALIDATION_REQUIRED:
    case OutcomeCode::REJECT_STALE_BACKEND:
    case OutcomeCode::REJECT_STALE_ENDPOINT:
      return FailureClass::STALE_ROUTE;
    case OutcomeCode::REJECT_POLICY:
    case OutcomeCode::REJECT_STALE_POLICY:
    case OutcomeCode::REJECT_TRUST:
    case OutcomeCode::REJECT_STALE_TRUST:
      return FailureClass::POLICY_CHANGED;
    case OutcomeCode::REJECT_BUDGET:
    case OutcomeCode::REJECT_STALE_BUDGET:
      return FailureClass::BUDGET_CHANGED;
    case OutcomeCode::REJECT_COST:
    case OutcomeCode::REJECT_COST_UNKNOWN:
    case OutcomeCode::REJECT_STALE_PRICE:
      return FailureClass::PRICE_CHANGED;
    case OutcomeCode::REJECT_SLO:
    case OutcomeCode::REJECT_STALE_SLO:
      return FailureClass::SLO_CHANGED;
    case OutcomeCode::REJECT_CAPACITY:
    case OutcomeCode::REJECT_STALE_CAPACITY:
      return FailureClass::CAPACITY_CHANGED;
    case OutcomeCode::REJECT_COMPATIBILITY:
    case OutcomeCode::REJECT_STALE_COMPATIBILITY:
      return FailureClass::COMPATIBILITY_CHANGED;
    case OutcomeCode::CANCELLED:
      return FailureClass::CALLER_CANCELLED;
    case OutcomeCode::REJECT_CAPABILITY:
    case OutcomeCode::REJECT_STALE_CAPABILITY:
    case OutcomeCode::REJECT_CONTEXT_LIMIT:
    case OutcomeCode::REJECT_MODALITY:
    case OutcomeCode::REJECT_QUALITY:
    case OutcomeCode::REJECT_INVALID:
    case OutcomeCode::REJECT_CONFLICT:
    case OutcomeCode::REJECT_LOCALITY:
    case OutcomeCode::REJECT_RESIDENCY:
    case OutcomeCode::REJECT_SPOOFED:
      return FailureClass::PERMANENT_REJECTION;
    case OutcomeCode::REJECT_STALE_HEALTH:
    case OutcomeCode::REJECT_STALE_AVAILABILITY:
    case OutcomeCode::REJECT_STALE_READINESS:
    case OutcomeCode::REJECT_STALE_RESIDENCY:
      return FailureClass::TRANSIENT_BACKEND_FAILURE;
    default:
      return FailureClass::UNKNOWN;
  }
}

bool is_permanent(FailureClass failure) noexcept {
  switch (failure) {
    case FailureClass::PERMANENT_REJECTION:
    case FailureClass::CALLER_CANCELLED:
    case FailureClass::MODEL_UNAVAILABLE:
      return true;
    default:
      return false;
  }
}

}  // namespace model_router
