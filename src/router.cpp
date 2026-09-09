// Model Router - router lifecycle, canonical mutation, and public forwarding.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "router_impl.hpp"

#include <algorithm>
#include <utility>

namespace model_router::detail {

void CanonicalState::rebuild_indexes() {
  index_backends_by_model.clear();
  index_backends_by_provider.clear();
  index_backends_by_trust.clear();
  for (const auto& [backend_id, backend] : backends) {
    index_backends_by_provider[backend.provider_id].insert(backend_id);
    index_backends_by_trust[trust_rank(backend.trust_domain)].insert(backend_id);
    for (const ModelBinding& binding : backend.model_bindings) {
      index_backends_by_model[binding.model_id].insert(backend_id);
    }
  }
}

void CanonicalState::index_add_backend(const BackendDescriptor& backend) {
  index_backends_by_provider[backend.provider_id].insert(backend.backend_id);
  index_backends_by_trust[trust_rank(backend.trust_domain)].insert(backend.backend_id);
  for (const ModelBinding& binding : backend.model_bindings) {
    index_backends_by_model[binding.model_id].insert(backend.backend_id);
  }
}

void CanonicalState::index_remove_backend(const BackendDescriptor& backend) {
  const auto provider = index_backends_by_provider.find(backend.provider_id);
  if (provider != index_backends_by_provider.end()) {
    provider->second.erase(backend.backend_id);
    if (provider->second.empty()) {
      index_backends_by_provider.erase(provider);
    }
  }
  const auto trust_index = index_backends_by_trust.find(trust_rank(backend.trust_domain));
  if (trust_index != index_backends_by_trust.end()) {
    trust_index->second.erase(backend.backend_id);
    if (trust_index->second.empty()) {
      index_backends_by_trust.erase(trust_index);
    }
  }
  for (const ModelBinding& binding : backend.model_bindings) {
    const auto model = index_backends_by_model.find(binding.model_id);
    if (model != index_backends_by_model.end()) {
      model->second.erase(backend.backend_id);
      if (model->second.empty()) {
        index_backends_by_model.erase(model);
      }
    }
  }
}

void CanonicalState::stale_decisions_for_backend(BackendId backend_id,
                                                 BackendBootId backend_boot) {
  for (auto& [decision_id, record] : routes) {
    (void)decision_id;
    if (record.decision.status != RouteStatus::CURRENT &&
        record.decision.status != RouteStatus::DISPATCHED) {
      continue;
    }
    if (record.decision.authority.backend_id == backend_id &&
        (!backend_boot.valid() || record.decision.authority.backend_boot == backend_boot)) {
      record.decision.status = RouteStatus::STALE;
      record.decision.explanation.currentness = Currentness::STALE;
      record.decision.explanation.revalidation_required = true;
    }
  }
}

RouteLookup RouteLookup::from(const RouteRecord& record) {
  RouteLookup lookup;
  lookup.decision_id = record.decision.decision_id;
  lookup.decision_generation = record.decision.decision_generation;
  lookup.request_id = record.decision.request_id;
  lookup.request_generation = record.decision.request_generation;
  lookup.status = record.decision.status;
  lookup.code = record.decision.code;
  lookup.authority = record.decision.authority;
  lookup.tenant = record.tenant;
  lookup.name_space = record.name_space;
  return lookup;
}

bool build_current_authority(const CanonicalState& state, const RouteAuthority& bound,
                             RouteAuthority* out) {
  if (out == nullptr) {
    return false;
  }
  *out = RouteAuthority{};
  out->router_id = bound.router_id;
  out->router_epoch = state.router_epoch;
  out->coordinator_epoch = state.coordinator_epoch;
  out->request_id = bound.request_id;
  out->request_generation = bound.request_generation;
  out->decision_id = bound.decision_id;
  out->decision_generation = bound.decision_generation;
  out->model_id = bound.model_id;
  out->backend_id = bound.backend_id;
  out->tenant = bound.tenant;
  out->name_space = bound.name_space;
  out->policy_id = state.policy.policy_id;
  out->policy_generation = state.policy.generation;
  out->slo_id = state.slo.slo_id;
  out->slo_generation = state.slo.generation;
  if (bound.budget_generation.valid()) {
    out->budget_id = state.budget.budget_id;
    out->budget_generation = state.budget.generation;
  }
  const auto model = state.models.find(bound.model_id);
  if (model == state.models.end()) {
    return false;
  }
  out->model_generation = model->second.model_generation;
  out->artifact_generation = model->second.artifact_generation;
  const auto backend = state.backends.find(bound.backend_id);
  if (backend == state.backends.end()) {
    return false;
  }
  const BackendDescriptor& descriptor = backend->second;
  out->provider_id = descriptor.provider_id;
  out->provider_generation = descriptor.provider_generation;
  out->backend_generation = descriptor.backend_generation;
  out->backend_boot = descriptor.backend_boot;
  out->backend_registration_generation = descriptor.backend_registration_generation;
  out->endpoint_id = descriptor.endpoint.endpoint_id;
  out->endpoint_generation = descriptor.endpoint.endpoint_generation;
  out->capability_profile_id = descriptor.capability_profile_id;
  out->capability_generation = descriptor.capability_generation;
  out->compatibility_profile_id = descriptor.compatibility_profile_id;
  out->compatibility_generation = descriptor.compatibility_generation;
  out->trust_profile_id = descriptor.trust_profile_id;
  out->trust_generation = descriptor.trust_generation;
  out->health_generation = descriptor.health.generation;
  out->availability_generation = descriptor.availability.generation;
  out->readiness_generation = descriptor.readiness.generation;
  out->residency_generation = descriptor.residency.generation;
  out->capacity_generation = descriptor.capacity.generation;
  const auto binding = descriptor.find_binding(bound.model_id);
  if (binding != nullptr) {
    out->cost_evidence_id = binding->cost.evidence_id;
    out->price_generation = binding->cost.price_generation;
  }
  const auto reservation = state.reservations.find(bound.backend_id);
  if (reservation != state.reservations.end()) {
    out->reservation_id = reservation->second.reservation_id;
    out->reservation_generation = reservation->second.generation;
  }
  return true;
}

OutcomeCode dispatchability_reason(const CanonicalState& state, const RouteAuthority& bound,
                                       UnixMillis now) {
  if (bound.expired(now)) {
    return OutcomeCode::REJECT_STALE_REQUEST;
  }
  if (state.fenced_boots.find(FenceKey{bound.backend_id, bound.backend_boot}) !=
      state.fenced_boots.end()) {
    return OutcomeCode::REJECT_STALE_BACKEND_BOOT;
  }
  RouteAuthority observed;
  if (!build_current_authority(state, bound, &observed)) {
    return OutcomeCode::REJECT_STALE_BACKEND;
  }
  const AuthorityComparison comparison =
      compare_authority(bound, observed, bound.required_mask());
  if (!comparison.current()) {
    return comparison.code;
  }
  // A generation can stay identical while a lifecycle or availability fact
  // changes, so those facts are checked explicitly.
  const auto model = state.models.find(bound.model_id);
  const auto backend = state.backends.find(bound.backend_id);
  if (model == state.models.end()) {
    return OutcomeCode::REJECT_STALE_MODEL;
  }
  if (backend == state.backends.end()) {
    return OutcomeCode::REJECT_STALE_BACKEND;
  }
  if (model->second.lifecycle == ModelLifecycle::RETIRED) {
    return OutcomeCode::REJECT_RETIRED;
  }
  if (model->second.lifecycle != ModelLifecycle::CURRENT) {
    return OutcomeCode::REJECT_STALE_MODEL;
  }
  if (backend->second.retired()) {
    return OutcomeCode::REJECT_RETIRED;
  }
  if (backend->second.draining()) {
    return OutcomeCode::REJECT_DRAINING;
  }
  if (backend->second.backend_boot != bound.backend_boot ||
      backend->second.backend_generation != bound.backend_generation) {
    return OutcomeCode::REJECT_STALE_BACKEND_BOOT;
  }
  if (backend->second.endpoint.endpoint_generation != bound.endpoint_generation) {
    return OutcomeCode::REJECT_STALE_ENDPOINT;
  }
  return OutcomeCode::ACCEPTED;
}

bool is_authority_dispatchable(const CanonicalState& state, const RouteAuthority& bound,
                              UnixMillis now) {
  return dispatchability_reason(state, bound, now) == OutcomeCode::ACCEPTED;
}

void CanonicalState::stale_decision_if_not_dispatchable(RouteRecord& record, UnixMillis now) {
  if (record.decision.status != RouteStatus::CURRENT &&
      record.decision.status != RouteStatus::DISPATCHED) {
    return;
  }
  if (is_authority_dispatchable(*this, record.decision.authority, now)) {
    return;
  }
  record.decision.status = RouteStatus::STALE;
  record.decision.explanation.currentness = Currentness::STALE;
  record.decision.explanation.revalidation_required = true;
}

void CanonicalState::stale_decisions_for_authority_change(UnixMillis now) {
  for (auto& [decision_id, record] : routes) {
    (void)decision_id;
    stale_decision_if_not_dispatchable(record, now);
  }
}

void CanonicalState::note_tenant(TenantId tenant) {
  if (tenant.valid()) {
    tenants.insert(tenant);
  }
}

}  // namespace model_router::detail

