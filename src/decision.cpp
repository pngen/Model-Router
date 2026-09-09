// Model Router - route decisions, plans, and dispatch records.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/decision.hpp"

#include <algorithm>

#include "model_router/detail/sha256.hpp"

namespace model_router {
namespace {

void append_key(std::string* out, const CandidateKey& key) {
  *out += key.to_string();
  out->push_back('|');
}

}  // namespace

std::string_view to_string(RouteStatus status) noexcept {
  switch (status) {
    case RouteStatus::CURRENT:
      return "CURRENT";
    case RouteStatus::SUPERSEDED:
      return "SUPERSEDED";
    case RouteStatus::STALE:
      return "STALE";
    case RouteStatus::DISPATCHED:
      return "DISPATCHED";
    case RouteStatus::COMPLETED:
      return "COMPLETED";
    case RouteStatus::FAILED:
      return "FAILED";
    case RouteStatus::CANCELLED:
      return "CANCELLED";
    default:
      return "INVALID";
  }
}

std::string RouteDecision::semantic_digest() const {
  std::string canonical;
  canonical += explanation.requirement_digest;
  canonical.push_back('|');
  append_key(&canonical, explanation.winner);
  for (const CandidateKey& key : fallbacks) {
    append_key(&canonical, key);
  }
  canonical += std::string(to_string(code));
  canonical.push_back('|');
  canonical += explanation.semantic_digest;
  canonical.push_back('|');
  const auto append = [&canonical](std::uint64_t value) {
    canonical += std::to_string(value);
    canonical.push_back(',');
  };
  append(authority.router_epoch.value());
  append(authority.coordinator_epoch.value());
  append(authority.model_generation.value());
  append(authority.artifact_generation.value());
  append(authority.backend_generation.value());
  append(authority.backend_boot.value());
  append(authority.endpoint_generation.value());
  append(authority.capability_generation.value());
  append(authority.policy_generation.value());
  append(authority.budget_generation.value());
  append(authority.price_generation.value());
  append(authority.slo_generation.value());
  append(authority.compatibility_generation.value());
  append(authority.trust_generation.value());
  append(authority.health_generation.value());
  append(authority.availability_generation.value());
  append(authority.readiness_generation.value());
  append(authority.residency_generation.value());
  append(authority.capacity_generation.value());
  append(authority.reservation_generation.value());
  return detail::to_hex(detail::sha256(canonical));
}

std::string RoutePlan::semantic_digest() const {
  std::string canonical;
  canonical += std::to_string(plan_id.value());
  canonical.push_back('/');
  canonical += std::to_string(plan_generation.value());
  canonical.push_back('|');
  canonical += std::to_string(decision_id.value());
  canonical.push_back('/');
  canonical += std::to_string(decision_generation.value());
  canonical.push_back('|');
  append_key(&canonical, primary);
  for (const CandidateKey& key : fallbacks) {
    append_key(&canonical, key);
  }
  canonical += authority_digest(authority);
  canonical.push_back('|');
  canonical += std::to_string(static_cast<int>(constraints.require_revalidation));
  canonical += std::to_string(static_cast<int>(constraints.require_reservation));
  canonical += std::to_string(constraints.max_attempts);
  canonical += std::to_string(constraints.max_fallbacks);
  canonical += std::to_string(constraints.dispatch_deadline_unix_millis);
  canonical.push_back('|');
  canonical += std::string(to_string(fallback_policy));
  canonical += std::to_string(retry_policy.max_attempts);
  canonical += std::to_string(retry_policy.max_fallbacks);
  canonical += std::to_string(static_cast<int>(retry_policy.allow_reroute));
  canonical += std::to_string(static_cast<int>(retry_policy.allow_model_switch));
  canonical += std::to_string(static_cast<int>(retry_policy.allow_provider_switch));
  return detail::to_hex(detail::sha256(canonical));
}

RouteOutcome RouteOutcome::make(OutcomeCode code_value, std::string subject, std::string message) {
  ExplanationBuilder builder(code_value, std::move(subject));
  builder.set_message(std::move(message));
  RouteOutcome outcome;
  outcome.code = code_value;
  outcome.explanation = builder.build();
  outcome.has_decision = false;
  return outcome;
}

}  // namespace model_router
