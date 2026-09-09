// Model Router - callable invariant checking.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <sstream>

#include "router_impl.hpp"

namespace model_router {
namespace {

void violate(InvariantReport* report, std::string name, std::string detail) {
  report->violations.push_back(InvariantViolation{std::move(name), std::move(detail)});
}

}  // namespace

InvariantReport ModelRouter::Impl::check_invariants() const {
  InvariantReport report;
  const UnixMillis now = this->now();
  std::shared_lock lock(mutex);

  // 1. One current router epoch and coordinator epoch.
  ++report.checks_run;
  if (state.running && (!state.router_epoch.valid() || !state.coordinator_epoch.valid())) {
    violate(&report, "one_current_router_epoch",
            "router is running without a published router or coordinator epoch");
  }

  // 2. Backend incarnations unique by (BackendId, BackendBootId).
  ++report.checks_run;
  {
    std::set<detail::FenceKey> seen;
    for (const auto& [backend_id, backend] : state.backends) {
      (void)backend_id;
      const detail::FenceKey key{backend.backend_id, backend.backend_boot};
      if (!seen.insert(key).second) {
        violate(&report, "backend_incarnation_unique",
                "duplicate backend incarnation " + std::to_string(backend.backend_id.value()) +
                    ".boot=" + std::to_string(backend.backend_boot.value()));
      }
    }
  }

  // 3. Fenced backend boots never become authoritative again.
  ++report.checks_run;
  for (const auto& [backend_id, backend] : state.backends) {
    (void)backend_id;
    if (state.fenced_boots.find(detail::FenceKey{backend.backend_id, backend.backend_boot}) !=
        state.fenced_boots.end()) {
      violate(&report, "fenced_boot_never_authoritative",
              "fenced incarnation is present in canonical state: backend=" +
                  std::to_string(backend.backend_id.value()) +
                  " boot=" + std::to_string(backend.backend_boot.value()));
    }
  }

  // 4. Model and backend generations are monotonic against their watermarks.
  ++report.checks_run;
  for (const auto& [model_id, model] : state.models) {
    const auto watermark = state.model_generation_watermark.find(model_id);
    if (watermark != state.model_generation_watermark.end() &&
        model.model_generation < watermark->second) {
      violate(&report, "model_generation_monotonic",
              "model " + std::to_string(model_id.value()) + " regressed");
    }
  }
  for (const auto& [backend_id, backend] : state.backends) {
    const auto watermark = state.backend_generation_watermark.find(backend_id);
    if (watermark != state.backend_generation_watermark.end() &&
        backend.backend_generation < watermark->second) {
      violate(&report, "backend_generation_monotonic",
              "backend " + std::to_string(backend_id.value()) + " regressed");
    }
  }

  // 5. Indexes equal canonical scans.
  ++report.checks_run;
  {
    std::map<ModelId, std::set<BackendId>> by_model;
    std::map<ProviderId, std::set<BackendId>> by_provider;
    std::map<std::uint8_t, std::set<BackendId>> by_trust;
    for (const auto& [backend_id, backend] : state.backends) {
      by_provider[backend.provider_id].insert(backend_id);
      by_trust[trust_rank(backend.trust_domain)].insert(backend_id);
      for (const ModelBinding& binding : backend.model_bindings) {
        by_model[binding.model_id].insert(backend_id);
      }
    }
    if (by_model != state.index_backends_by_model) {
      violate(&report, "index_backends_by_model", "index disagrees with the canonical scan");
    }
    if (by_provider != state.index_backends_by_provider) {
      violate(&report, "index_backends_by_provider", "index disagrees with the canonical scan");
    }
    if (by_trust != state.index_backends_by_trust) {
      violate(&report, "index_backends_by_trust", "index disagrees with the canonical scan");
    }
  }

  // 6. Bounded history.
  ++report.checks_run;
  if (state.routes.size() > state.limits.max_route_history ||
      state.route_order.size() > state.limits.max_route_history) {
    violate(&report, "bounded_route_history", "retained route history exceeds the configured bound");
  }
  if (state.route_order.size() != state.routes.size()) {
    violate(&report, "route_order_consistent",
            "route eviction order and retained route map disagree");
  }
  if (state.fenced_boots.size() > state.limits.max_backend_incarnations) {
    violate(&report, "bounded_fenced_boots", "fenced incarnation history exceeds the bound");
  }

  // 7. Shutdown cannot publish a new executable route.
  ++report.checks_run;
  if (state.shutting_down) {
    for (const auto& [decision_id, record] : state.routes) {
      (void)decision_id;
      if (record.decision.status == RouteStatus::CURRENT) {
        violate(&report, "shutdown_no_executable_route",
                "a current route exists while the router is shutting down");
        break;
      }
    }
  }

  // 8-14. Per-route invariants.
  for (const auto& [decision_id, record] : state.routes) {
    const RouteDecision& decision = record.decision;
    ++report.checks_run;

    if (!decision.decision_generation.valid()) {
      violate(&report, "route_generation_monotonic",
              "decision " + std::to_string(decision_id.value()) + " has no valid generation");
    }
    if (!(record.tenant == decision.authority.tenant)) {
      violate(&report, "tenant_isolation",
              "decision " + std::to_string(decision_id.value()) +
                  " record tenant disagrees with its bound authority tenant");
    }
    if (!(decision.explanation.authority == decision.authority)) {
      violate(&report, "bound_generations_match_snapshot",
              "decision " + std::to_string(decision_id.value()) +
                  " explanation authority differs from its bound authority");
    }
    if (decision.explanation.request_id.valid() &&
        !(decision.explanation.request_id == decision.request_id)) {
      violate(&report, "bound_request_identity",
              "decision " + std::to_string(decision_id.value()) +
                  " explanation request identity differs from the decision");
    }
    if (!decision.explanation.ranking.empty() &&
        !(decision.explanation.ranking.front().key == record.plan.primary)) {
      violate(&report, "winner_is_top_ranked",
              "decision " + std::to_string(decision_id.value()) +
                  " plan primary is not the top-ranked candidate");
    }
    if (!(decision.fallbacks == record.plan.fallbacks)) {
      violate(&report, "fallback_order_stable",
              "decision " + std::to_string(decision_id.value()) +
                  " fallback list differs from its plan");
    }

    // No duplicate candidate identity in the ranked output.
    {
      std::set<CandidateKey> seen;
      for (const RankedCandidate& ranked : decision.explanation.ranking) {
        if (!seen.insert(ranked.key).second) {
          violate(&report, "no_duplicate_ranked_candidate",
                  "decision " + std::to_string(decision_id.value()) +
                      " contains a duplicate candidate identity");
        }
      }
    }
    // Ranking is canonically ordered.
    for (std::size_t index = 1; index < decision.explanation.ranking.size(); ++index) {
      if (candidate_ranks_before(decision.explanation.ranking[index],
                                 decision.explanation.ranking[index - 1])) {
        violate(&report, "deterministic_ranking_order",
                "decision " + std::to_string(decision_id.value()) +
                    " ranked list is not in canonical order");
        break;
      }
    }
    // Rejections are canonically ordered.
    for (std::size_t index = 1; index < decision.explanation.rejections.size(); ++index) {
      if (decision.explanation.rejections[index].canonical_less(
              decision.explanation.rejections[index - 1])) {
        violate(&report, "deterministic_rejection_order",
                "decision " + std::to_string(decision_id.value()) +
                    " rejection list is not in canonical order");
        break;
      }
    }
    // Revalidation must be required before dispatch.
    if (!record.plan.constraints.require_revalidation) {
      violate(&report, "cache_cannot_bypass_revalidation",
              "decision " + std::to_string(decision_id.value()) +
                  " plan does not require pre-dispatch revalidation");
    }

    if (decision.status != RouteStatus::CURRENT) {
      continue;
    }

    // A current route must be dispatchable right now.
    const auto model = state.models.find(decision.authority.model_id);
    if (model == state.models.end()) {
      violate(&report, "current_route_winner_exists",
              "current decision " + std::to_string(decision_id.value()) +
                  " references an unregistered model");
      continue;
    }
    if (model->second.lifecycle == ModelLifecycle::RETIRED) {
      violate(&report, "retired_model_cannot_dispatch",
              "current decision " + std::to_string(decision_id.value()) +
                  " references a retired model");
    }
    const auto backend = state.backends.find(decision.authority.backend_id);
    if (backend == state.backends.end()) {
      violate(&report, "current_route_winner_exists",
              "current decision " + std::to_string(decision_id.value()) +
                  " references an unregistered backend");
      continue;
    }
    const BackendDescriptor& descriptor = backend->second;
    if (state.fenced_boots.find(detail::FenceKey{descriptor.backend_id, descriptor.backend_boot}) !=
        state.fenced_boots.end()) {
      violate(&report, "stale_backend_boot_cannot_dispatch",
              "current decision " + std::to_string(decision_id.value()) +
                  " references a fenced incarnation");
    }
    if (!(descriptor.backend_boot == decision.authority.backend_boot)) {
      violate(&report, "stale_backend_boot_cannot_dispatch",
              "current decision " + std::to_string(decision_id.value()) +
                  " bound an older backend incarnation");
    }
    if (descriptor.retired() || descriptor.draining()) {
      violate(&report, "unavailable_backend_cannot_dispatch",
              "current decision " + std::to_string(decision_id.value()) +
                  " references a retired or draining backend");
    }
    if (!(state.policy.generation == decision.authority.policy_generation)) {
      violate(&report, "stale_policy_cannot_dispatch",
              "current decision " + std::to_string(decision_id.value()) +
                  " bound an older policy generation");
    }
    if (!(state.router_epoch == decision.authority.router_epoch)) {
      violate(&report, "stale_router_epoch_cannot_dispatch",
              "current decision " + std::to_string(decision_id.value()) +
                  " bound an older router epoch");
    }
    if (!(state.coordinator_epoch == decision.authority.coordinator_epoch)) {
      violate(&report, "stale_coordinator_epoch_cannot_dispatch",
              "current decision " + std::to_string(decision_id.value()) +
                  " bound an older coordinator epoch");
    }
    if (decision.authority.expired(now)) {
      violate(&report, "expired_route_cannot_dispatch",
              "current decision " + std::to_string(decision_id.value()) + " has expired");
    }
    // Policy must still allow the bound model and backend. The candidate is
    // constructed from the state this scan already holds, so no nested shared
    // lock is taken.
    {
      RouteCandidate candidate;
      bool constructed = false;
      const auto bound_model = state.models.find(decision.authority.model_id);
      const auto bound_backend = state.backends.find(decision.authority.backend_id);
      if (bound_model != state.models.end() && bound_backend != state.backends.end()) {
        const ModelBinding* binding = bound_backend->second.find_binding(
            decision.authority.model_id);
        if (binding != nullptr) {
          candidate.key.model_id = bound_model->second.model_id;
          candidate.key.model_generation = bound_model->second.model_generation;
          candidate.key.artifact_generation = bound_model->second.artifact_generation;
          candidate.key.backend_id = bound_backend->second.backend_id;
          candidate.key.backend_generation = bound_backend->second.backend_generation;
          candidate.key.backend_boot = bound_backend->second.backend_boot;
          candidate.key.endpoint_id = bound_backend->second.endpoint.endpoint_id;
          candidate.key.endpoint_generation = bound_backend->second.endpoint.endpoint_generation;
          candidate.model = bound_model->second;
          candidate.backend = bound_backend->second;
          candidate.endpoint = bound_backend->second.endpoint;
          candidate.context_limit_tokens = binding->context_limit_tokens != 0
                                               ? binding->context_limit_tokens
                                               : bound_model->second.context_limit_tokens;
          candidate.quality_class = binding->quality_class != kUnclassifiedQuality
                                        ? binding->quality_class
                                        : bound_model->second.quality_class;
          constructed = true;
        }
      }
      if (constructed) {
        RouteRequest request;
        request.request_id = decision.request_id;
        request.request_generation = decision.request_generation;
        request.tenant = decision.authority.tenant;
        request.name_space = decision.authority.name_space;
        request.policy_id = decision.authority.policy_id;
        request.policy_generation = decision.authority.policy_generation;
        const PolicyVerdict verdict =
            PolicyEvaluator::evaluate_candidate(state.policy, request, candidate, now);
        if (!verdict.allowed()) {
          violate(&report, "policy_denied_route_cannot_dispatch",
                  "current decision " + std::to_string(decision_id.value()) +
                      " is denied by the current policy");
        }
      }
    }
  }

  std::sort(report.violations.begin(), report.violations.end(),
            [](const InvariantViolation& lhs, const InvariantViolation& rhs) {
              if (lhs.name != rhs.name) {
                return lhs.name < rhs.name;
              }
              return lhs.detail < rhs.detail;
            });
  return report;
}

std::string InvariantReport::to_text() const {
  std::ostringstream out;
  out << (violations.empty() ? "OK" : "VIOLATED") << " checks=" << checks_run
      << " violations=" << violations.size() << "\n";
  for (const InvariantViolation& violation : violations) {
    out << "  " << violation.name << ": " << violation.detail << "\n";
  }
  return out.str();
}

std::string InvariantReport::to_json() const {
  std::ostringstream out;
  out << "{\"ok\":" << (violations.empty() ? "true" : "false") << ",\"checks\":" << checks_run
      << ",\"violations\":[";
  bool first = true;
  for (const InvariantViolation& violation : violations) {
    if (!first) {
      out << ",";
    }
    first = false;
    out << "{\"name\":\"" << violation.name << "\",\"detail\":\"" << violation.detail
        << "\"}";
  }
  out << "]}";
  return out.str();
}

}  // namespace model_router