namespace model_router {
namespace {

using detail::CanonicalState;
using detail::FenceKey;

[[nodiscard]] bool generation_regressed(std::uint64_t incoming, std::uint64_t current) noexcept {
  return current != 0 && incoming != 0 && incoming < current;
}

/// Copies the boot-bound identity fields of dynamic evidence so a caller can
/// see exactly which incarnation a rejected observation belonged to.
template <class Evidence>
[[nodiscard]] bool evidence_matches_backend(const Evidence& evidence,
                                            const BackendDescriptor& backend) noexcept {
  return evidence.backend_id.valid() && (evidence.backend_id == backend.backend_id) &&
         (evidence.backend_boot == backend.backend_boot) &&
         (!evidence.backend_generation.valid() ||
          (evidence.backend_generation == backend.backend_generation));
}

}  // namespace

std::string ModelRouterOptions::validate() const {
  if (const std::string error = limits.validate(); !error.empty()) {
    return "resource limits are incoherent: " + error;
  }
  if (const std::string error = weights.validate(); !error.empty()) {
    return "ranking weights are incoherent: " + error;
  }
  if (const std::string error = scales.validate(); !error.empty()) {
    return "ranking scales are incoherent: " + error;
  }
  if (save_on_shutdown && persistence_path.empty()) {
    return "save_on_shutdown is set without a persistence path";
  }
  if (load_on_start && persistence_path.empty()) {
    return "load_on_start is set without a persistence path";
  }
  return {};
}

ModelRouter::Impl::Impl(ModelRouterOptions options_in) : options(std::move(options_in)) {
  clock = options.clock != nullptr ? options.clock : std::make_shared<SystemClock>();
  options_error = options.validate();
  state.limits = options.limits;
  state.weights = options.weights;
  state.scales = options.scales;
  state.router_id = options.router_id.valid() ? options.router_id : allocate_id<RouterTag>();
  state.router_generation = options.router_generation.valid()
                                ? options.router_generation
                                : Generation<RouterTag>(1);
}

ModelRouter::Impl::~Impl() = default;

UnixMillis ModelRouter::Impl::now() const { return clock->now_unix_millis(); }

DiscoveryContext ModelRouter::Impl::make_context() const {
  DiscoveryContext context;
  context.router_id = state.router_id;
  context.router_epoch = state.router_epoch;
  context.coordinator_epoch = state.coordinator_epoch;
  context.now_unix_millis = now();
  context.limits = &state.limits;
  return context;
}

MutationResult ModelRouter::Impl::reject(OutcomeCode code, const char* subject,
                                         const std::string& message) const {
  return MutationResult::make(code, subject, message);
}

bool ModelRouter::Impl::is_fenced(BackendId backend_id, BackendBootId boot) const {
  return state.fenced_boots.find(FenceKey{backend_id, boot}) != state.fenced_boots.end();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

MutationResult ModelRouter::Impl::start() {
  if (!options_error.empty()) {
    return reject(OutcomeCode::REJECT_INVALID, "start",
                  "router options are invalid: " + options_error);
  }
  std::string load_detail;
  OutcomeCode load_code = OutcomeCode::ACCEPTED;
  RouterPersistentState persisted;
  bool have_persisted = false;

  if (options.load_on_start && !options.persistence_path.empty()) {
    const PersistenceLoad loaded = RouterStateStore::load(options.persistence_path, state.limits);
    if (loaded.ok()) {
      persisted = loaded.state;
      have_persisted = true;
    } else {
      load_code = loaded.code;
      load_detail = loaded.detail;
    }
  }

  {
    std::unique_lock lock(mutex);
    if (state.running) {
      return MutationResult::make(OutcomeCode::NO_CHANGE, "start", "router is already running");
    }
    // A previous shutdown leaves the router stopped, not permanently dead: the
    // caller may start it again and the epochs advance.
    state.shutting_down = false;
  }

  if (!load_detail.empty()) {
    return reject(load_code, "start", "durable state could not be loaded: " + load_detail);
  }

  if (have_persisted) {
    const MutationResult imported = import_persistent_state(persisted);
    if (!imported.accepted()) {
      return imported;
    }
  }

  std::unique_lock lock(mutex);
  if (state.running) {
    return MutationResult::make(OutcomeCode::NO_CHANGE, "start", "router is already running");
  }
  state.router_boot = state.router_boot.next();
  state.router_epoch = state.router_epoch.next();
  state.coordinator_epoch = state.coordinator_epoch.next();
  state.running = true;
  state.shutting_down = false;
  state.started_at_unix_millis = now();

  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "start");
  builder.add_u64("router_id", state.router_id.value());
  builder.add_u64("router_generation", state.router_generation.value());
  builder.add_u64("router_epoch", state.router_epoch.value());
  builder.add_u64("coordinator_epoch", state.coordinator_epoch.value());
  builder.add_u64("router_boot", state.router_boot.value());
  builder.add_bool("recovered", state.recovered);
  builder.set_message("router identity published; dynamic evidence requires fresh publication");
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult ModelRouter::Impl::shutdown() {
  RouterPersistentState persisted;
  std::string path;
  bool should_save = false;

  {
    std::unique_lock lock(mutex);
    if (!state.running && !state.shutting_down) {
      return MutationResult::make(OutcomeCode::NO_CHANGE, "shutdown", "router is not running");
    }
    state.shutting_down = true;
    // No new executable route authority may be created from this point on.
    for (auto& [id, record] : state.routes) {
      (void)id;
      if (record.decision.status == RouteStatus::CURRENT) {
        record.decision.status = RouteStatus::STALE;
      }
    }
    state.running = false;
    should_save = options.save_on_shutdown && !options.persistence_path.empty();
    path = options.persistence_path;
    if (should_save) {
      persisted = export_persistent_state();
    }
  }

  if (should_save) {
    // Persistence performs filesystem I/O and is deliberately outside the lock.
    const PersistenceResult saved = RouterStateStore::save(persisted, path, state.limits);
    if (!saved.ok()) {
      return reject(OutcomeCode::INTERNAL_ERROR, "shutdown",
                    "durable state could not be saved: " + saved.detail);
    }
  }

  std::unique_lock lock(mutex);
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "shutdown");
  builder.add_u64("router_epoch", state.router_epoch.value());
  builder.add_bool("persisted", should_save);
  builder.set_message("router stopped accepting route requests");
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

// ---------------------------------------------------------------------------
// Catalog mutation
// ---------------------------------------------------------------------------

MutationResult ModelRouter::Impl::register_model(ModelDescriptor model) {
  if (const std::string error = model.validate(); !error.empty()) {
    return reject(OutcomeCode::REJECT_INVALID, "register_model", error);
  }
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return reject(OutcomeCode::SHUTTING_DOWN, "register_model", "router is shutting down");
  }
  if (state.models.size() >= state.limits.max_models &&
      state.models.find(model.model_id) == state.models.end()) {
    return reject(OutcomeCode::REJECT_LIMIT, "register_model", "model limit reached");
  }
  const auto watermark = state.model_generation_watermark.find(model.model_id);
  if (watermark != state.model_generation_watermark.end() &&
      generation_regressed(model.model_generation.value(), watermark->second.value())) {
    return reject(OutcomeCode::REJECT_STALE_MODEL, "register_model",
                  "model generation regressed below the observed watermark");
  }
  const auto existing = state.models.find(model.model_id);
  if (existing != state.models.end() &&
      existing->second.model_generation == model.model_generation &&
      existing->second.artifact_generation == model.artifact_generation &&
      existing->second.lifecycle == model.lifecycle) {
    return MutationResult::make(OutcomeCode::NO_CHANGE, "register_model",
                                "model generation is already registered");
  }
  const ModelId model_id = model.model_id;
  const ModelGeneration model_generation = model.model_generation;
  const ArtifactGeneration artifact_generation = model.artifact_generation;
  state.model_generation_watermark[model_id] = model_generation;
  state.models[model_id] = std::move(model);

  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "register_model");
  builder.add_u64("model_id", model_id.value());
  builder.add_u64("model_generation", model_generation.value());
  builder.add_u64("artifact_generation", artifact_generation.value());
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult ModelRouter::Impl::register_provider(ProviderDescriptor provider) {
  if (!provider.provider_id.valid()) {
    return reject(OutcomeCode::REJECT_INVALID, "register_provider", "provider identity is invalid");
  }
  if (!provider.generation.valid()) {
    return reject(OutcomeCode::REJECT_INVALID, "register_provider",
                  "provider generation is invalid");
  }
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return reject(OutcomeCode::SHUTTING_DOWN, "register_provider", "router is shutting down");
  }
  if (state.providers.size() >= state.limits.max_providers &&
      state.providers.find(provider.provider_id) == state.providers.end()) {
    return reject(OutcomeCode::REJECT_LIMIT, "register_provider", "provider limit reached");
  }
  const auto existing = state.providers.find(provider.provider_id);
  if (existing != state.providers.end() && existing->second.generation == provider.generation &&
      existing->second.display_name == provider.display_name) {
    return MutationResult::make(OutcomeCode::NO_CHANGE, "register_provider",
                                "provider generation is already registered");
  }
  if (existing != state.providers.end() &&
      generation_regressed(provider.generation.value(), existing->second.generation.value())) {
    return reject(OutcomeCode::REJECT_STALE_BACKEND, "register_provider",
                  "provider generation regressed");
  }
  state.providers[provider.provider_id] = std::move(provider);
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "register_provider");
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult ModelRouter::Impl::register_backend(BackendDescriptor backend) {
  backend.canonicalize();
  if (const std::string error = backend.validate(); !error.empty()) {
    return reject(OutcomeCode::REJECT_INVALID, "register_backend", error);
  }
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return reject(OutcomeCode::SHUTTING_DOWN, "register_backend", "router is shutting down");
  }
  if (is_fenced(backend.backend_id, backend.backend_boot)) {
    return reject(OutcomeCode::REJECT_STALE_BACKEND_BOOT, "register_backend",
                  "backend incarnation is fenced and can never become authoritative again");
  }
  if (state.backends.size() >= state.limits.max_backends &&
      state.backends.find(backend.backend_id) == state.backends.end()) {
    return reject(OutcomeCode::REJECT_LIMIT, "register_backend", "backend limit reached");
  }
  const auto watermark = state.backend_generation_watermark.find(backend.backend_id);
  if (watermark != state.backend_generation_watermark.end() &&
      generation_regressed(backend.backend_generation.value(), watermark->second.value())) {
    return reject(OutcomeCode::REJECT_STALE_BACKEND, "register_backend",
                  "backend generation regressed below the observed watermark");
  }
  const auto existing = state.backends.find(backend.backend_id);
  if (existing != state.backends.end()) {
    if (backend.backend_registration_generation <=
        existing->second.backend_registration_generation) {
      return reject(OutcomeCode::REJECT_STALE_BACKEND, "register_backend",
                    "registration generation did not advance");
    }
  }
  const BackendId backend_id = backend.backend_id;
  const BackendGeneration backend_generation = backend.backend_generation;
  const BackendBootId backend_boot = backend.backend_boot;
  const BackendRegistrationGeneration registration = backend.backend_registration_generation;
  const Provenance provenance = backend.provenance;
  state.backend_generation_watermark[backend_id] = backend_generation;
  const auto replaced = state.backends.find(backend_id);
  if (replaced != state.backends.end()) {
    state.index_remove_backend(replaced->second);
  }
  state.backends[backend_id] = std::move(backend);
  state.index_add_backend(state.backends[backend_id]);
  state.stale_decisions_for_authority_change(now());

  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "register_backend");
  builder.add_u64("backend_id", backend_id.value());
  builder.add_u64("backend_generation", backend_generation.value());
  builder.add_u64("backend_boot", backend_boot.value());
  builder.add_u64("registration_generation", registration.value());
  builder.add("provenance", std::string(to_string(provenance)));
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult ModelRouter::Impl::unregister_backend(BackendId backend_id,
                                                     BackendGeneration generation) {
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return reject(OutcomeCode::SHUTTING_DOWN, "unregister_backend", "router is shutting down");
  }
  const auto existing = state.backends.find(backend_id);
  if (existing == state.backends.end()) {
    return MutationResult::make(OutcomeCode::NO_CHANGE, "unregister_backend",
                                "backend is not registered");
  }
  if (generation.valid() && existing->second.backend_generation != generation) {
    return reject(OutcomeCode::REJECT_STALE_BACKEND, "unregister_backend",
                  "backend generation does not match the current registration");
  }
  const BackendBootId retired_boot = existing->second.backend_boot;
  FencedBootRecord fence_record;
  fence_record.backend_id = backend_id;
  fence_record.backend_generation = existing->second.backend_generation;
  fence_record.backend_boot = retired_boot;
  fence_record.reason = OutcomeCode::FENCED;
  fence_record.fenced_at_unix_millis = now();
  state.fenced_boots[FenceKey{backend_id, retired_boot}] = fence_record;
  state.index_remove_backend(existing->second);
  state.backends.erase(existing);
  state.stale_decisions_for_backend(backend_id, retired_boot);
  state.stale_decisions_for_authority_change(now());
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "unregister_backend");
  builder.add_u64("backend_id", backend_id.value());
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult ModelRouter::Impl::publish_capabilities(BackendCapabilityPublication publication) {
  if (!publication.backend_id.valid() || !publication.backend_boot.valid()) {
    return reject(OutcomeCode::REJECT_INVALID, "publish_capabilities",
                  "publication has no valid backend identity or incarnation");
  }
  if (!publication.generation.valid()) {
    return reject(OutcomeCode::REJECT_INVALID, "publish_capabilities",
                  "publication has no valid capability generation");
  }
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return reject(OutcomeCode::SHUTTING_DOWN, "publish_capabilities",
                  "router is shutting down");
  }
  if (is_fenced(publication.backend_id, publication.backend_boot)) {
    return reject(OutcomeCode::REJECT_STALE_BACKEND_BOOT, "publish_capabilities",
                  "publication belongs to a fenced backend incarnation");
  }
  const auto existing = state.backends.find(publication.backend_id);
  if (existing == state.backends.end()) {
    return reject(OutcomeCode::REJECT_STALE_BACKEND, "publish_capabilities",
                  "backend is not registered");
  }
  BackendDescriptor& backend = existing->second;
  if (!(backend.backend_boot == publication.backend_boot)) {
    return reject(OutcomeCode::REJECT_STALE_BACKEND_BOOT, "publish_capabilities",
                  "publication belongs to a different backend incarnation");
  }
  if (publication.backend_generation.valid() &&
      !(publication.backend_generation == backend.backend_generation)) {
    return reject(OutcomeCode::REJECT_STALE_BACKEND, "publish_capabilities",
                  "publication backend generation is not current");
  }
  if (generation_regressed(publication.generation.value(),
                           backend.capability_generation.value())) {
    return reject(OutcomeCode::REJECT_STALE_CAPABILITY, "publish_capabilities",
                  "capability generation regressed");
  }
  if (publication.claims.size() > state.limits.max_capability_entries_per_backend) {
    return reject(OutcomeCode::REJECT_LIMIT, "publish_capabilities",
                  "capability publication exceeds the configured limit");
  }

  CapabilityProfile profile(publication.profile_id, publication.generation);
  for (const CapabilityEvidence& claim : publication.claims) {
    if (const std::string error = CapabilityKey::validate(claim.key.value()); !error.empty()) {
      return reject(OutcomeCode::REJECT_INVALID, "publish_capabilities", error);
    }
    CapabilityEvidence bound = claim;
    bound.profile_id = publication.profile_id;
    bound.generation = publication.generation;
    bound.backend_boot = publication.backend_boot;
    bound.backend_generation = backend.backend_generation;
    if (!profile.set(std::move(bound), state.limits.max_capability_entries_per_backend)) {
      return reject(OutcomeCode::REJECT_LIMIT, "publish_capabilities",
                    "capability entry limit reached");
    }
  }

  if (backend.capability_generation.valid() &&
      publication.generation == backend.capability_generation) {
    // An already-issued generation may only be re-published unchanged. A
    // changed claim set requires a new generation, exactly like every other
    // generation-bound evidence kind.
    if (profile.entries() == backend.capabilities.entries()) {
      return MutationResult::make(OutcomeCode::NO_CHANGE, "publish_capabilities",
                                  "capability generation is already current");
    }
    return reject(OutcomeCode::REJECT_CONFLICT, "publish_capabilities",
                  "capability claims changed without advancing the capability generation");
  }

  backend.capabilities = std::move(profile);
  backend.capability_profile_id = publication.profile_id;
  backend.capability_generation = publication.generation;
  state.stale_decisions_for_authority_change(now());

  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "publish_capabilities");
  builder.add_u64("backend_id", publication.backend_id.value());
  builder.add_u64("backend_boot", publication.backend_boot.value());
  builder.add_u64("capability_generation", publication.generation.value());
  builder.add_u64("claims", publication.claims.size());
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult ModelRouter::Impl::retire_model(ModelId model_id, ModelGeneration generation) {
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return reject(OutcomeCode::SHUTTING_DOWN, "retire_model", "router is shutting down");
  }
  const auto existing = state.models.find(model_id);
  if (existing == state.models.end()) {
    return reject(OutcomeCode::REJECT_STALE_MODEL, "retire_model", "model is not registered");
  }
  if (generation.valid() && !(existing->second.model_generation == generation)) {
    return reject(OutcomeCode::REJECT_STALE_MODEL, "retire_model",
                  "model generation is not the registered generation");
  }
  if (existing->second.lifecycle == ModelLifecycle::RETIRED) {
    return MutationResult::make(OutcomeCode::NO_CHANGE, "retire_model", "model is already retired");
  }
  existing->second.lifecycle = ModelLifecycle::RETIRED;
  state.stale_decisions_for_authority_change(now());
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "retire_model");
  builder.add_u64("model_id", model_id.value());
  builder.add_u64("model_generation", existing->second.model_generation.value());
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult ModelRouter::Impl::fence_backend_boot(BackendId backend_id,
                                                     BackendGeneration generation,
                                                     BackendBootId backend_boot,
                                                     OutcomeCode reason) {
  if (!backend_id.valid() || !backend_boot.valid()) {
    return reject(OutcomeCode::REJECT_INVALID, "fence_backend_boot",
                  "backend identity or incarnation is invalid");
  }
  std::unique_lock lock(mutex);
  const FenceKey key{backend_id, backend_boot};
  if (state.fenced_boots.find(key) != state.fenced_boots.end()) {
    return MutationResult::make(OutcomeCode::NO_CHANGE, "fence_backend_boot",
                                "backend incarnation is already fenced");
  }
  if (state.fenced_boots.size() >= state.limits.max_backend_incarnations) {
    return reject(OutcomeCode::REJECT_LIMIT, "fence_backend_boot",
                  "fenced incarnation limit reached");
  }
  FencedBootRecord fence_record;
  fence_record.backend_id = backend_id;
  fence_record.backend_generation = generation;
  fence_record.backend_boot = backend_boot;
  fence_record.reason = reason;
  fence_record.fenced_at_unix_millis = now();
  const auto existing = state.backends.find(backend_id);
  const bool retires_current =
      existing != state.backends.end() && (existing->second.backend_boot == backend_boot) &&
      (!generation.valid() || existing->second.backend_generation == generation);
  if (retires_current) {
    fence_record.backend_generation = existing->second.backend_generation;
    state.index_remove_backend(existing->second);
    state.backends.erase(existing);
  }
  state.fenced_boots[key] = fence_record;
  state.stale_decisions_for_backend(backend_id, backend_boot);
  state.stale_decisions_for_authority_change(now());
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "fence_backend_boot");
  builder.add_u64("backend_id", backend_id.value());
  builder.add_u64("backend_boot", backend_boot.value());
  builder.add("reason", std::string(to_string(reason)));
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

