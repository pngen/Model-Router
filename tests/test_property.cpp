// Model Router - deterministic randomized property sequences.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// A seeded generator drives long sequences of canonical mutations, routing,
// dispatch, persistence, and lifecycle operations. After EVERY operation the
// full invariant scan runs; a violation prints the seed, the operation index,
// and the complete operation history before failing.
//
// Determinism is itself a property: replaying a seed against a fresh router must
// reproduce the identical operation history and the identical routing outcome
// for every operation.

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "router_fixture.hpp"

using model_router::BackendBootId;
using model_router::BackendGeneration;
using model_router::BackendId;
using model_router::ModelGeneration;
using model_router::OutcomeCode;
using model_router::PolicyGeneration;
using model_router::PolicyId;
using model_router::PriceGeneration;
using model_router::RouteOutcome;
using model_router::TenantId;

namespace {

constexpr int kOperationsPerSeed = 420;
constexpr std::uint64_t kSeeds[] = {0x9E3779B97F4A7C15ull, 0x0123456789ABCDEFull,
                                    0xDEADBEEFCAFEF00Dull};
constexpr TenantId kTenant(1);
constexpr TenantId kOtherTenant(2);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);

/// Deterministic 64-bit generator (splitmix64). Every sequence is reproducible
/// from its seed alone.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  [[nodiscard]] std::uint64_t next() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
  }

  [[nodiscard]] std::uint64_t below(std::uint64_t bound) {
    return bound == 0 ? 0 : next() % bound;
  }

  [[nodiscard]] bool chance(std::uint64_t numerator, std::uint64_t denominator) {
    return below(denominator) < numerator;
  }

 private:
  std::uint64_t state_;
};

[[nodiscard]] model_router::CapabilityRequirement require_capability(std::string key) {
  model_router::CapabilityRequirement requirement;
  requirement.key = model_router::CapabilityKey(std::move(key));
  requirement.minimum_state = model_router::CapabilityState::DECLARED;
  return requirement;
}

/// A retained decision plus the tenancy it belongs to.
struct TrackedDecision {
  model_router::RouteDecisionId id{};
  TenantId tenant{};
};

/// One backend incarnation as the driver knows it.
struct TrackedBackend {
  BackendId id{};
  BackendGeneration generation{};
  BackendBootId boot{};
  model_router::BackendRegistrationGeneration registration{};
};

enum class OpKind {
  REGISTER_MODEL,
  MODEL_GENERATION_CHANGE,
  REGISTER_BACKEND,
  REINCARNATE_BACKEND,
  PUBLISH_CAPABILITIES,
  HEALTH,
  AVAILABILITY,
  READINESS,
  CAPACITY,
  PRICE,
  POLICY,
  BUDGET,
  SLO,
  TRUST,
  COMPATIBILITY,
  DISCOVERY,
  ROUTE,
  REVALIDATE,
  DISPATCH,
  REROUTE,
  BACKEND_DEATH,
  FENCE,
  RETIRE,
  SAVE,
  LOAD,
  START,
  SHUTDOWN,
  kCount
};

[[nodiscard]] const char* op_name(OpKind kind) {
  switch (kind) {
    case OpKind::REGISTER_MODEL: return "register_model";
    case OpKind::MODEL_GENERATION_CHANGE: return "model_generation_change";
    case OpKind::REGISTER_BACKEND: return "register_backend";
    case OpKind::REINCARNATE_BACKEND: return "reincarnate_backend";
    case OpKind::PUBLISH_CAPABILITIES: return "publish_capabilities";
    case OpKind::HEALTH: return "health";
    case OpKind::AVAILABILITY: return "availability";
    case OpKind::READINESS: return "readiness";
    case OpKind::CAPACITY: return "capacity";
    case OpKind::PRICE: return "price";
    case OpKind::POLICY: return "policy";
    case OpKind::BUDGET: return "budget";
    case OpKind::SLO: return "slo";
    case OpKind::TRUST: return "trust";
    case OpKind::COMPATIBILITY: return "compatibility";
    case OpKind::DISCOVERY: return "discovery";
    case OpKind::ROUTE: return "route";
    case OpKind::REVALIDATE: return "revalidate";
    case OpKind::DISPATCH: return "dispatch";
    case OpKind::REROUTE: return "reroute";
    case OpKind::BACKEND_DEATH: return "backend_death";
    case OpKind::FENCE: return "fence";
    case OpKind::RETIRE: return "retire";
    case OpKind::SAVE: return "save";
    case OpKind::LOAD: return "load";
    case OpKind::START: return "start";
    case OpKind::SHUTDOWN: return "shutdown";
    default: return "invalid";
  }
}

/// Drives one reproducible operation sequence against a fresh router.
class Driver {
 public:
  Driver(std::uint64_t seed, const std::string& state_path)
      : seed_(seed), rng_(seed), state_path_(state_path) {
    fixture_.start(64);
    MR_CHECK(mrtest::install_open_policy(*fixture_.router, kPolicy, kPolicyGeneration).accepted());
    policy_generation_ = kPolicyGeneration;
  }

  /// Applies p operations and returns the canonical operation history.
  [[nodiscard]] std::vector<std::string> run(int operations) {
    std::vector<std::string> history;
    history.reserve(static_cast<std::size_t>(operations));
    for (int index = 0; index < operations; ++index) {
      OpKind kind = static_cast<OpKind>(rng_.below(static_cast<std::uint64_t>(OpKind::kCount)));
      // Shutdown ends the useful life of a sequence, so it is only exercised
      // near the end; every remaining operation must still be safe.
      if (kind == OpKind::SHUTDOWN && index + 8 < operations) {
        kind = OpKind::ROUTE;
      }
      const std::string line = std::to_string(index) + " " + op_name(kind) + " " + apply(kind);
      history.push_back(line);
      const model_router::InvariantReport report = fixture_.router->check_invariants();
      if (!report.ok()) {
        std::string message = "invariants violated after operation " + std::to_string(index) +
                              " (seed=0x" + hex(seed_) + ")\n" + report.to_text() + "history:\n";
        for (const std::string& entry : history) {
          message += "  " + entry + "\n";
        }
        ::mrtest::fail(__FILE__, __LINE__, message);
      }
    }
    return history;
  }

  [[nodiscard]] model_router::ModelRouter& router() { return *fixture_.router; }

 private:
  [[nodiscard]] static std::string hex(std::uint64_t value) {
    std::string out;
    for (int shift = 60; shift >= 0; shift -= 4) {
      const std::uint64_t digit = (value >> shift) & 0xF;
      out.push_back(static_cast<char>(digit < 10 ? '0' + digit : 'a' + (digit - 10)));
    }
    return out;
  }

  [[nodiscard]] model_router::UnixMillis now() const { return fixture_.now(); }

  [[nodiscard]] model_router::ModelId model_id(std::uint64_t index) const {
    switch (index % 3) {
      case 0: return fixture_.catalog.small_model;
      case 1: return fixture_.catalog.general_model;
      default: return fixture_.catalog.specialist_model;
    }
  }

