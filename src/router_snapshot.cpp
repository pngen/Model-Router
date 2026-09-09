// Model Router - immutable snapshots, summaries, and retained-history access.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <sstream>

#include "model_router/detail/sha256.hpp"
#include "router_impl.hpp"

namespace model_router {

RouterSnapshot ModelRouter::Impl::snapshot() const {
  const UnixMillis now = this->now();
  RouterSnapshot snapshot;
  std::shared_lock lock(mutex);

  snapshot.router_id = state.router_id;
  snapshot.router_generation = state.router_generation;
  snapshot.router_epoch = state.router_epoch;
  snapshot.coordinator_epoch = state.coordinator_epoch;
  snapshot.router_boot = state.router_boot;
  snapshot.policy_generation = state.policy.generation;
  snapshot.slo_generation = state.slo.generation;
  snapshot.budget_generation = state.budget.generation;
  snapshot.model_count = static_cast<std::uint32_t>(state.models.size());
  snapshot.backend_count = static_cast<std::uint32_t>(state.backends.size());
  snapshot.fenced_boot_count = static_cast<std::uint32_t>(state.fenced_boots.size());
  snapshot.route_decision_count = static_cast<std::uint32_t>(state.routes.size());
  snapshot.tenant_count = static_cast<std::uint32_t>(state.tenants.size());
  snapshot.shutting_down = state.shutting_down;
  snapshot.taken_at_unix_millis = now;
  snapshot.currentness = state.recovered ? Currentness::RECONSTRUCTED : Currentness::CURRENT;

  CapabilityGeneration capability_generation{};
  PriceGeneration price_generation{};
  for (const auto& [backend_id, backend] : state.backends) {
    (void)backend_id;
    BackendSummary summary;
    summary.backend_id = backend.backend_id;
    summary.backend_generation = backend.backend_generation;
    summary.backend_boot = backend.backend_boot;
    summary.provider_id = backend.provider_id;
    summary.endpoint_id = backend.endpoint.endpoint_id;
    summary.endpoint_generation = backend.endpoint.endpoint_generation;
    summary.trust_domain = backend.trust_domain;
    summary.locality = backend.locality;
    summary.health = backend.health.state;
    summary.availability = backend.availability.state;
    summary.readiness = backend.readiness.state;
    summary.residency = backend.residency.state;
    summary.capacity = backend.capacity.state;
    summary.bound_model_count = static_cast<std::uint32_t>(backend.model_bindings.size());
    summary.fenced =
        state.fenced_boots.find(detail::FenceKey{backend.backend_id, backend.backend_boot}) !=
        state.fenced_boots.end();
    summary.evidence_current = backend.health.generation.valid() &&
                               backend.availability.generation.valid() &&
                               backend.readiness.generation.valid() &&
                               !backend.health.expired(now) && !backend.availability.expired(now) &&
                               !backend.readiness.expired(now);
    summary.price_known = !backend.model_bindings.empty();
    for (const ModelBinding& binding : backend.model_bindings) {
      if (!binding.cost.known() || !binding.cost.current(now)) {
        summary.price_known = false;
      }
    }
    summary.provenance = backend.provenance;
    if (capability_generation < backend.capability_generation) {
      capability_generation = backend.capability_generation;
    }
    for (const ModelBinding& binding : backend.model_bindings) {
      if (price_generation < binding.cost.price_generation) {
        price_generation = binding.cost.price_generation;
      }
    }
    snapshot.backends.push_back(std::move(summary));
  }
  snapshot.capability_generation = capability_generation;
  snapshot.price_generation = price_generation;

  for (const auto& [decision_id, record] : state.routes) {
    (void)decision_id;
    RouteSummary summary;
    summary.decision_id = record.decision.decision_id;
    summary.decision_generation = record.decision.decision_generation;
    summary.request_id = record.decision.request_id;
    summary.winner = record.plan.primary;
    summary.status = record.decision.status;
    summary.code = record.decision.code;
    summary.currentness = record.decision.explanation.currentness;
    summary.fallback_count = static_cast<std::uint32_t>(record.decision.fallbacks.size());
    summary.created_at_unix_millis = record.decision.created_at_unix_millis;
    summary.semantic_digest = record.decision.explanation.semantic_digest;
    snapshot.routes.push_back(std::move(summary));
  }

  std::string canonical;
  canonical += std::to_string(snapshot.router_id.value());
  canonical += std::to_string(snapshot.router_epoch.value());
  canonical += std::to_string(snapshot.coordinator_epoch.value());
  canonical += std::to_string(snapshot.policy_generation.value());
  canonical += std::to_string(snapshot.price_generation.value());
  for (const BackendSummary& backend : snapshot.backends) {
    canonical += std::to_string(backend.backend_id.value());
    canonical += std::to_string(backend.backend_boot.value());
    canonical += std::to_string(static_cast<int>(backend.health));
    canonical += std::to_string(static_cast<int>(backend.availability));
    canonical += std::to_string(static_cast<int>(backend.readiness));
  }
  for (const RouteSummary& route : snapshot.routes) {
    canonical += std::to_string(route.decision_id.value());
    canonical += std::to_string(static_cast<int>(route.status));
    canonical += route.semantic_digest;
  }
  snapshot.semantic_digest = detail::to_hex(detail::sha256(canonical));
  return snapshot;
}

RouterSummary ModelRouter::Impl::summary() const {
  std::shared_lock lock(mutex);
  RouterSummary summary;
  summary.router_id = state.router_id;
  summary.router_generation = state.router_generation;
  summary.router_epoch = state.router_epoch;
  summary.coordinator_epoch = state.coordinator_epoch;
  summary.router_boot = state.router_boot;
  summary.model_count = static_cast<std::uint32_t>(state.models.size());
  summary.provider_count = static_cast<std::uint32_t>(state.providers.size());
  summary.backend_count = static_cast<std::uint32_t>(state.backends.size());
  summary.fenced_boot_count = static_cast<std::uint32_t>(state.fenced_boots.size());
  summary.route_decision_count = static_cast<std::uint32_t>(state.routes.size());
  summary.tenant_count = static_cast<std::uint32_t>(state.tenants.size());
  summary.eligible_last_request = state.last_eligible;
  summary.rejected_last_request = state.last_rejected;
  summary.counters = state.counters;
  summary.policy_generation = state.policy.generation;
  summary.budget_generation = state.budget.generation;
  summary.slo_generation = state.slo.generation;
  summary.running = state.running;
  summary.shutting_down = state.shutting_down;
  summary.recovered = state.recovered;
  summary.started_at_unix_millis = state.started_at_unix_millis;

  CapabilityGeneration capability_generation{};
  PriceGeneration price_generation{};
  for (const auto& [backend_id, backend] : state.backends) {
    (void)backend_id;
    if (capability_generation < backend.capability_generation) {
      capability_generation = backend.capability_generation;
    }
    for (const ModelBinding& binding : backend.model_bindings) {
      if (price_generation < binding.cost.price_generation) {
        price_generation = binding.cost.price_generation;
      }
    }
  }
  summary.capability_generation = capability_generation;
  summary.price_generation = price_generation;

  std::string canonical;
  canonical += std::to_string(summary.router_id.value());
  canonical += std::to_string(summary.router_epoch.value());
  canonical += std::to_string(summary.coordinator_epoch.value());
  canonical += std::to_string(summary.model_count);
  canonical += std::to_string(summary.backend_count);
  canonical += std::to_string(summary.route_decision_count);
  canonical += std::to_string(summary.fenced_boot_count);
  summary.semantic_digest = detail::to_hex(detail::sha256(canonical));
  return summary;
}

bool ModelRouter::Impl::find_model(ModelId model_id, ModelDescriptor* out) const {
  std::shared_lock lock(mutex);
  const auto found = state.models.find(model_id);
  if (found == state.models.end()) {
    return false;
  }
  if (out != nullptr) {
    *out = found->second;
  }
  return true;
}

bool ModelRouter::Impl::find_backend(BackendId backend_id, BackendDescriptor* out) const {
  std::shared_lock lock(mutex);
  const auto found = state.backends.find(backend_id);
  if (found == state.backends.end()) {
    return false;
  }
  if (out != nullptr) {
    *out = found->second;
  }
  return true;
}

bool ModelRouter::Impl::find_decision(RouteDecisionId decision_id, RouteDecision* out) const {
  std::shared_lock lock(mutex);
  const auto found = state.routes.find(decision_id);
  if (found == state.routes.end()) {
    return false;
  }
  if (out != nullptr) {
    *out = found->second.decision;
  }
  return true;
}

std::vector<RouteDecision> ModelRouter::Impl::route_history() const {
  std::shared_lock lock(mutex);
  std::vector<RouteDecision> history;
  history.reserve(state.routes.size());
  for (const auto& [decision_id, record] : state.routes) {
    (void)decision_id;
    history.push_back(record.decision);
  }
  return history;
}

std::vector<FencedBootRecord> ModelRouter::Impl::fenced_boots() const {
  std::shared_lock lock(mutex);
  std::vector<FencedBootRecord> records;
  records.reserve(state.fenced_boots.size());
  for (const auto& [key, record] : state.fenced_boots) {
    (void)key;
    records.push_back(record);
  }
  return records;
}

std::string ModelRouter::Impl::explain_route(RouteDecisionId decision_id) const {
  std::shared_lock lock(mutex);
  const auto found = state.routes.find(decision_id);
  if (found == state.routes.end()) {
    return std::string(to_string(OutcomeCode::REJECT_INVALID)) +
           " explain_route {decision_id=" + std::to_string(decision_id.value()) +
           "} route decision is not retained";
  }
  const detail::RouteRecord& record = found->second;
  std::string out;
  out += "decision_id=";
  out += std::to_string(record.decision.decision_id.value());
  out += " status=";
  out += std::string(to_string(record.decision.status));
  out += " code=";
  out += std::string(to_string(record.decision.code));
  out += "\n";
  out += record.decision.explanation.to_text();
  out += "\nplan_digest=";
  out += record.plan.semantic_digest();
  out += " decision_digest=";
  out += record.decision.semantic_digest();
  return out;
}

// --- rendering -------------------------------------------------------------

std::string RouterSnapshot::to_text() const {
  std::ostringstream out;
  out << "router=" << router_id.value() << " generation=" << router_generation.value()
      << " epoch=" << router_epoch.value() << " coordinator_epoch=" << coordinator_epoch.value()
      << " boot=" << router_boot.value() << "\n";
  out << "models=" << model_count << " backends=" << backend_count
      << " fenced_boots=" << fenced_boot_count << " decisions=" << route_decision_count
      << " tenants=" << tenant_count << "\n";
  out << "currentness=" << to_string(currentness) << " shutting_down=" << (shutting_down ? 1 : 0)
      << " digest=" << semantic_digest << "\n";
  for (const BackendSummary& backend : backends) {
    out << "backend " << backend.backend_id.value() << "@" << backend.backend_generation.value()
        << ".boot=" << backend.backend_boot.value() << " provider=" << backend.provider_id.value()
        << " endpoint=" << backend.endpoint_id.value() << "@"
        << backend.endpoint_generation.value() << " trust=" << to_string(backend.trust_domain)
        << " locality=" << (backend.locality.empty() ? "none" : backend.locality.value())
        << " health=" << to_string(backend.health) << " availability=" << to_string(backend.availability)
        << " readiness=" << to_string(backend.readiness) << " residency=" << to_string(backend.residency)
        << " capacity=" << to_string(backend.capacity) << " models=" << backend.bound_model_count
        << " fenced=" << (backend.fenced ? 1 : 0)
        << " evidence_current=" << (backend.evidence_current ? 1 : 0)
        << " price_known=" << (backend.price_known ? 1 : 0)
        << " provenance=" << to_string(backend.provenance) << "\n";
  }
  for (const RouteSummary& route : routes) {
    out << "decision " << route.decision_id.value() << " status=" << to_string(route.status)
        << " code=" << to_string(route.code) << " currentness=" << to_string(route.currentness)
        << " fallbacks=" << route.fallback_count << " winner=" << route.winner.to_string()
        << " digest=" << route.semantic_digest << "\n";
  }
  return out.str();
}

std::string RouterSnapshot::to_json() const {
  std::ostringstream out;
  out << "{\"router_id\":" << router_id.value() << ",\"router_epoch\":" << router_epoch.value()
      << ",\"coordinator_epoch\":" << coordinator_epoch.value()
      << ",\"router_boot\":" << router_boot.value() << ",\"currentness\":\""
      << to_string(currentness) << "\",\"shutting_down\":" << (shutting_down ? "true" : "false")
      << ",\"semantic_digest\":\"" << semantic_digest << "\""
      << ",\"counts\":{\"models\":" << model_count << ",\"backends\":" << backend_count
      << ",\"fenced_boots\":" << fenced_boot_count << ",\"decisions\":" << route_decision_count
      << ",\"tenants\":" << tenant_count << "},\"backends\":[";
  bool first = true;
  for (const BackendSummary& backend : backends) {
    if (!first) {
      out << ",";
    }
    first = false;
    out << "{\"backend_id\":" << backend.backend_id.value()
        << ",\"backend_generation\":" << backend.backend_generation.value()
        << ",\"backend_boot\":" << backend.backend_boot.value()
        << ",\"provider_id\":" << backend.provider_id.value()
        << ",\"endpoint_id\":" << backend.endpoint_id.value()
        << ",\"endpoint_generation\":" << backend.endpoint_generation.value()
        << ",\"trust_domain\":\"" << to_string(backend.trust_domain) << "\",\"health\":\""
        << to_string(backend.health) << "\",\"availability\":\"" << to_string(backend.availability)
        << "\",\"readiness\":\"" << to_string(backend.readiness) << "\",\"residency\":\""
        << to_string(backend.residency) << "\",\"capacity\":\"" << to_string(backend.capacity)
        << "\",\"fenced\":" << (backend.fenced ? "true" : "false")
        << ",\"evidence_current\":" << (backend.evidence_current ? "true" : "false")
        << ",\"price_known\":" << (backend.price_known ? "true" : "false")
        << ",\"provenance\":\"" << to_string(backend.provenance) << "\"}";
  }
  out << "],\"routes\":[";
  first = true;
  for (const RouteSummary& route : routes) {
    if (!first) {
      out << ",";
    }
    first = false;
    out << "{\"decision_id\":" << route.decision_id.value()
        << ",\"status\":\"" << to_string(route.status) << "\",\"code\":\"" << to_string(route.code)
        << "\",\"currentness\":\"" << to_string(route.currentness)
        << "\",\"fallbacks\":" << route.fallback_count << ",\"digest\":\""
        << route.semantic_digest << "\"}";
  }
  out << "]}";
  return out.str();
}

std::string RouterSummary::to_text() const {
  std::ostringstream out;
  out << "router=" << router_id.value() << " generation=" << router_generation.value()
      << " epoch=" << router_epoch.value() << " coordinator_epoch=" << coordinator_epoch.value()
      << " boot=" << router_boot.value() << "\n";
  out << "running=" << (running ? 1 : 0) << " shutting_down=" << (shutting_down ? 1 : 0)
      << " recovered=" << (recovered ? 1 : 0) << "\n";
  out << "models=" << model_count << " providers=" << provider_count
      << " backends=" << backend_count << " fenced_boots=" << fenced_boot_count
      << " decisions=" << route_decision_count << " tenants=" << tenant_count << "\n";
  out << "policy_generation=" << policy_generation.value()
      << " price_generation=" << price_generation.value()
      << " budget_generation=" << budget_generation.value()
      << " slo_generation=" << slo_generation.value()
      << " capability_generation=" << capability_generation.value() << "\n";
  out << "route_requests=" << counters.route_requests
      << " route_decisions=" << counters.route_decisions
      << " dispatches=" << counters.dispatches
      << " rejections=" << counters.route_rejections << " reroutes=" << counters.reroutes
      << " fallbacks=" << counters.fallbacks_taken << "\n";
  out << "digest=" << semantic_digest << "\n";
  return out.str();
}

std::string RouterSummary::to_json() const {
  std::ostringstream out;
  out << "{\"router_id\":" << router_id.value() << ",\"router_epoch\":" << router_epoch.value()
      << ",\"coordinator_epoch\":" << coordinator_epoch.value()
      << ",\"router_boot\":" << router_boot.value() << ",\"running\":" << (running ? "true" : "false")
      << ",\"shutting_down\":" << (shutting_down ? "true" : "false")
      << ",\"recovered\":" << (recovered ? "true" : "false") << ",\"counts\":{\"models\":"
      << model_count << ",\"providers\":" << provider_count << ",\"backends\":" << backend_count
      << ",\"fenced_boots\":" << fenced_boot_count << ",\"decisions\":" << route_decision_count
      << ",\"tenants\":" << tenant_count << "},\"generations\":{\"policy\":"
      << policy_generation.value() << ",\"price\":" << price_generation.value()
      << ",\"budget\":" << budget_generation.value() << ",\"slo\":" << slo_generation.value()
      << ",\"capability\":" << capability_generation.value() << "},\"counters\":{\"requests\":"
      << counters.route_requests << ",\"decisions\":" << counters.route_decisions
      << ",\"dispatches\":" << counters.dispatches << ",\"rejections\":"
      << counters.route_rejections << ",\"reroutes\":" << counters.reroutes
      << ",\"fallbacks\":" << counters.fallbacks_taken << "},\"semantic_digest\":\""
      << semantic_digest << "\"}";
  return out.str();
}

}  // namespace model_router