// ---------------------------------------------------------------------------
// Dynamic evidence
// ---------------------------------------------------------------------------

namespace {

template <class Evidence>
[[nodiscard]] MutationResult apply_evidence(CanonicalState& state, std::shared_mutex& mutex,
                                            Evidence evidence, const char* subject, UnixMillis now,
                                            OutcomeCode stale_code, OutcomeCode unknown_code) {
  if (!evidence.backend_id.valid() || !evidence.backend_boot.valid()) {
    return MutationResult::make(OutcomeCode::REJECT_INVALID, subject,
                                "evidence has no valid backend identity or incarnation");
  }
  if (!evidence.generation.valid()) {
    return MutationResult::make(OutcomeCode::REJECT_INVALID, subject,
                                "evidence has no valid generation");
  }
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return MutationResult::make(OutcomeCode::SHUTTING_DOWN, subject, "router is shutting down");
  }
  if (state.fenced_boots.find(FenceKey{evidence.backend_id, evidence.backend_boot}) !=
      state.fenced_boots.end()) {
    return MutationResult::make(OutcomeCode::REJECT_STALE_BACKEND_BOOT, subject,
                                "evidence belongs to a fenced backend incarnation");
  }
  const auto existing = state.backends.find(evidence.backend_id);
  if (existing == state.backends.end()) {
    return MutationResult::make(OutcomeCode::REJECT_STALE_BACKEND, subject,
                                "backend is not registered");
  }
  BackendDescriptor& backend = existing->second;
  if (!(backend.backend_boot == evidence.backend_boot)) {
    return MutationResult::make(OutcomeCode::REJECT_STALE_BACKEND_BOOT, subject,
                                "evidence belongs to a different backend incarnation");
  }
  if (evidence.backend_generation.valid() &&
      !(evidence.backend_generation == backend.backend_generation)) {
    return MutationResult::make(OutcomeCode::REJECT_STALE_BACKEND, subject,
                                "evidence backend generation is not current");
  }
  (void)unknown_code;