  [[nodiscard]] ModelGeneration model_generation(std::uint64_t index) const {
    return model_generations_[index % 3];
  }

  /// Builds a reference backend whose bindings carry the driver's current model
  /// generations, so a model generation change is visible to the bindings.
  [[nodiscard]] model_router::BackendDescriptor make_backend(BackendId id,
                                                             BackendBootId boot,
                                                             std::uint64_t registration) {
    model_router::BackendDescriptor backend = model_router::reference::make_backend(
        fixture_.catalog, id, BackendGeneration(1), boot,
        model_router::BackendRegistrationGeneration(registration),
        "127.0.0.1:" + std::to_string(7000 + id.value()),
        model_router::Provenance::SYNTHETIC, now());
    for (model_router::ModelBinding& binding : backend.model_bindings) {
      const std::uint64_t index = binding.model_id.value() - 1;
      binding.model_generation = model_generations_[index % 3];
      binding.artifact_generation = artifact_generations_[index % 3];
    }
    backend.canonicalize();
    return backend;
  }

  /// Re-reads the registered incarnations after recovery replaced canonical state.
  void rebuild_backends_from_state() {
    backends_.clear();
    const model_router::RouterSnapshot snapshot = fixture_.router->snapshot();
    for (const model_router::BackendSummary& summary : snapshot.backends) {
      model_router::BackendDescriptor descriptor;
      if (!fixture_.router->find_backend(summary.backend_id, &descriptor)) {
        continue;
      }
      backends_.push_back(TrackedBackend{descriptor.backend_id, descriptor.backend_generation,
                                         descriptor.backend_boot,
                                         descriptor.backend_registration_generation});
    }
  }

  void publish_evidence(BackendId id, BackendBootId boot) {
    const model_router::UnixMillis current = now();
    (void)fixture_.publisher.health(*fixture_.router, id, boot,
                                    rng_.chance(3, 4) ? model_router::HealthState::HEALTHY
                                                      : model_router::HealthState::DEGRADED,
                                    current);
    (void)fixture_.publisher.availability(
        *fixture_.router, id, boot,
        rng_.chance(4, 5) ? model_router::AvailabilityState::AVAILABLE
                          : model_router::AvailabilityState::UNAVAILABLE,
        current);
    (void)fixture_.publisher.readiness(
        *fixture_.router, id, boot,
        rng_.chance(4, 5) ? model_router::ReadinessState::READY
                          : model_router::ReadinessState::WARMING,
        current);
    (void)fixture_.publisher.residency(*fixture_.router, id, boot,
                                       model_router::ResidencyState::RESIDENT, current);
    (void)fixture_.publisher.capacity(*fixture_.router, id, boot,
                                      model_router::CapacityState::AVAILABLE,
                                      static_cast<std::uint32_t>(rng_.below(16)),
                                      static_cast<std::uint32_t>(16 + rng_.below(16)), current);
    (void)fixture_.publisher.latency(*fixture_.router, id, boot,
                                     static_cast<std::uint32_t>(500 + rng_.below(5000)),
                                     static_cast<std::uint32_t>(rng_.below(500)), current);
  }

  [[nodiscard]] TrackedBackend* pick_backend() {
    if (backends_.empty()) {
      return nullptr;
    }
    return &backends_[rng_.below(backends_.size())];
  }

  [[nodiscard]] const TrackedDecision* pick_decision() {
    if (decisions_.empty()) {
      return nullptr;
    }
    return &decisions_[rng_.below(decisions_.size())];
  }

  [[nodiscard]] model_router::RouteRequest make_request(TenantId tenant) {
    model_router::RouteRequest request;
    request.request_id = model_router::allocate_id<model_router::RouteRequestTag>();
    request.request_generation = model_router::RouteRequestGeneration(1);
    request.tenant = tenant;
    request.name_space = model_router::NamespaceId(1);
    request.policy_id = kPolicy;
    request.policy_generation = fixture_.router->summary().policy_generation;
    request.created_at_unix_millis = now();
    request.estimated_input_tokens = 1000;
    request.estimated_output_tokens = 500;
    request.requirements.required_input_modalities =
        model_router::ModalitySet(static_cast<std::uint32_t>(model_router::Modality::TEXT));
    request.requirements.required_output_modalities =
        model_router::ModalitySet(static_cast<std::uint32_t>(model_router::Modality::TEXT));
    if (rng_.chance(1, 3)) {
      request.requirements.required_capabilities.push_back(
          require_capability(std::string(model_router::capability_keys::code)));
    }
    if (rng_.chance(1, 4)) {
      request.requirements.min_context_tokens = 100000;
    }
    if (rng_.chance(1, 3)) {
      request.requirements.require_known_cost = true;
    }
    const model_router::RouterSummary summary = fixture_.router->summary();
    if (summary.budget_generation.valid() && rng_.chance(1, 3)) {
      request.budget_id = model_router::BudgetId(1);
      request.budget_generation = summary.budget_generation;
    }
    return request;
  }

  [[nodiscard]] std::string describe_route(const RouteOutcome& outcome) const {
    std::string line = "code=" + std::string(model_router::to_string(outcome.code));
    line += " has_decision=" + std::string(outcome.has_decision ? "1" : "0");
    if (outcome.has_decision) {
      line += " winner=" + outcome.decision.explanation.winner.to_string();
    }
    line += " ranking=" + std::to_string(outcome.route.ranking.size());
    line += " rejections=" + std::to_string(outcome.route.rejections.size());
    line += " eligible=" + std::to_string(outcome.route.eligible_candidate_count);
    if (!outcome.route.ranking.empty()) {
      line += " top_score=" + std::to_string(outcome.route.ranking.front().score);
    }
    return line;
  }

