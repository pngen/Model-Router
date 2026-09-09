// Model Router - durable state export, import, and conservative recovery.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>

#include "router_impl.hpp"

namespace model_router {

RouterPersistentState ModelRouter::Impl::export_persistent_state() const {
  std::shared_lock lock(mutex);
  RouterPersistentState persisted;
  persisted.router_id = state.router_id;
  persisted.router_generation = state.router_generation;
  persisted.last_router_epoch = state.router_epoch;
  persisted.last_coordinator_epoch = state.coordinator_epoch;
  persisted.last_boot = state.router_boot;
  persisted.limits = state.limits;
  persisted.weights = state.weights;
  persisted.last_policy_generation = state.policy.generation;
  persisted.last_budget_generation = state.budget.generation;
  persisted.last_slo_generation = state.slo.generation;
  persisted.saved_at_unix_millis = now();
  persisted.last_started_at_unix_millis = state.started_at_unix_millis;
  persisted.counters = state.counters;
  persisted.counters.highest_identity_issued = IdAllocator::highest_issued();

  for (const auto& [model_id, model] : state.models) {
    (void)model_id;
    PersistedModelRecord record;
    record.model = model;
    persisted.models.push_back(std::move(record));
    if (persisted.last_model_generation < model.model_generation) {
      persisted.last_model_generation = model.model_generation;
    }
    if (persisted.last_capability_generation < model.capability_generation) {
      persisted.last_capability_generation = model.capability_generation;
    }
  }

  for (const auto& [backend_id, backend] : state.backends) {
    (void)backend_id;
    PersistedBackendRecord record;
    record.backend_id = backend.backend_id;
    record.backend_generation = backend.backend_generation;
    record.last_boot = backend.backend_boot;
    record.last_registration_generation = backend.backend_registration_generation;
    record.provider_id = backend.provider_id;
    record.provider_generation = backend.provider_generation;
    record.endpoint_id = backend.endpoint.endpoint_id;
    record.last_endpoint_generation = backend.endpoint.endpoint_generation;
    record.trust_profile_id = backend.trust_profile_id;
    record.trust_generation = backend.trust_generation;
    record.trust_domain = backend.trust_domain;
    record.locality = backend.locality;
    record.network_distance = backend.network_distance;
    record.capability_profile_id = backend.capability_profile_id;
    record.capability_generation = backend.capability_generation;
    record.capabilities = backend.capabilities;
    record.model_bindings = backend.model_bindings;
    record.offline_capable = backend.offline_capable;
    record.provenance = backend.provenance;
    record.registered_at_unix_millis = backend.registered_at_unix_millis;
    persisted.backends.push_back(std::move(record));
    if (persisted.last_backend_generation < backend.backend_generation) {
      persisted.last_backend_generation = backend.backend_generation;
    }
    if (persisted.last_capability_generation < backend.capability_generation) {
      persisted.last_capability_generation = backend.capability_generation;
    }
    for (const ModelBinding& binding : backend.model_bindings) {
      if (persisted.last_price_generation < binding.cost.price_generation) {
        persisted.last_price_generation = binding.cost.price_generation;
      }
    }
  }

  for (const auto& [key, record] : state.fenced_boots) {
    (void)key;
    persisted.fenced_boots.push_back(record);
  }

  for (const auto& [decision_id, record] : state.routes) {
    (void)decision_id;
    PersistedRouteRecord route;
    route.decision_id = record.decision.decision_id;
    route.decision_generation = record.decision.decision_generation;
    route.request_id = record.decision.request_id;
    route.request_generation = record.decision.request_generation;
    route.winner = record.plan.primary;
    route.fallbacks = record.decision.fallbacks;
    route.authority = record.decision.authority;
    route.status = record.decision.status;
    route.code = record.decision.code;
    route.cost = record.decision.explanation.cost;
    route.policy_generation = record.decision.authority.policy_generation;
    route.price_generation = record.decision.authority.price_generation;
    route.budget_generation = record.decision.authority.budget_generation;
    route.slo_generation = record.decision.authority.slo_generation;
    route.semantic_digest = record.decision.explanation.semantic_digest;
    route.requirement_digest = record.decision.explanation.requirement_digest;
    route.created_at_unix_millis = record.decision.created_at_unix_millis;
    persisted.routes.push_back(std::move(route));
  }

  persisted.canonicalize();
  return persisted;
}

MutationResult ModelRouter::Impl::import_persistent_state(
    const RouterPersistentState& persisted) {
  RouterPersistentState canonical = persisted;
  canonical.canonicalize();
  if (const std::string error = canonical.validate(state.limits); !error.empty()) {
    return reject(OutcomeCode::REJECT_INVALID, "load", error);
  }

  std::unique_lock lock(mutex);
  if (state.running) {
    return reject(OutcomeCode::REJECT_CONFLICT, "load",
                  "durable state cannot be loaded while the router is running");
  }

  // Router identity is restored; authority epochs are always advanced so that
  // traffic and evidence bound to the previous process can never be current.
  state.router_id = canonical.router_id;
  state.router_generation = canonical.router_generation;
  state.router_epoch = canonical.last_router_epoch.next();
  state.coordinator_epoch = canonical.last_coordinator_epoch.next();
  state.router_boot = canonical.last_boot;
  state.limits = canonical.limits;
  state.weights = canonical.weights;
  state.counters = canonical.counters;
  state.started_at_unix_millis = canonical.last_started_at_unix_millis;
  state.recovered = true;

  state.models.clear();
  state.backends.clear();
  state.fenced_boots.clear();
  state.routes.clear();
  state.route_order.clear();
  state.dispatches.clear();
  state.dispatch_index.clear();
  state.compatibility.clear();
  state.trust.clear();
  state.costs.clear();
  state.reservations.clear();
  state.last_winner_by_tenant.clear();
  state.tenants.clear();
  state.model_generation_watermark.clear();
  state.backend_generation_watermark.clear();

  for (const PersistedModelRecord& record : canonical.models) {
    state.models[record.model.model_id] = record.model;
    state.model_generation_watermark[record.model.model_id] = record.model.model_generation;
  }

  for (const PersistedBackendRecord& record : canonical.backends) {
    BackendDescriptor backend;
    backend.backend_id = record.backend_id;
    backend.backend_generation = record.backend_generation;
    backend.backend_boot = record.last_boot;
    backend.backend_registration_generation = record.last_registration_generation;
    backend.provider_id = record.provider_id;
    backend.provider_generation = record.provider_generation;
    backend.endpoint.endpoint_id = record.endpoint_id;
    backend.endpoint.endpoint_generation = record.last_endpoint_generation;
    backend.endpoint.backend_id = record.backend_id;
    backend.endpoint.backend_boot = record.last_boot;
    backend.endpoint.reference = "recovered";
    backend.trust_profile_id = record.trust_profile_id;
    backend.trust_generation = record.trust_generation;
    backend.trust_domain = record.trust_domain;
    backend.locality = record.locality;
    backend.network_distance = record.network_distance;
    backend.capability_profile_id = record.capability_profile_id;
    backend.capability_generation = record.capability_generation;
    backend.capabilities = record.capabilities;
    backend.model_bindings = record.model_bindings;
    backend.offline_capable = record.offline_capable;
    backend.provenance = record.provenance;
    backend.registered_at_unix_millis = record.registered_at_unix_millis;
    // Dynamic evidence is never resurrected: state stays UNKNOWN and every
    // generation stays unset, so any previously bound generation is stale.
    backend.health = HealthEvidence{};
    backend.availability = AvailabilityEvidence{};
    backend.readiness = ReadinessEvidence{};
    backend.residency = ResidencyEvidence{};
    backend.capacity = CapacityEvidence{};
    backend.capacity_detail = CapacityDetail{};
    backend.latency = LatencyEvidence{};
    backend.health.backend_id = record.backend_id;
    backend.health.backend_boot = record.last_boot;
    backend.availability.backend_id = record.backend_id;
    backend.availability.backend_boot = record.last_boot;
    backend.readiness.backend_id = record.backend_id;
    backend.readiness.backend_boot = record.last_boot;
    backend.residency.backend_id = record.backend_id;
    backend.residency.backend_boot = record.last_boot;
    backend.capacity.backend_id = record.backend_id;
    backend.capacity.backend_boot = record.last_boot;
    backend.latency.backend_id = record.backend_id;
    backend.latency.backend_boot = record.last_boot;
    backend.canonicalize();
    state.backends[record.backend_id] = std::move(backend);
    state.backend_generation_watermark[record.backend_id] = record.backend_generation;
  }

  for (const FencedBootRecord& record : canonical.fenced_boots) {
    state.fenced_boots[detail::FenceKey{record.backend_id, record.backend_boot}] = record;
  }

  for (const PersistedRouteRecord& record : canonical.routes) {
    detail::RouteRecord retained;
    retained.decision.decision_id = record.decision_id;
    retained.decision.decision_generation = record.decision_generation;
    retained.decision.request_id = record.request_id;
    retained.decision.request_generation = record.request_generation;
    retained.decision.status = RouteStatus::STALE;
    retained.decision.code = record.code;
    retained.decision.authority = record.authority;
    retained.decision.fallbacks = record.fallbacks;
    retained.decision.created_at_unix_millis = record.created_at_unix_millis;
    retained.decision.explanation.request_id = record.request_id;
    retained.decision.explanation.request_generation = record.request_generation;
    retained.decision.explanation.winner = record.winner;
    retained.decision.explanation.authority = record.authority;
    retained.decision.explanation.cost = record.cost;
    retained.decision.explanation.requirement_digest = record.requirement_digest;
    retained.decision.explanation.semantic_digest = record.semantic_digest;
    retained.decision.explanation.fallback_order = record.fallbacks;
    retained.decision.explanation.currentness = Currentness::RECONSTRUCTED;
    retained.decision.explanation.revalidation_required = true;
    retained.decision.explanation.detail_retained = false;
    retained.plan.plan_id = allocate_id<RoutePlanTag>();
    retained.plan.plan_generation = Generation<RoutePlanTag>(1);
    retained.plan.decision_id = record.decision_id;
    retained.plan.decision_generation = record.decision_generation;
    retained.plan.primary = record.winner;
    retained.plan.fallbacks = record.fallbacks;
    retained.plan.authority = record.authority;
    retained.plan.constraints.require_revalidation = true;
    retained.plan.created_at_unix_millis = record.created_at_unix_millis;
    retained.tenant = record.authority.tenant;
    retained.name_space = record.authority.name_space;
    state.routes[record.decision_id] = std::move(retained);
    state.route_order.push_back(record.decision_id);
  }

  state.policy = PolicySnapshot{};
  state.budget = BudgetSnapshot{};
  state.slo = SloEvidence{};
  state.rebuild_indexes();

  // Identity allocation must not reissue identities that were already durable.
  IdAllocator::observe(canonical.counters.highest_identity_issued);

  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "load");
  builder.add_u64("router_id", state.router_id.value());
  builder.add_u64("router_epoch", state.router_epoch.value());
  builder.add_u64("coordinator_epoch", state.coordinator_epoch.value());
  builder.add_u64("models", state.models.size());
  builder.add_u64("backends", state.backends.size());
  builder.add_u64("fenced_boots", state.fenced_boots.size());
  builder.add_u64("route_history", state.routes.size());
  builder.set_message(
      "durable state restored; dynamic evidence invalidated and old dispatch authority "
      "invalidated");
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

PersistenceResult ModelRouter::Impl::save(const std::string& path) {
  const RouterPersistentState persisted = export_persistent_state();
  ResourceLimits limits;
  {
    std::shared_lock lock(mutex);
    limits = state.limits;
  }
  return RouterStateStore::save(persisted, path, limits);
}

PersistenceResult ModelRouter::Impl::load(const std::string& path) {
  ResourceLimits limits;
  {
    std::shared_lock lock(mutex);
    limits = state.limits;
  }
  const PersistenceLoad loaded = RouterStateStore::load(path, limits);
  PersistenceResult result;
  result.code = loaded.code;
  result.detail = loaded.detail;
  if (!loaded.ok()) {
    return result;
  }
  const MutationResult imported = import_persistent_state(loaded.state);
  if (!imported.accepted()) {
    result.code = imported.code;
    result.detail = imported.explanation.message();
    return result;
  }
  result.detail = "records=" + std::to_string(loaded.record_count);
  return result;
}

}  // namespace model_router