  // The current evidence object for this backend.
  auto* current = [&backend]() -> Evidence* {
    if constexpr (std::is_same_v<Evidence, HealthEvidence>) {
      return &backend.health;
    } else if constexpr (std::is_same_v<Evidence, AvailabilityEvidence>) {
      return &backend.availability;
    } else if constexpr (std::is_same_v<Evidence, ReadinessEvidence>) {
      return &backend.readiness;
    } else if constexpr (std::is_same_v<Evidence, CapacityEvidence>) {
      return &backend.capacity;
    } else {
      return &backend.residency;
    }
  }();

  if (generation_regressed(evidence.generation.value(), current->generation.value())) {
    return MutationResult::make(stale_code, subject, "evidence generation regressed");
  }
  if (current->generation.valid() && evidence.generation == current->generation) {
    if (current->state == evidence.state && current->expires_at_unix_millis ==
                                                evidence.expires_at_unix_millis) {
      return MutationResult::make(OutcomeCode::NO_CHANGE, subject,
                                  "evidence generation is already current");
    }
    return MutationResult::make(OutcomeCode::REJECT_CONFLICT, subject,
                                "evidence state changed without advancing its generation");
  }
  *current = std::move(evidence);
  state.stale_decisions_for_authority_change(now);
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, subject);
  builder.add_u64("backend_id", backend.backend_id.value());
  builder.add_u64("backend_boot", backend.backend_boot.value());
  builder.add_u64("generation", current->generation.value());
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

}  // namespace