  [[nodiscard]] std::string apply(OpKind kind) {
    switch (kind) {
      case OpKind::REGISTER_MODEL:
      case OpKind::MODEL_GENERATION_CHANGE: {
        const std::uint64_t index = rng_.below(3);
        const bool bump = kind == OpKind::MODEL_GENERATION_CHANGE;
        std::vector<model_router::ModelDescriptor> models =
            model_router::reference::make_models(fixture_.catalog, now());
        model_router::ModelDescriptor model = models[static_cast<std::size_t>(index)];
        if (bump) {
          model_generations_[index] = model_generations_[index].next();
          artifact_generations_[index] = artifact_generations_[index].next();
        }
        model.model_generation = model_generations_[index];
        model.artifact_generation = artifact_generations_[index];
        model.lifecycle = model_router::ModelLifecycle::CURRENT;
        model.registered_at_unix_millis = now();
        const model_router::MutationResult result = fixture_.router->register_model(model);
        return "code=" + std::string(model_router::to_string(result.code)) +
               " model=" + std::to_string(model.model_id.value()) +
               " generation=" + std::to_string(model.model_generation.value());
      }
      case OpKind::REGISTER_BACKEND:
      case OpKind::REINCARNATE_BACKEND: {
        const BackendId id(1 + rng_.below(3));
        TrackedBackend* existing = nullptr;
        for (TrackedBackend& backend : backends_) {
          if (backend.id == id) {
            existing = &backend;
          }
        }
        const std::uint64_t registration =
            existing == nullptr ? 1 : existing->registration.value() + 1;
        const std::uint64_t boot = existing == nullptr ? 1 : existing->boot.value() + 1;
        const model_router::MutationResult result =
            fixture_.router->register_backend(make_backend(id, BackendBootId(boot), registration));
        if (result.accepted()) {
          if (existing == nullptr) {
            backends_.push_back(TrackedBackend{id, BackendGeneration(1), BackendBootId(boot),
                                               model_router::BackendRegistrationGeneration(registration)});
          } else {
            existing->boot = BackendBootId(boot);
            existing->registration = model_router::BackendRegistrationGeneration(registration);
          }
          publish_evidence(id, BackendBootId(boot));
        }
        return "code=" + std::string(model_router::to_string(result.code)) +
               " backend=" + std::to_string(id.value()) + " boot=" + std::to_string(boot);
      }
      case OpKind::PUBLISH_CAPABILITIES: {
        const TrackedBackend* backend = pick_backend();
        if (backend == nullptr) {
          return "skipped=no_backend";
        }
        model_router::BackendDescriptor descriptor;
        if (!fixture_.router->find_backend(backend->id, &descriptor)) {
          return "skipped=backend_not_registered";
        }
        model_router::BackendCapabilityPublication publication;
        publication.backend_id = backend->id;
        publication.backend_generation = backend->generation;
        publication.backend_boot = backend->boot;
        publication.profile_id = model_router::CapabilityProfileId(backend->id.value() * 100 + 2);
        publication.generation = descriptor.capability_generation.next();
        model_router::CapabilityEvidence generation_claim;
        generation_claim.key =
            model_router::CapabilityKey(std::string(model_router::capability_keys::text_generation));
        generation_claim.state = model_router::CapabilityState::VERIFIED;
        publication.claims.push_back(generation_claim);
        model_router::CapabilityEvidence code_claim;
        code_claim.key = model_router::CapabilityKey(std::string(model_router::capability_keys::code));
        code_claim.state = rng_.chance(1, 2) ? model_router::CapabilityState::VERIFIED
                                             : model_router::CapabilityState::REVOKED;
        publication.claims.push_back(code_claim);
        const model_router::MutationResult result =
            fixture_.router->publish_capabilities(std::move(publication));
        return "code=" + std::string(model_router::to_string(result.code)) +
               " backend=" + std::to_string(backend->id.value());
      }
      case OpKind::HEALTH:
      case OpKind::AVAILABILITY:
      case OpKind::READINESS:
      case OpKind::CAPACITY: {
        const TrackedBackend* backend = pick_backend();
        if (backend == nullptr) {
          return "skipped=no_backend";
        }
        model_router::MutationResult result;
        if (kind == OpKind::HEALTH) {
          const model_router::HealthState states[3] = {model_router::HealthState::HEALTHY,
                                                       model_router::HealthState::DEGRADED,
                                                       model_router::HealthState::UNHEALTHY};
          result = fixture_.publisher.health(*fixture_.router, backend->id, backend->boot,
                                             states[rng_.below(3)], now());
        } else if (kind == OpKind::AVAILABILITY) {
          result = fixture_.publisher.availability(
              *fixture_.router, backend->id, backend->boot,
              rng_.chance(3, 4) ? model_router::AvailabilityState::AVAILABLE
                                : model_router::AvailabilityState::UNAVAILABLE,
              now());
        } else if (kind == OpKind::READINESS) {
          const model_router::ReadinessState states[3] = {model_router::ReadinessState::READY,
                                                          model_router::ReadinessState::WARMING,
                                                          model_router::ReadinessState::NOT_READY};
          result = fixture_.publisher.readiness(*fixture_.router, backend->id, backend->boot,
                                                states[rng_.below(3)], now());
        } else {
          result = fixture_.publisher.capacity(
              *fixture_.router, backend->id, backend->boot,
              rng_.chance(3, 4) ? model_router::CapacityState::AVAILABLE
                                : model_router::CapacityState::SATURATED,
              static_cast<std::uint32_t>(rng_.below(16)),
              static_cast<std::uint32_t>(16 + rng_.below(16)), now());
        }
        return "code=" + std::string(model_router::to_string(result.code)) +
               " backend=" + std::to_string(backend->id.value());
      }
      case OpKind::PRICE: {
        const TrackedBackend* backend = pick_backend();
        if (backend == nullptr) {
          return "skipped=no_backend";
        }
        const model_router::ModelId model = model_id(rng_.below(3));
        model_router::BackendDescriptor descriptor;
        if (!fixture_.router->find_backend(backend->id, &descriptor)) {
          return "skipped=backend_not_registered";
        }
        const model_router::ModelBinding* binding = descriptor.find_binding(model);
        const PriceGeneration generation =
            binding == nullptr ? PriceGeneration(1) : binding->cost.price_generation.next();
        const std::int64_t total = static_cast<std::int64_t>(rng_.below(1000) + 1);
        const model_router::MutationResult result = fixture_.router->set_cost(
            model_router::reference::make_cost_evidence(backend->id, model, generation, total,
                                                        total, total, now()));
        return "code=" + std::string(model_router::to_string(result.code)) +
               " backend=" + std::to_string(backend->id.value()) +
               " model=" + std::to_string(model.value()) +
               " price_generation=" + std::to_string(generation.value());
      }
      case OpKind::POLICY: {
        policy_generation_ = fixture_.router->summary().policy_generation.next();
        model_router::PolicyBuilder builder(kPolicy, policy_generation_);
        builder.fallback_policy(model_router::FallbackPolicy::PERMITTED).max_fallback_depth(4);
        if (rng_.chance(1, 3)) {
          builder.deny_backend(BackendId(1 + rng_.below(3)));
        }
        if (rng_.chance(1, 4)) {
          builder.deny_model(model_id(rng_.below(3)));
        }
        const model_router::MutationResult result = fixture_.router->set_policy(builder.build());
        return "code=" + std::string(model_router::to_string(result.code)) +
               " policy_generation=" + std::to_string(policy_generation_.value());
      }
      case OpKind::BUDGET: {
        model_router::BudgetSnapshot budget;
        budget.budget_id = model_router::BudgetId(1);
        budget.generation = fixture_.router->summary().budget_generation.next();
        budget.verdict = rng_.chance(2, 3) ? model_router::BudgetVerdict::ALLOWED
                                           : model_router::BudgetVerdict::DENIED;
        budget.unit = model_router::reference::reference_cost_unit();
        budget.observed_at_unix_millis = now();
        const model_router::MutationResult result =
            fixture_.router->set_budget(std::move(budget));
        return "code=" + std::string(model_router::to_string(result.code)) +
               " budget_generation=" +
               std::to_string(fixture_.router->summary().budget_generation.value());
      }
      case OpKind::SLO: {
        model_router::SloEvidence slo;
        slo.slo_id = model_router::SLOId(1);
        slo.generation = fixture_.router->summary().slo_generation.next();
        slo.verdict = rng_.chance(2, 3) ? model_router::SloVerdict::FEASIBLE
                                        : model_router::SloVerdict::INFEASIBLE;
        slo.latency_target_micros = static_cast<std::uint32_t>(1000 + rng_.below(9000));
        slo.observed_at_unix_millis = now();
        const model_router::MutationResult result = fixture_.router->set_slo(std::move(slo));
        return "code=" + std::string(model_router::to_string(result.code)) +
               " slo_generation=" +
               std::to_string(fixture_.router->summary().slo_generation.value());
      }
      case OpKind::TRUST: {
        const TrackedBackend* backend = pick_backend();
        if (backend == nullptr) {
          return "skipped=no_backend";
        }
        model_router::TrustEvidence evidence;
        evidence.profile_id = model_router::TrustProfileId(1);
        evidence.generation = model_router::TrustGeneration(1 + rng_.below(4));
        evidence.backend_id = backend->id;
        evidence.domain = rng_.chance(1, 2) ? model_router::TrustDomain::LOCAL
                                            : model_router::TrustDomain::PRIVATE_NETWORK;
        evidence.observed_at_unix_millis = now();
        const model_router::MutationResult result = fixture_.router->set_trust(std::move(evidence));
        return "code=" + std::string(model_router::to_string(result.code)) +
               " backend=" + std::to_string(backend->id.value());
      }
      case OpKind::COMPATIBILITY: {
        const TrackedBackend* backend = pick_backend();
        if (backend == nullptr) {
          return "skipped=no_backend";
        }
        model_router::CompatibilityEvidence evidence;
        evidence.profile_id = model_router::CompatibilityProfileId(backend->id.value() * 100 + 3);
        evidence.generation = model_router::CompatibilityGeneration(1 + rng_.below(4));
        evidence.backend_id = backend->id;
        evidence.verdict = model_router::CompatibilityVerdict::COMPATIBLE;
        evidence.protocol = "model_router.reference";
        evidence.protocol_version = 1;
        evidence.streaming = true;
        evidence.tool_calling = true;
        evidence.max_context_tokens = 131072;
        evidence.observed_at_unix_millis = now();
        const model_router::MutationResult result =
            fixture_.router->set_compatibility(std::move(evidence));
        return "code=" + std::string(model_router::to_string(result.code)) +
               " backend=" + std::to_string(backend->id.value());
      }
      case OpKind::DISCOVERY:
      case OpKind::ROUTE: {
        const RouteOutcome outcome =
            fixture_.router->route(make_request(rng_.chance(4, 5) ? kTenant : kOtherTenant));
        if (outcome.has_decision) {
          decisions_.push_back(TrackedDecision{outcome.decision.decision_id,
                                               outcome.decision.authority.tenant});
          verify_decision(outcome);
        }
        return describe_route(outcome);
      }
      case OpKind::REVALIDATE: {
        const TrackedDecision* decision = pick_decision();
        if (decision == nullptr) {
          return "skipped=no_decision";
        }
        const model_router::MutationResult result =
            fixture_.router->revalidate(decision->id, decision->tenant);
        return "code=" + std::string(model_router::to_string(result.code));
      }
      case OpKind::DISPATCH: {
        const TrackedDecision* decision = pick_decision();
        if (decision == nullptr) {
          return "skipped=no_decision";
        }
        const model_router::DispatchRecord record =
            fixture_.router->dispatch(decision->id, decision->tenant);
        return "code=" + std::string(model_router::to_string(record.code)) +
               " handed_off=" + std::string(record.handed_off ? "1" : "0") +
               " target=" + record.target.to_string();
      }
      case OpKind::REROUTE: {
        const TrackedDecision* decision = pick_decision();
        if (decision == nullptr) {
          return "skipped=no_decision";
        }
        const RouteOutcome outcome = fixture_.router->reroute(
            decision->id, model_router::FailureClass::TRANSIENT_BACKEND_FAILURE,
            make_request(decision->tenant));
        if (outcome.has_decision) {
          decisions_.push_back(TrackedDecision{outcome.decision.decision_id,
                                               outcome.decision.authority.tenant});
          verify_decision(outcome);
        }
        return describe_route(outcome);
      }
      case OpKind::BACKEND_DEATH: {
        const TrackedBackend* backend = pick_backend();
        if (backend == nullptr) {
          return "skipped=no_backend";
        }
        const BackendId id = backend->id;
        const model_router::MutationResult result =
            fixture_.router->unregister_backend(id, backend->generation);
        if (result.accepted()) {
          backends_.erase(std::remove_if(backends_.begin(), backends_.end(),
                                         [id](const TrackedBackend& entry) { return entry.id == id; }),
                          backends_.end());
        }
        return "code=" + std::string(model_router::to_string(result.code)) +
               " backend=" + std::to_string(id.value());
      }
      case OpKind::FENCE: {
        const TrackedBackend* backend = pick_backend();
        if (backend == nullptr) {
          return "skipped=no_backend";
        }
        const model_router::MutationResult result = fixture_.router->fence_backend_boot(
            backend->id, backend->generation, backend->boot, OutcomeCode::FENCED);
        return "code=" + std::string(model_router::to_string(result.code)) +
               " backend=" + std::to_string(backend->id.value()) +
               " boot=" + std::to_string(backend->boot.value());
      }
      case OpKind::RETIRE: {
        const std::uint64_t index = rng_.below(3);
        const model_router::MutationResult result =
            fixture_.router->retire_model(model_id(index), model_generations_[index]);
        return "code=" + std::string(model_router::to_string(result.code)) +
               " model=" + std::to_string(model_id(index).value());
      }
      case OpKind::SAVE: {
        const model_router::PersistenceResult result = fixture_.router->save(state_path_);
        if (result.ok()) {
          saved_ = true;
        }
        return "code=" + std::string(model_router::to_string(result.code));
      }
      case OpKind::LOAD: {
        if (!saved_) {
          return "skipped=nothing_saved";
        }
        const model_router::PersistenceResult result = fixture_.router->load(state_path_);
        if (result.ok()) {
          rebuild_backends_from_state();
        }
        return "code=" + std::string(model_router::to_string(result.code));
      }
      case OpKind::START: {
        const model_router::MutationResult result = fixture_.router->start();
        return "code=" + std::string(model_router::to_string(result.code)) +
               " epoch=" + std::to_string(fixture_.router->router_epoch().value());
      }
      case OpKind::SHUTDOWN: {
        const model_router::MutationResult result = fixture_.router->shutdown();
        return "code=" + std::string(model_router::to_string(result.code)) +
               " running=" + std::string(fixture_.router->running() ? "1" : "0");
      }
      default:
        return "invalid";
    }
  }

  /// Property checks applied to every committed decision.
  void verify_decision(const RouteOutcome& outcome) {
    if (!outcome.has_decision) {
      return;
    }
    const model_router::RouteDecision& decision = outcome.decision;
    if (!(decision.authority == decision.explanation.authority)) {
      ::mrtest::fail(__FILE__, __LINE__,
                     "seed=0x" + hex(seed_) + ": decision authority differs from its explanation");
    }
    // No duplicate candidate identity in the ranked output.
    std::vector<model_router::CandidateKey> keys;
    for (const model_router::RankedCandidate& ranked : outcome.route.ranking) {
      keys.push_back(ranked.key);
    }
    std::vector<model_router::CandidateKey> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
      ::mrtest::fail(__FILE__, __LINE__,
                     "seed=0x" + hex(seed_) + ": duplicate candidate identity in ranked output");
    }
    // Every decision binds the current generations of the identity it names.
    model_router::ModelDescriptor model;
    model_router::BackendDescriptor backend;
    if (!fixture_.router->find_model(decision.authority.model_id, &model) ||
        !fixture_.router->find_backend(decision.authority.backend_id, &backend)) {
      ::mrtest::fail(__FILE__, __LINE__,
                     "seed=0x" + hex(seed_) + ": decision binds an identity that does not exist");
    }
    if (!(model.model_generation == decision.authority.model_generation) ||
        !(model.artifact_generation == decision.authority.artifact_generation)) {
      ::mrtest::fail(__FILE__, __LINE__,
                     "seed=0x" + hex(seed_) + ": decision bound a stale model generation");
    }
    if (!(backend.backend_boot == decision.authority.backend_boot) ||
        !(backend.backend_generation == decision.authority.backend_generation)) {
      ::mrtest::fail(__FILE__, __LINE__,
                     "seed=0x" + hex(seed_) + ": decision bound a stale backend incarnation");
    }
  }

  std::uint64_t seed_;
  Rng rng_;
  std::string state_path_;
  mrtest::Fixture fixture_;
  std::vector<TrackedBackend> backends_;
  std::vector<TrackedDecision> decisions_;
  PolicyGeneration policy_generation_{};
  ModelGeneration model_generations_[3] = {ModelGeneration(1), ModelGeneration(1),
                                           ModelGeneration(1)};
  model_router::ArtifactGeneration artifact_generations_[3] = {
      model_router::ArtifactGeneration(1), model_router::ArtifactGeneration(1),
      model_router::ArtifactGeneration(1)};
  bool saved_{false};
};