MutationResult ModelRouter::Impl::update_health(HealthEvidence evidence) {
  return apply_evidence(state, mutex, std::move(evidence), "update_health", now(),
                        OutcomeCode::REJECT_STALE_HEALTH, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
}

MutationResult ModelRouter::Impl::update_availability(AvailabilityEvidence evidence) {
  return apply_evidence(state, mutex, std::move(evidence), "update_availability", now(),
                        OutcomeCode::REJECT_STALE_AVAILABILITY,
                        OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
}

MutationResult ModelRouter::Impl::update_readiness(ReadinessEvidence evidence) {
  return apply_evidence(state, mutex, std::move(evidence), "update_readiness", now(),
                        OutcomeCode::REJECT_STALE_READINESS,
                        OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
}

MutationResult ModelRouter::Impl::update_residency(ResidencyEvidence evidence) {
  return apply_evidence(state, mutex, std::move(evidence), "update_residency", now(),
                        OutcomeCode::REJECT_STALE_RESIDENCY,
                        OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
}

MutationResult ModelRouter::Impl::update_capacity(CapacityEvidence evidence, CapacityDetail detail) {
  if (detail.backend_id.valid() && !(detail.backend_id == evidence.backend_id)) {
    return reject(OutcomeCode::REJECT_CONFLICT, "update_capacity",
                  "capacity detail belongs to a different backend");
  }
  if (detail.backend_boot.valid() && !(detail.backend_boot == evidence.backend_boot)) {
    return reject(OutcomeCode::REJECT_CONFLICT, "update_capacity",
                  "capacity detail belongs to a different backend incarnation");
  }
  MutationResult result = apply_evidence(state, mutex, std::move(evidence), "update_capacity", now(),
                                         OutcomeCode::REJECT_STALE_CAPACITY,
                                         OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
  if (!result.accepted()) {
    return result;
  }
  std::unique_lock lock(mutex);
  const auto existing = state.backends.find(detail.backend_id);
  if (existing == state.backends.end()) {
    return result;
  }
  detail.generation = existing->second.capacity.generation;
  existing->second.capacity_detail = detail;
  return result;
}

MutationResult ModelRouter::Impl::update_latency(LatencyEvidence evidence) {
  if (!evidence.backend_id.valid() || !evidence.backend_boot.valid()) {
    return reject(OutcomeCode::REJECT_INVALID, "update_latency",
                  "latency evidence has no valid backend identity or incarnation");
  }
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return reject(OutcomeCode::SHUTTING_DOWN, "update_latency", "router is shutting down");
  }
  if (state.fenced_boots.find(FenceKey{evidence.backend_id, evidence.backend_boot}) !=
      state.fenced_boots.end()) {
    return reject(OutcomeCode::REJECT_STALE_BACKEND_BOOT, "update_latency",
                  "latency evidence belongs to a fenced backend incarnation");
  }
  const auto existing = state.backends.find(evidence.backend_id);
  if (existing == state.backends.end()) {
    return reject(OutcomeCode::REJECT_STALE_BACKEND, "update_latency",
                  "backend is not registered");
  }
  BackendDescriptor& backend = existing->second;
  if (!(backend.backend_boot == evidence.backend_boot)) {
    return reject(OutcomeCode::REJECT_STALE_BACKEND_BOOT, "update_latency",
                  "latency evidence belongs to a different backend incarnation");
  }
  if (generation_regressed(evidence.health_generation.value(),
                           backend.latency.health_generation.value())) {
    return reject(OutcomeCode::REJECT_STALE_HEALTH, "update_latency",
                  "latency evidence generation regressed");
  }
  backend.latency = std::move(evidence);
  state.stale_decisions_for_authority_change(now());
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "update_latency");
  builder.add_u64("backend_id", backend.backend_id.value());
  builder.add_u64("backend_boot", backend.backend_boot.value());
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult ModelRouter::Impl::set_policy(PolicySnapshot policy) {
  policy.canonicalize();
  if (const std::string error = policy.validate(); !error.empty()) {
    return reject(OutcomeCode::REJECT_INVALID, "set_policy", error);
  }
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return reject(OutcomeCode::SHUTTING_DOWN, "set_policy", "router is shutting down");
  }
  if (state.policy.generation.valid() &&
      generation_regressed(policy.generation.value(), state.policy.generation.value())) {
    return reject(OutcomeCode::REJECT_STALE_POLICY, "set_policy", "policy generation regressed");
  }
  if (state.policy.generation.valid() && policy.generation == state.policy.generation &&
      !(state.policy.policy_id == policy.policy_id)) {
    return reject(OutcomeCode::REJECT_CONFLICT, "set_policy",
                  "policy identity changed without advancing the policy generation");
  }
  state.policy = std::move(policy);
  state.stale_decisions_for_authority_change(now());
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "set_policy");
  builder.add_u64("policy_id", state.policy.policy_id.value());
  builder.add_u64("policy_generation", state.policy.generation.value());
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult ModelRouter::Impl::set_budget(BudgetSnapshot budget) {
  if (!budget.generation.valid()) {
    return reject(OutcomeCode::REJECT_INVALID, "set_budget", "budget generation is invalid");
  }
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return reject(OutcomeCode::SHUTTING_DOWN, "set_budget", "router is shutting down");
  }
  if (state.budget.generation.valid() &&
      generation_regressed(budget.generation.value(), state.budget.generation.value())) {
    return reject(OutcomeCode::REJECT_STALE_BUDGET, "set_budget", "budget generation regressed");
  }
  state.budget = std::move(budget);
  state.stale_decisions_for_authority_change(now());
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "set_budget");
  builder.add_u64("budget_id", state.budget.budget_id.value());
  builder.add_u64("budget_generation", state.budget.generation.value());
  builder.add("verdict", std::string(to_string(state.budget.verdict)));
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult ModelRouter::Impl::set_slo(SloEvidence slo) {
  if (!slo.generation.valid()) {
    return reject(OutcomeCode::REJECT_INVALID, "set_slo", "SLO generation is invalid");
  }
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return reject(OutcomeCode::SHUTTING_DOWN, "set_slo", "router is shutting down");
  }
  if (state.slo.generation.valid() &&
      generation_regressed(slo.generation.value(), state.slo.generation.value())) {
    return reject(OutcomeCode::REJECT_STALE_SLO, "set_slo", "SLO generation regressed");
  }
  state.slo = std::move(slo);
  state.stale_decisions_for_authority_change(now());
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "set_slo");
  builder.add_u64("slo_id", state.slo.slo_id.value());
  builder.add_u64("slo_generation", state.slo.generation.value());
  builder.add("verdict", std::string(to_string(state.slo.verdict)));
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult ModelRouter::Impl::set_compatibility(CompatibilityEvidence evidence) {
  if (!evidence.backend_id.valid() || !evidence.generation.valid()) {
    return reject(OutcomeCode::REJECT_INVALID, "set_compatibility",
                  "compatibility evidence has no valid backend identity or generation");
  }
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return reject(OutcomeCode::SHUTTING_DOWN, "set_compatibility", "router is shutting down");
  }
  const auto existing = state.compatibility.find(evidence.backend_id);
  if (existing != state.compatibility.end() &&
      generation_regressed(evidence.generation.value(), existing->second.generation.value())) {
    return reject(OutcomeCode::REJECT_STALE_COMPATIBILITY, "set_compatibility",
                  "compatibility generation regressed");
  }
  state.compatibility[evidence.backend_id] = std::move(evidence);
  state.stale_decisions_for_authority_change(now());
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "set_compatibility");
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult ModelRouter::Impl::set_trust(TrustEvidence evidence) {
  if (!evidence.profile_id.valid() || !evidence.generation.valid() || !evidence.backend_id.valid()) {
    return reject(OutcomeCode::REJECT_INVALID, "set_trust",
                  "trust evidence has no valid profile identity, generation, or backend identity");
  }
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return reject(OutcomeCode::SHUTTING_DOWN, "set_trust", "router is shutting down");
  }
  const auto existing = state.trust.find(evidence.backend_id);
  if (existing != state.trust.end()) {
    if (generation_regressed(evidence.generation.value(), existing->second.generation.value())) {
      return reject(OutcomeCode::REJECT_STALE_TRUST, "set_trust", "trust generation regressed");
    }
    if (evidence.generation == existing->second.generation &&
        existing->second.domain == evidence.domain) {
      return MutationResult::make(OutcomeCode::NO_CHANGE, "set_trust",
                                  "trust generation is already current");
    }
  }
  const BackendId backend_id = evidence.backend_id;
  state.trust[backend_id] = std::move(evidence);
  const auto backend = state.backends.find(backend_id);
  if (backend != state.backends.end()) {
    state.index_remove_backend(backend->second);
    const TrustEvidence& stored = state.trust[backend_id];
    backend->second.trust_profile_id = stored.profile_id;
    backend->second.trust_generation = stored.generation;
    backend->second.trust_domain = stored.domain;
    state.index_add_backend(backend->second);
  }
  state.stale_decisions_for_authority_change(now());
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "set_trust");
  builder.add_u64("backend_id", backend_id.value());
  builder.add_u64("trust_profile_id", state.trust[backend_id].profile_id.value());
  builder.add_u64("trust_generation", state.trust[backend_id].generation.value());
  builder.add("domain", std::string(to_string(state.trust[backend_id].domain)));
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult ModelRouter::Impl::set_cost(CostEvidence evidence) {
  if (!evidence.price_generation.valid() || !evidence.backend_id.valid() ||
      !evidence.model_id.valid()) {
    return reject(OutcomeCode::REJECT_INVALID, "set_cost",
                  "cost evidence has no valid price generation, backend identity, or model "
                  "identity");
  }
  if (evidence.knownness != CostKnownness::UNKNOWN && !evidence.unit.valid()) {
    return reject(OutcomeCode::REJECT_INVALID, "set_cost",
                  "known cost evidence has no cost unit identity");
  }
  if (evidence.estimated_total_micros < 0) {
    return reject(OutcomeCode::REJECT_INVALID, "set_cost",
                  "cost evidence has a negative estimated total");
  }
  std::unique_lock lock(mutex);
  if (state.shutting_down) {
    return reject(OutcomeCode::SHUTTING_DOWN, "set_cost", "router is shutting down");
  }
  const auto backend = state.backends.find(evidence.backend_id);
  if (backend == state.backends.end()) {
    return reject(OutcomeCode::REJECT_STALE_BACKEND, "set_cost", "backend is not registered");
  }
  const auto binding = std::lower_bound(
      backend->second.model_bindings.begin(), backend->second.model_bindings.end(),
      evidence.model_id, [](const ModelBinding& value, ModelId key) {
        return value.model_id < key;
      });
  if (binding == backend->second.model_bindings.end() || !(binding->model_id == evidence.model_id)) {
    return reject(OutcomeCode::REJECT_STALE_MODEL, "set_cost",
                  "backend does not bind the model the cost evidence refers to");
  }
  if (generation_regressed(evidence.price_generation.value(),
                           binding->cost.price_generation.value())) {
    return reject(OutcomeCode::REJECT_STALE_PRICE, "set_cost", "price generation regressed");
  }
  if (binding->cost.price_generation.valid() &&
      evidence.price_generation == binding->cost.price_generation &&
      binding->cost.knownness == evidence.knownness &&
      binding->cost.estimated_total_micros == evidence.estimated_total_micros &&
      binding->cost.unit == evidence.unit) {
    return MutationResult::make(OutcomeCode::NO_CHANGE, "set_cost",
                                "price generation is already current");
  }
  const BackendId backend_id = evidence.backend_id;
  const ModelId model_id = evidence.model_id;
  const PriceGeneration price_generation = evidence.price_generation;
  const CostKnownness knownness = evidence.knownness;
  const CostUnit unit = evidence.unit;
  const std::int64_t total = evidence.estimated_total_micros;
  binding->cost = std::move(evidence);
  state.costs[std::make_pair(backend_id, model_id)] = binding->cost;
  state.stale_decisions_for_authority_change(now());

  ExplanationBuilder builder(OutcomeCode::ACCEPTED, "set_cost");
  builder.add_u64("backend_id", backend_id.value());
  builder.add_u64("model_id", model_id.value());
  builder.add_u64("price_generation", price_generation.value());
  builder.add("knownness", std::string(to_string(knownness)));
  builder.add("currency", unit.currency);
  builder.add("basis", unit.basis);
  builder.add_i64("estimated_total_micros", total);
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

// ---------------------------------------------------------------------------
// Public API forwarding
// ---------------------------------------------------------------------------

ModelRouter::ModelRouter(ModelRouterOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

ModelRouter::~ModelRouter() = default;

MutationResult ModelRouter::start() { return impl_->start(); }
MutationResult ModelRouter::shutdown() { return impl_->shutdown(); }
bool ModelRouter::running() const noexcept {
  std::shared_lock lock(impl_->mutex);
  return impl_->state.running;
}
bool ModelRouter::shutting_down() const noexcept {
  std::shared_lock lock(impl_->mutex);
  return impl_->state.shutting_down;
}

MutationResult ModelRouter::register_model(ModelDescriptor model) {
  return impl_->register_model(std::move(model));
}
MutationResult ModelRouter::register_provider(ProviderDescriptor provider) {
  return impl_->register_provider(std::move(provider));
}
MutationResult ModelRouter::register_backend(BackendDescriptor backend) {
  return impl_->register_backend(std::move(backend));
}
MutationResult ModelRouter::unregister_backend(BackendId backend_id,
                                               BackendGeneration generation) {
  return impl_->unregister_backend(backend_id, generation);
}
MutationResult ModelRouter::publish_capabilities(BackendCapabilityPublication publication) {
  return impl_->publish_capabilities(std::move(publication));
}
MutationResult ModelRouter::retire_model(ModelId model_id, ModelGeneration generation) {
  return impl_->retire_model(model_id, generation);
}
MutationResult ModelRouter::fence_backend_boot(BackendId backend_id, BackendGeneration generation,
                                               BackendBootId backend_boot, OutcomeCode reason) {
  return impl_->fence_backend_boot(backend_id, generation, backend_boot, reason);
}

MutationResult ModelRouter::update_health(HealthEvidence evidence) {
  return impl_->update_health(std::move(evidence));
}
MutationResult ModelRouter::update_availability(AvailabilityEvidence evidence) {
  return impl_->update_availability(std::move(evidence));
}
MutationResult ModelRouter::update_readiness(ReadinessEvidence evidence) {
  return impl_->update_readiness(std::move(evidence));
}
MutationResult ModelRouter::update_residency(ResidencyEvidence evidence) {
  return impl_->update_residency(std::move(evidence));
}
MutationResult ModelRouter::update_capacity(CapacityEvidence evidence, CapacityDetail detail) {
  return impl_->update_capacity(std::move(evidence), std::move(detail));
}
MutationResult ModelRouter::update_latency(LatencyEvidence evidence) {
  return impl_->update_latency(std::move(evidence));
}

MutationResult ModelRouter::set_policy(PolicySnapshot policy) {
  return impl_->set_policy(std::move(policy));
}
MutationResult ModelRouter::set_budget(BudgetSnapshot budget) {
  return impl_->set_budget(std::move(budget));
}
MutationResult ModelRouter::set_slo(SloEvidence slo) { return impl_->set_slo(std::move(slo)); }
MutationResult ModelRouter::set_compatibility(CompatibilityEvidence evidence) {
  return impl_->set_compatibility(std::move(evidence));
}
MutationResult ModelRouter::set_trust(TrustEvidence evidence) {
  return impl_->set_trust(std::move(evidence));
}
MutationResult ModelRouter::set_cost(CostEvidence evidence) {
  return impl_->set_cost(std::move(evidence));
}

RouteOutcome ModelRouter::route(RouteRequest request) { return impl_->route(std::move(request)); }

MutationResult ModelRouter::revalidate(RouteDecisionId decision_id, TenantId caller_tenant,
                                       NamespaceId caller_namespace) {
  return impl_->revalidate(decision_id, caller_tenant, caller_namespace);
}

DispatchRecord ModelRouter::dispatch(RouteDecisionId decision_id, TenantId caller_tenant,
                                     NamespaceId caller_namespace) {
  return impl_->dispatch(decision_id, caller_tenant, caller_namespace);
}

MutationResult ModelRouter::record_completion(const CompletionRecord& completion) {
  return impl_->record_completion(completion);
}

RouteOutcome ModelRouter::reroute(RouteDecisionId decision_id, FailureClass failure,
                                  RouteRequest request) {
  return impl_->reroute(decision_id, failure, std::move(request));
}

RouterSnapshot ModelRouter::snapshot() const { return impl_->snapshot(); }
RouterSummary ModelRouter::summary() const { return impl_->summary(); }
InvariantReport ModelRouter::check_invariants() const { return impl_->check_invariants(); }

bool ModelRouter::find_model(ModelId model_id, ModelDescriptor* out) const {
  return impl_->find_model(model_id, out);
}
bool ModelRouter::find_backend(BackendId backend_id, BackendDescriptor* out) const {
  return impl_->find_backend(backend_id, out);
}
bool ModelRouter::find_decision(RouteDecisionId decision_id, RouteDecision* out) const {
  return impl_->find_decision(decision_id, out);
}
std::vector<RouteDecision> ModelRouter::route_history() const { return impl_->route_history(); }
std::vector<FencedBootRecord> ModelRouter::fenced_boots() const { return impl_->fenced_boots(); }
std::string ModelRouter::explain_route(RouteDecisionId decision_id) const {
  return impl_->explain_route(decision_id);
}

RouterId ModelRouter::router_id() const noexcept {
  std::shared_lock lock(impl_->mutex);
  return impl_->state.router_id;
}
RouterEpoch ModelRouter::router_epoch() const noexcept {
  std::shared_lock lock(impl_->mutex);
  return impl_->state.router_epoch;
}
CoordinatorEpoch ModelRouter::coordinator_epoch() const noexcept {
  std::shared_lock lock(impl_->mutex);
  return impl_->state.coordinator_epoch;
}
RouterBootId ModelRouter::router_boot() const noexcept {
  std::shared_lock lock(impl_->mutex);
  return impl_->state.router_boot;
}

PersistenceResult ModelRouter::save(const std::string& path) { return impl_->save(path); }
PersistenceResult ModelRouter::load(const std::string& path) { return impl_->load(path); }

}  // namespace model_router