[[nodiscard]] std::string state_path(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

void remove_state_file(const std::string& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path + ".tmp", ignored);
}

// A seeded sequence of every canonical operation kind, with a full invariant
// scan after each operation. Three independent seeds are replayed.
MR_TEST(property, seeded_sequences_preserve_invariants) {
  const std::string path = state_path("model_router_property_sequence.state");
  remove_state_file(path);
  for (const std::uint64_t seed : kSeeds) {
    Driver driver(seed, path);
    const std::vector<std::string> history = driver.run(kOperationsPerSeed);
    MR_CHECK_EQ(history.size(), static_cast<std::size_t>(kOperationsPerSeed));
    MR_CHECK(driver.router().check_invariants().ok());
  }
  remove_state_file(path);
}

// Determinism: the same seed against a fresh router reproduces the identical
// operation history and the identical routing outcome for every operation.
MR_TEST(property, identical_state_gives_identical_history) {
  const std::string path = state_path("model_router_property_determinism.state");
  remove_state_file(path);
  const std::uint64_t seed = kSeeds[0];
  Driver first(seed, path);
  const std::vector<std::string> first_history = first.run(kOperationsPerSeed);
  Driver second(seed, path);
  const std::vector<std::string> second_history = second.run(kOperationsPerSeed);
  MR_CHECK_EQ(first_history.size(), second_history.size());
  for (std::size_t index = 0; index < first_history.size(); ++index) {
    if (first_history[index] != second_history[index]) {
      ::mrtest::fail(__FILE__, __LINE__,
                     "seed=0x9e3779b97f4a7c15 diverged at operation " + std::to_string(index) +
                         "\n  first : " + first_history[index] + "\n  second: " +
                         second_history[index]);
    }
  }
  remove_state_file(path);
}

// A hard-invalid candidate never wins: a context limit no reference model can
// satisfy leaves the router with no eligible candidate at all.
MR_TEST(property, hard_invalid_candidates_never_win) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);
  fixture.add_backend(BackendId(2), 1);
  fixture.publish_costs(BackendId(1), PriceGeneration(1), 1, 1, 1);

  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.min_context_tokens = 1u << 20;
  request.requirements.require_known_cost = true;
  const RouteOutcome outcome = fixture.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::NO_ELIGIBLE_CANDIDATE);
  MR_CHECK(!outcome.has_decision);
  MR_CHECK(outcome.route.ranking.empty());
  for (const model_router::RouteRejection& rejection : outcome.route.rejections) {
    MR_CHECK(rejection.code != OutcomeCode::ACCEPTED);
  }
  MR_CHECK(fixture.router->check_invariants().ok());
}

// UNKNOWN evidence never satisfies a hard requirement.
MR_TEST(property, unknown_never_satisfies_a_hard_requirement) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  // Register without publishing any dynamic evidence.
  model_router::BackendDescriptor backend = model_router::reference::make_backend(
      fixture.catalog, BackendId(1), BackendGeneration(1), BackendBootId(1),
      model_router::BackendRegistrationGeneration(1), "127.0.0.1:7001",
      model_router::Provenance::SYNTHETIC, fixture.now());
  MR_CHECK(fixture.router->register_backend(std::move(backend)).accepted());

  const RouteOutcome outcome =
      fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
  MR_CHECK_EQ(outcome.code, OutcomeCode::NO_ELIGIBLE_CANDIDATE);
  MR_CHECK(!outcome.has_decision);
  MR_CHECK(!outcome.route.rejections.empty());
  for (const model_router::RouteRejection& rejection : outcome.route.rejections) {
    MR_CHECK(rejection.code == OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
  }
  // Publishing only health is still not enough: availability and readiness stay
  // UNKNOWN and keep the candidate ineligible.
  MR_CHECK(fixture.publisher
               .health(*fixture.router, BackendId(1), BackendBootId(1),
                       model_router::HealthState::HEALTHY, fixture.now())
               .accepted());
  const RouteOutcome partial =
      fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
  MR_CHECK_EQ(partial.code, OutcomeCode::NO_ELIGIBLE_CANDIDATE);
  MR_CHECK(fixture.router->check_invariants().ok());
}

/// Discovery adapter that emits the canonical candidate set in reverse order
/// and duplicates one entry, to prove that ranking depends only on canonical
/// state and never on discovery order or duplicates.
class ReversingCandidateProvider final : public model_router::CandidateProvider {
 public:
  [[nodiscard]] std::string name() const override { return "reversing-provider"; }

  void attach(model_router::ModelRouter* router) { router_ = router; }

  [[nodiscard]] model_router::EvidenceResult<std::vector<model_router::RouteCandidate>> discover(
      const model_router::RouteRequest&, const model_router::DiscoveryContext&) override {
    std::vector<model_router::RouteCandidate> candidates;
    if (router_ == nullptr) {
      return model_router::EvidenceResult<std::vector<model_router::RouteCandidate>>::accepted(
          std::move(candidates));
    }
    const model_router::RouterSnapshot snapshot = router_->snapshot();
    for (const model_router::BackendSummary& summary : snapshot.backends) {
      model_router::BackendDescriptor backend;
      if (!router_->find_backend(summary.backend_id, &backend)) {
        continue;
      }
      for (const model_router::ModelBinding& binding : backend.model_bindings) {
        model_router::ModelDescriptor model;
        if (!router_->find_model(binding.model_id, &model)) {
          continue;
        }
        model_router::RouteCandidate candidate;
        candidate.key.model_id = model.model_id;
        candidate.key.model_generation = model.model_generation;
        candidate.key.artifact_generation = model.artifact_generation;
        candidate.key.backend_id = backend.backend_id;
        candidate.key.backend_generation = backend.backend_generation;
        candidate.key.backend_boot = backend.backend_boot;
        candidate.key.endpoint_id = backend.endpoint.endpoint_id;
        candidate.key.endpoint_generation = backend.endpoint.endpoint_generation;
        candidate.model = model;
        candidate.backend = backend;
        candidate.endpoint = backend.endpoint;
        candidate.context_limit_tokens =
            binding.context_limit_tokens != 0 ? binding.context_limit_tokens
                                              : model.context_limit_tokens;
        candidate.quality_class =
            binding.quality_class != model_router::kUnclassifiedQuality ? binding.quality_class
                                                                       : model.quality_class;
        candidate.discovery_source = "reversing-provider";
        candidates.push_back(std::move(candidate));
      }
    }
    std::reverse(candidates.begin(), candidates.end());
    if (!candidates.empty()) {
      candidates.push_back(candidates.front());
    }
    return model_router::EvidenceResult<std::vector<model_router::RouteCandidate>>::accepted(
        std::move(candidates));
  }

 private:
  model_router::ModelRouter* router_{nullptr};
};

/// Builds two routers with identical canonical state, one using built-in
/// catalog discovery and one using the reversing provider.
void build_twin_state(mrtest::Fixture& fixture, mrtest::Fixture& twin,
                      const std::shared_ptr<ReversingCandidateProvider>& provider) {
  fixture.start();
  model_router::ModelRouterOptions options;
  options.clock = twin.clock;
  options.providers.dispatcher = twin.dispatcher;
  options.providers.candidates = provider;
  twin.router = std::make_unique<model_router::ModelRouter>(std::move(options));
  MR_CHECK(twin.router->start().accepted());
  twin.register_catalog();
  provider->attach(twin.router.get());
  for (mrtest::Fixture* target : {&fixture, &twin}) {
    MR_CHECK(mrtest::install_open_policy(*target->router, kPolicy, kPolicyGeneration).accepted());
    target->add_backend(BackendId(1), 1);
    target->add_backend(BackendId(2), 1);
    target->publish_costs(BackendId(1), PriceGeneration(1), 100, 100, 100);
    target->publish_costs(BackendId(2), PriceGeneration(1), 900, 900, 900);
  }
}

// Identical canonical state produces an identical ranking, and discovery order
// is irrelevant: a provider that emits the same candidates reversed and
// duplicated must produce the same ranking as built-in discovery.
MR_TEST(property, identical_state_gives_identical_ranking) {
  mrtest::Fixture baseline;
  mrtest::Fixture twin;
  const std::shared_ptr<ReversingCandidateProvider> provider =
      std::make_shared<ReversingCandidateProvider>();
  build_twin_state(baseline, twin, provider);

  model_router::RouteRequest request =
      mrtest::make_request(baseline, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  request.requirements.require_known_cost = true;

  const RouteOutcome expected = baseline.router->route(request);
  const RouteOutcome observed = twin.router->route(request);
  MR_CHECK_EQ(expected.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(observed.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(observed.decision.explanation.winner, expected.decision.explanation.winner);
  MR_CHECK_EQ(observed.route.ranking.size(), expected.route.ranking.size());
  for (std::size_t index = 0; index < expected.route.ranking.size(); ++index) {
    MR_CHECK_EQ(observed.route.ranking[index].key, expected.route.ranking[index].key);
    MR_CHECK_EQ(observed.route.ranking[index].score, expected.route.ranking[index].score);
    MR_CHECK_EQ(observed.route.ranking[index].rank, expected.route.ranking[index].rank);
  }
  // The duplicated candidate was collapsed, so the ranked output carries no
  // duplicate identity.
  std::vector<model_router::CandidateKey> keys;
  for (const model_router::RankedCandidate& ranked : observed.route.ranking) {
    keys.push_back(ranked.key);
  }
  std::sort(keys.begin(), keys.end());
  MR_CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
  MR_CHECK(baseline.router->check_invariants().ok());
  MR_CHECK(twin.router->check_invariants().ok());
}

// A stale route never dispatches.
MR_TEST(property, stale_routes_never_dispatch) {
  const struct {
    const char* name;
    std::function<void(mrtest::Fixture&)> mutate;
    OutcomeCode expected;
  } cases[] = {
      {"policy",
       [](mrtest::Fixture& fixture) {
         model_router::PolicySnapshot policy =
             model_router::PolicyBuilder(kPolicy, PolicyGeneration(2))
                 .fallback_policy(model_router::FallbackPolicy::PERMITTED)
                 .max_fallback_depth(4)
                 .build();
         MR_CHECK(fixture.router->set_policy(std::move(policy)).accepted());
       },
       OutcomeCode::REJECT_STALE_POLICY},
      {"price",
       [](mrtest::Fixture& fixture) {
         for (const model_router::ModelId model_id :
              {model_router::ModelId(1), model_router::ModelId(2), model_router::ModelId(3)}) {
           MR_CHECK(fixture.router
                        ->set_cost(model_router::reference::make_cost_evidence(
                            BackendId(1), model_id, PriceGeneration(2), 42, 42, 42, fixture.now()))
                        .accepted());
         }
       },
       OutcomeCode::REJECT_STALE_PRICE},
      {"health",
       [](mrtest::Fixture& fixture) {
         MR_CHECK(fixture.publisher
                      .health(*fixture.router, BackendId(1), BackendBootId(1),
                              model_router::HealthState::DEGRADED, fixture.now())
                      .accepted());
       },
       OutcomeCode::REJECT_STALE_HEALTH},
      {"readiness",
       [](mrtest::Fixture& fixture) {
         MR_CHECK(fixture.publisher
                      .readiness(*fixture.router, BackendId(1), BackendBootId(1),
                                 model_router::ReadinessState::WARMING, fixture.now())
                      .accepted());
       },
       OutcomeCode::REJECT_STALE_READINESS}};

  for (const auto& item : cases) {
    mrtest::Fixture fixture;
    fixture.start();
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    fixture.add_backend(BackendId(1), 1);
    fixture.add_backend(BackendId(2), 1);
    fixture.publish_costs(BackendId(1), PriceGeneration(1), 100, 100, 100);
    fixture.publish_costs(BackendId(2), PriceGeneration(1), 900, 900, 900);
    const RouteOutcome outcome =
        fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
    MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
    MR_CHECK_EQ(outcome.decision.authority.backend_id, BackendId(1));
    MR_CHECK_EQ(fixture.router->dispatch(outcome.decision.decision_id, kTenant).code,
                OutcomeCode::DISPATCHED);
    // The same decision is now terminal; take a second decision and stale it.
    const RouteOutcome second =
        fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
    MR_CHECK_EQ(second.code, OutcomeCode::ROUTED);
    item.mutate(fixture);
    const model_router::MutationResult revalidated =
        fixture.router->revalidate(second.decision.decision_id, kTenant);
    MR_CHECK_EQ(revalidated.code, item.expected);
    const model_router::DispatchRecord dispatched =
        fixture.router->dispatch(second.decision.decision_id, kTenant);
    MR_CHECK(!dispatched.handed_off);
    MR_CHECK_EQ(dispatched.code, item.expected);
    MR_CHECK(fixture.router->check_invariants().ok());
  }
}

// A fenced incarnation can never regain authority.
MR_TEST(property, fenced_boots_never_regain_authority) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);
  MR_CHECK(fixture.router
               ->fence_backend_boot(BackendId(1), BackendGeneration(1), BackendBootId(1),
                                    OutcomeCode::FENCED)
               .accepted());
  const std::vector<model_router::FencedBootRecord> fenced = fixture.router->fenced_boots();
  MR_CHECK_EQ(fenced.size(), std::size_t{1});
  MR_CHECK_EQ(fenced.front().backend_id, BackendId(1));
  MR_CHECK_EQ(fenced.front().backend_boot, BackendBootId(1));
  MR_CHECK_EQ(fenced.front().backend_generation, BackendGeneration(1));
  MR_CHECK_NE(fenced.front().fenced_at_unix_millis, model_router::UnixMillis{0});

  // Re-registering the fenced incarnation is refused, at any registration
  // generation, and no evidence for it is ever accepted again.
  for (const std::uint64_t registration : {2u, 3u, 100u}) {
    model_router::BackendDescriptor fenced_backend = model_router::reference::make_backend(
        fixture.catalog, BackendId(1), BackendGeneration(1), BackendBootId(1),
        model_router::BackendRegistrationGeneration(registration), "127.0.0.1:7001",
        model_router::Provenance::SYNTHETIC, fixture.now());
    MR_CHECK_EQ(fixture.router->register_backend(std::move(fenced_backend)).code,
                OutcomeCode::REJECT_STALE_BACKEND_BOOT);
  }
  MR_CHECK_EQ(fixture.publisher
                  .health(*fixture.router, BackendId(1), BackendBootId(1),
                          model_router::HealthState::HEALTHY, fixture.now())
                  .code,
              OutcomeCode::REJECT_STALE_BACKEND_BOOT);
  // A fresh incarnation of the same backend is legal and starts from UNKNOWN.
  fixture.add_backend(BackendId(1), 2, BackendGeneration(1), 2);
  model_router::BackendDescriptor recovered;
  MR_CHECK(fixture.router->find_backend(BackendId(1), &recovered));
  MR_CHECK_EQ(recovered.backend_boot, BackendBootId(2));
  MR_CHECK(fixture.router->check_invariants().ok());
}

// Policy-denied and budget-denied candidates never dispatch.
MR_TEST(property, policy_and_budget_denied_models_never_dispatch) {
  {
    mrtest::Fixture fixture;
    fixture.start();
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    fixture.add_backend(BackendId(1), 1);
    const RouteOutcome outcome =
        fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
    MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
    const model_router::ModelId denied = outcome.decision.authority.model_id;
    model_router::PolicySnapshot policy =
        model_router::PolicyBuilder(kPolicy, PolicyGeneration(2)).deny_model(denied).build();
    MR_CHECK(fixture.router->set_policy(std::move(policy)).accepted());
    const model_router::DispatchRecord dispatched =
        fixture.router->dispatch(outcome.decision.decision_id, kTenant);
    MR_CHECK(!dispatched.handed_off);
    MR_CHECK_EQ(dispatched.code, OutcomeCode::REJECT_STALE_POLICY);
    MR_CHECK_EQ(fixture.dispatcher->calls, std::uint64_t{0});
    MR_CHECK(fixture.router->check_invariants().ok());
  }
  {
    mrtest::Fixture fixture;
    fixture.start();
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    fixture.add_backend(BackendId(1), 1);
    model_router::BudgetSnapshot allowed;
    allowed.budget_id = model_router::BudgetId(1);
    allowed.generation = model_router::BudgetGeneration(1);
    allowed.verdict = model_router::BudgetVerdict::ALLOWED;
    allowed.unit = model_router::reference::reference_cost_unit();
    MR_CHECK(fixture.router->set_budget(std::move(allowed)).accepted());
    model_router::RouteRequest request =
        mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
    request.budget_id = model_router::BudgetId(1);
    request.budget_generation = model_router::BudgetGeneration(1);
    const RouteOutcome outcome = fixture.router->route(request);
    MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
    model_router::BudgetSnapshot denied;
    denied.budget_id = model_router::BudgetId(1);
    denied.generation = model_router::BudgetGeneration(2);
    denied.verdict = model_router::BudgetVerdict::DENIED;
    denied.unit = model_router::reference::reference_cost_unit();
    MR_CHECK(fixture.router->set_budget(std::move(denied)).accepted());
    const model_router::DispatchRecord dispatched =
        fixture.router->dispatch(outcome.decision.decision_id, kTenant);
    MR_CHECK(!dispatched.handed_off);
    MR_CHECK_EQ(dispatched.code, OutcomeCode::REJECT_STALE_BUDGET);
    MR_CHECK_EQ(fixture.dispatcher->calls, std::uint64_t{0});
    MR_CHECK(fixture.router->check_invariants().ok());
  }
}

// Tenant isolation: a decision made for one tenant can never be revalidated or
// dispatched by another, and a tenant-bound policy rejects foreign requests.
MR_TEST(property, tenant_isolation_holds) {
  mrtest::Fixture fixture;
  fixture.start();
  model_router::PolicySnapshot policy =
      model_router::PolicyBuilder(kPolicy, kPolicyGeneration).bind_tenant(kTenant).build();
  MR_CHECK(fixture.router->set_policy(std::move(policy)).accepted());
  fixture.add_backend(BackendId(1), 1);

  const RouteOutcome outcome =
      fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.tenant, kTenant);

  const model_router::MutationResult foreign_revalidation =
      fixture.router->revalidate(outcome.decision.decision_id, kOtherTenant);
  MR_CHECK_EQ(foreign_revalidation.code, OutcomeCode::REJECT_CONFLICT);
  const model_router::DispatchRecord foreign_dispatch =
      fixture.router->dispatch(outcome.decision.decision_id, kOtherTenant);
  MR_CHECK(!foreign_dispatch.handed_off);
  MR_CHECK_EQ(foreign_dispatch.code, OutcomeCode::REJECT_CONFLICT);
  MR_CHECK_EQ(fixture.dispatcher->calls, std::uint64_t{0});

  const RouteOutcome foreign_request =
      fixture.router->route(mrtest::make_request(fixture, kOtherTenant, kPolicy, kPolicyGeneration));
  MR_CHECK_EQ(foreign_request.code, OutcomeCode::REJECT_CONFLICT);
  MR_CHECK(!foreign_request.has_decision);
  MR_CHECK(fixture.router->check_invariants().ok());
}

// Serialization round-trips deterministically and never carries dynamic
// evidence; recovery never revives dynamic authority.
MR_TEST(property, serialization_round_trips_deterministically) {
  const std::string first_path = state_path("model_router_property_roundtrip_a.state");
  const std::string second_path = state_path("model_router_property_roundtrip_b.state");
  remove_state_file(first_path);
  remove_state_file(second_path);

  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);
  fixture.add_backend(BackendId(2), 1);
  fixture.publish_costs(BackendId(1), PriceGeneration(1), 100, 100, 100);
  fixture.publish_costs(BackendId(2), PriceGeneration(1), 200, 200, 200);
  const RouteOutcome outcome =
      fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK(fixture.router->save(first_path).ok());
  MR_CHECK(fixture.router->save(second_path).ok());

  const auto read_all = [](const std::string& path) {
    std::FILE* file = nullptr;
#if defined(_WIN32)
    if (fopen_s(&file, path.c_str(), "rb") != 0) {
      file = nullptr;
    }
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (file == nullptr) {
      return std::vector<std::uint8_t>{};
    }
    std::vector<std::uint8_t> bytes;
    std::uint8_t buffer[4096];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
      bytes.insert(bytes.end(), buffer, buffer + read);
    }
    std::fclose(file);
    return bytes;
  };
  const std::vector<std::uint8_t> first_bytes = read_all(first_path);
  const std::vector<std::uint8_t> second_bytes = read_all(second_path);
  MR_CHECK(!first_bytes.empty());
  MR_CHECK(first_bytes == second_bytes);

  // Reloading the same file twice must produce the same semantic state.
  const model_router::ResourceLimits limits = model_router::default_resource_limits();
  const model_router::PersistenceLoad first_load =
      model_router::RouterStateStore::load(first_path, limits);
  const model_router::PersistenceLoad second_load =
      model_router::RouterStateStore::load(second_path, limits);
  MR_CHECK(first_load.ok());
  MR_CHECK(second_load.ok());
  MR_CHECK_EQ(first_load.state.semantic_digest(), second_load.state.semantic_digest());
  MR_CHECK_EQ(first_load.state.models.size(), second_load.state.models.size());
  MR_CHECK_EQ(first_load.state.backends.size(), second_load.state.backends.size());
  MR_CHECK_EQ(first_load.state.fenced_boots.size(), second_load.state.fenced_boots.size());
  MR_CHECK_EQ(first_load.state.routes.size(), second_load.state.routes.size());

  remove_state_file(first_path);
  remove_state_file(second_path);
}

// Recovery restores registration identity only: dynamic evidence stays UNKNOWN,
// old authority is never executable, and epochs advance.
MR_TEST(property, recovery_never_revives_dynamic_authority) {
  const std::string path = state_path("model_router_property_recovery.state");
  remove_state_file(path);
  model_router::RouteDecisionId decision_id{};
  model_router::RouterEpoch epoch{};
  {
    mrtest::Fixture fixture;
    fixture.start();
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    fixture.add_backend(BackendId(1), 1);
    MR_CHECK(fixture.router
                 ->fence_backend_boot(BackendId(2), BackendGeneration(1), BackendBootId(1),
                                      OutcomeCode::FENCED)
                 .accepted());
    const RouteOutcome outcome =
        fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
    MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
    decision_id = outcome.decision.decision_id;
    epoch = fixture.router->router_epoch();
    MR_CHECK(fixture.router->save(path).ok());
  }

  // Recovery is only legal while the router is not running, so the restored
  // router is constructed without start().
  mrtest::Fixture restored;
  model_router::ModelRouterOptions options;
  options.clock = restored.clock;
  restored.router = std::make_unique<model_router::ModelRouter>(std::move(options));
  MR_CHECK(restored.router->load(path).ok());
  MR_CHECK(restored.router->start().accepted());
  const model_router::RouterSummary summary = restored.router->summary();
  MR_CHECK(summary.recovered);
  MR_CHECK(summary.router_epoch > epoch);
  MR_CHECK(summary.coordinator_epoch.valid());
  MR_CHECK_EQ(summary.backend_count, std::uint32_t{1});
  MR_CHECK_EQ(summary.fenced_boot_count, std::uint32_t{1});

  model_router::BackendDescriptor backend;
  MR_CHECK(restored.router->find_backend(BackendId(1), &backend));
  MR_CHECK_EQ(backend.health.state, model_router::HealthState::UNKNOWN);
  MR_CHECK_EQ(backend.availability.state, model_router::AvailabilityState::UNKNOWN);
  MR_CHECK_EQ(backend.readiness.state, model_router::ReadinessState::UNKNOWN);
  MR_CHECK(!backend.health.generation.valid());
  MR_CHECK(!backend.availability.generation.valid());
  MR_CHECK(!backend.readiness.generation.valid());

  // The recovered decision is retained history, never executable authority.
  model_router::RouteDecision recovered;
  MR_CHECK(restored.router->find_decision(decision_id, &recovered));
  MR_CHECK_EQ(recovered.status, model_router::RouteStatus::STALE);
  MR_CHECK_EQ(recovered.explanation.currentness, model_router::Currentness::RECONSTRUCTED);
  const model_router::MutationResult revalidated = restored.router->revalidate(decision_id, kTenant);
  MR_CHECK(!revalidated.accepted());
  const model_router::DispatchRecord dispatched = restored.router->dispatch(decision_id, kTenant);
  MR_CHECK(!dispatched.handed_off);
  MR_CHECK(restored.router->check_invariants().ok());

  // The fenced incarnation is still fenced after recovery.
  MR_CHECK_EQ(restored.router
                  ->fence_backend_boot(BackendId(2), BackendGeneration(1), BackendBootId(1),
                                       OutcomeCode::FENCED)
                  .code,
              OutcomeCode::NO_CHANGE);
  MR_CHECK(restored.router->check_invariants().ok());
  remove_state_file(path);
}

}  // namespace
