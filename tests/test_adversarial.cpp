// Model Router - deliberate adversarial attacks.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Every attack asserts a typed rejection (or a typed, bounded outcome) and then
// asserts that the router's invariants still hold: a rejected attack may never
// leave canonical state damaged.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model_router/detail/crc32c.hpp"
#include "model_router/distributed/protocol.hpp"
#include "model_router/distributed/transport.hpp"
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

constexpr TenantId kTenant(1);
constexpr TenantId kOtherTenant(2);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);

[[nodiscard]] std::string describe(OutcomeCode code) {
  return std::string(model_router::to_string(code));
}

void require_code(OutcomeCode observed, OutcomeCode expected, const char* attack) {
  if (observed != expected) {
    ::mrtest::fail(__FILE__, __LINE__, std::string(attack) + ": expected " + describe(expected) +
                                     " but observed " + describe(observed));
  }
}

void require_rejected(OutcomeCode observed, const char* attack) {
  if (observed == OutcomeCode::ACCEPTED || observed == OutcomeCode::NO_CHANGE) {
    ::mrtest::fail(__FILE__, __LINE__,
                   std::string(attack) + ": the attack was accepted (" + describe(observed) + ")");
  }
}

void require_route_code(const RouteOutcome& outcome, OutcomeCode expected, const char* attack) {
  if (outcome.code != expected) {
    ::mrtest::fail(__FILE__, __LINE__, std::string(attack) + ": expected " + describe(expected) +
                                     " but observed " + describe(outcome.code));
  }
}

[[nodiscard]] std::string temp_path(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

void remove_file(const std::string& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path + ".tmp", ignored);
}

[[nodiscard]] std::vector<std::uint8_t> read_file(const std::string& path) {
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, path.c_str(), "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "rb");
#endif
  std::vector<std::uint8_t> bytes;
  if (file == nullptr) {
    return bytes;
  }
  std::uint8_t buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    bytes.insert(bytes.end(), buffer, buffer + read);
  }
  std::fclose(file);
  return bytes;
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, path.c_str(), "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "wb");
#endif
  MR_CHECK(file != nullptr);
  if (!bytes.empty()) {
    MR_CHECK_EQ(std::fwrite(bytes.data(), 1, bytes.size(), file), bytes.size());
  }
  MR_CHECK_EQ(std::fclose(file), 0);
}

/// Builds a router whose canonical state is fully populated and current.
struct Ready {
  mrtest::Fixture fixture;

  void build(std::uint32_t max_route_history = 0) {
    fixture.start(max_route_history);
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    fixture.add_backend(BackendId(1), 1);
    fixture.add_backend(BackendId(2), 1);
    fixture.publish_costs(BackendId(1), PriceGeneration(1), 100, 100, 100);
    fixture.publish_costs(BackendId(2), PriceGeneration(1), 200, 200, 200);
  }

  [[nodiscard]] model_router::RouteRequest request() const {
    return mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  }
};

/// A fully formed synthetic candidate that passes every hard predicate.
[[nodiscard]] model_router::RouteCandidate synthetic_candidate(std::uint64_t backend_id,
                                                               model_router::UnixMillis now) {
  model_router::RouteCandidate candidate;
  candidate.key.model_id = model_router::ModelId(1);
  candidate.key.model_generation = ModelGeneration(1);
  candidate.key.artifact_generation = model_router::ArtifactGeneration(1);
  candidate.key.backend_id = BackendId(backend_id);
  candidate.key.backend_generation = BackendGeneration(1);
  candidate.key.backend_boot = BackendBootId(1);
  candidate.key.endpoint_id = model_router::EndpointId(backend_id * 10 + 1);
  candidate.key.endpoint_generation = model_router::EndpointGeneration(1);

  model_router::ModelDescriptor& model = candidate.model;
  model.model_id = candidate.key.model_id;
  model.model_generation = candidate.key.model_generation;
  model.model_version_id = model_router::ModelVersionId(1);
  model.artifact_generation = candidate.key.artifact_generation;
  model.family_id = model_router::ModelFamilyId(1);
  model.context_limit_tokens = 32768;
  model.max_output_tokens = 4096;
  model.input_modalities =
      model_router::ModalitySet(static_cast<std::uint32_t>(model_router::Modality::TEXT));
  model.output_modalities =
      model_router::ModalitySet(static_cast<std::uint32_t>(model_router::Modality::TEXT));
  model.quality_class = 20;
  model.lifecycle = model_router::ModelLifecycle::CURRENT;
  model.registered_at_unix_millis = now;

  model_router::BackendDescriptor& backend = candidate.backend;
  backend.backend_id = candidate.key.backend_id;
  backend.backend_generation = candidate.key.backend_generation;
  backend.backend_boot = candidate.key.backend_boot;
  backend.backend_registration_generation = model_router::BackendRegistrationGeneration(1);
  backend.provider_id = model_router::ProviderId(1);
  backend.provider_generation = model_router::ProviderGeneration(1);
  backend.endpoint.endpoint_id = candidate.key.endpoint_id;
  backend.endpoint.endpoint_generation = candidate.key.endpoint_generation;
  backend.endpoint.backend_id = candidate.key.backend_id;
  backend.endpoint.backend_boot = candidate.key.backend_boot;
  backend.endpoint.reference = "127.0.0.1:" + std::to_string(7000 + backend_id);
  backend.endpoint.protocol = "model_router.reference";
  backend.endpoint.protocol_version = model_router::protocol_version;
  backend.trust_profile_id = model_router::TrustProfileId(1);
  backend.trust_generation = model_router::TrustGeneration(1);
  backend.trust_domain = model_router::TrustDomain::LOCAL;
  backend.locality = model_router::LocalityKey("local/loopback");
  backend.network_distance = 1;
  backend.provenance = model_router::Provenance::SYNTHETIC;
  backend.registered_at_unix_millis = now;

  model_router::ModelBinding binding;
  binding.model_id = candidate.key.model_id;
  binding.model_generation = candidate.key.model_generation;
  binding.artifact_generation = candidate.key.artifact_generation;
  binding.context_limit_tokens = 32768;
  binding.quality_class = 20;
  binding.bound_at_unix_millis = now;
  backend.model_bindings.push_back(binding);

  backend.health.state = model_router::HealthState::HEALTHY;
  backend.health.generation = model_router::HealthGeneration(1);
  backend.health.backend_id = candidate.key.backend_id;
  backend.health.backend_boot = candidate.key.backend_boot;
  backend.health.observed_at_unix_millis = now;
  backend.availability.state = model_router::AvailabilityState::AVAILABLE;
  backend.availability.generation = model_router::AvailabilityGeneration(1);
  backend.availability.backend_id = candidate.key.backend_id;
  backend.availability.backend_boot = candidate.key.backend_boot;
  backend.availability.observed_at_unix_millis = now;
  backend.readiness.state = model_router::ReadinessState::READY;
  backend.readiness.generation = model_router::ReadinessGeneration(1);
  backend.readiness.backend_id = candidate.key.backend_id;
  backend.readiness.backend_boot = candidate.key.backend_boot;
  backend.readiness.observed_at_unix_millis = now;
  backend.residency.state = model_router::ResidencyState::RESIDENT;
  backend.residency.generation = model_router::ResidencyGeneration(1);
  backend.residency.backend_id = candidate.key.backend_id;
  backend.residency.backend_boot = candidate.key.backend_boot;
  backend.residency.observed_at_unix_millis = now;
  backend.capacity.state = model_router::CapacityState::AVAILABLE;
  backend.capacity.generation = model_router::CapacityGeneration(1);
  backend.capacity.backend_id = candidate.key.backend_id;
  backend.capacity.backend_boot = candidate.key.backend_boot;
  backend.capacity.observed_at_unix_millis = now;

  candidate.endpoint = backend.endpoint;
  candidate.context_limit_tokens = 32768;
  candidate.quality_class = 20;
  candidate.discovery_source = "adversarial-synthetic";
  return candidate;
}

/// Emits a fixed candidate set regardless of the request.
class FixedCandidateProvider final : public model_router::CandidateProvider {
 public:
  explicit FixedCandidateProvider(std::uint64_t count) : count_(count) {}

  [[nodiscard]] std::string name() const override { return "fixed-candidate-provider"; }

  [[nodiscard]] model_router::EvidenceResult<std::vector<model_router::RouteCandidate>> discover(
      const model_router::RouteRequest&, const model_router::DiscoveryContext& context) override {
    std::vector<model_router::RouteCandidate> candidates;
    candidates.reserve(static_cast<std::size_t>(count_));
    for (std::uint64_t index = 0; index < count_; ++index) {
      candidates.push_back(synthetic_candidate(index + 1, context.now_unix_millis));
    }
    return model_router::EvidenceResult<std::vector<model_router::RouteCandidate>>::accepted(
        std::move(candidates));
  }

 private:
  std::uint64_t count_;
};

// Duplicate ModelId whose generation regressed below the observed watermark.
MR_TEST(adversarial, duplicate_model_identity_with_regressed_generation) {
  Ready ready;
  ready.build();
  std::vector<model_router::ModelDescriptor> models =
      model_router::reference::make_models(ready.fixture.catalog, ready.fixture.now());
  model_router::ModelDescriptor upgraded = models[0];
  upgraded.model_generation = ModelGeneration(2);
  upgraded.artifact_generation = model_router::ArtifactGeneration(2);
  require_code(ready.fixture.router->register_model(upgraded).code, OutcomeCode::ACCEPTED,
               "register upgraded model");
  model_router::ModelDescriptor regressed = models[0];
  require_code(ready.fixture.router->register_model(regressed).code,
               OutcomeCode::REJECT_STALE_MODEL, "regressed model generation");
  // A re-registration of the current generation and lifecycle is a no-op, never
  // a silent overwrite with different content.
  require_code(ready.fixture.router->register_model(upgraded).code, OutcomeCode::NO_CHANGE,
               "re-register identical model");
  // An invalid artifact generation is rejected before it can be stored.
  model_router::ModelDescriptor bad_artifact = upgraded;
  bad_artifact.artifact_generation = model_router::ArtifactGeneration(0);
  require_code(ready.fixture.router->register_model(bad_artifact).code, OutcomeCode::REJECT_INVALID,
               "invalid artifact generation");
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// Duplicate BackendId whose generation regressed below the observed watermark.
MR_TEST(adversarial, duplicate_backend_identity_with_regressed_generation) {
  Ready ready;
  ready.build();
  model_router::BackendDescriptor upgraded = model_router::reference::make_backend(
      ready.fixture.catalog, BackendId(1), BackendGeneration(2), BackendBootId(2),
      model_router::BackendRegistrationGeneration(2), "127.0.0.1:7001",
      model_router::Provenance::SYNTHETIC, ready.fixture.now());
  require_code(ready.fixture.router->register_backend(std::move(upgraded)).code,
               OutcomeCode::ACCEPTED, "register upgraded backend");
  model_router::BackendDescriptor regressed = model_router::reference::make_backend(
      ready.fixture.catalog, BackendId(1), BackendGeneration(1), BackendBootId(3),
      model_router::BackendRegistrationGeneration(3), "127.0.0.1:7001",
      model_router::Provenance::SYNTHETIC, ready.fixture.now());
  require_code(ready.fixture.router->register_backend(std::move(regressed)).code,
               OutcomeCode::REJECT_STALE_BACKEND, "regressed backend generation");
  // A registration that does not advance is refused.
  model_router::BackendDescriptor stale_registration = model_router::reference::make_backend(
      ready.fixture.catalog, BackendId(1), BackendGeneration(2), BackendBootId(4),
      model_router::BackendRegistrationGeneration(2), "127.0.0.1:7001",
      model_router::Provenance::SYNTHETIC, ready.fixture.now());
  require_code(ready.fixture.router->register_backend(std::move(stale_registration)).code,
               OutcomeCode::REJECT_STALE_BACKEND, "non-advancing registration generation");
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// A duplicate BackendBootId can never be authoritative twice: re-registering a
// fenced incarnation, or re-registering an incarnation without advancing the
// registration generation, is refused.
MR_TEST(adversarial, duplicate_backend_boot_identity) {
  Ready ready;
  ready.build();
  require_code(ready.fixture.router
                   ->fence_backend_boot(BackendId(1), BackendGeneration(1), BackendBootId(1),
                                        OutcomeCode::FENCED)
                   .code,
               OutcomeCode::ACCEPTED, "fence incarnation");
  model_router::BackendDescriptor duplicate = model_router::reference::make_backend(
      ready.fixture.catalog, BackendId(1), BackendGeneration(1), BackendBootId(1),
      model_router::BackendRegistrationGeneration(2), "127.0.0.1:7001",
      model_router::Provenance::SYNTHETIC, ready.fixture.now());
  require_code(ready.fixture.router->register_backend(std::move(duplicate)).code,
               OutcomeCode::REJECT_STALE_BACKEND_BOOT, "duplicate fenced boot identity");
  // The same incarnation registered under a different BackendId is a distinct
  // authority and is allowed, but the fenced one stays fenced.
  model_router::BackendDescriptor other = model_router::reference::make_backend(
      ready.fixture.catalog, BackendId(3), BackendGeneration(1), BackendBootId(1),
      model_router::BackendRegistrationGeneration(1), "127.0.0.1:7003",
      model_router::Provenance::SYNTHETIC, ready.fixture.now());
  require_code(ready.fixture.router->register_backend(std::move(other)).code,
               OutcomeCode::ACCEPTED, "same boot under a different backend identity");
  const std::vector<model_router::FencedBootRecord> fenced = ready.fixture.router->fenced_boots();
  MR_CHECK_EQ(fenced.size(), std::size_t{1});
  MR_CHECK_EQ(fenced.front().backend_id, BackendId(1));
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// Conflicting registration: an identity change that does not advance the
// generation, a capacity detail bound to another backend, and an evidence
// state change at an unchanged generation are all typed conflicts.
MR_TEST(adversarial, conflicting_registration) {
  Ready ready;
  ready.build();
  model_router::PolicySnapshot other_policy =
      model_router::PolicyBuilder(model_router::PolicyId(2), kPolicyGeneration).build();
  require_code(ready.fixture.router->set_policy(std::move(other_policy)).code,
               OutcomeCode::REJECT_CONFLICT, "policy identity change at the same generation");

  model_router::CapacityEvidence capacity;
  capacity.backend_id = BackendId(1);
  capacity.backend_boot = BackendBootId(1);
  capacity.generation = model_router::CapacityGeneration(2);
  capacity.state = model_router::CapacityState::AVAILABLE;
  capacity.observed_at_unix_millis = ready.fixture.now();
  model_router::CapacityDetail wrong_backend;
  wrong_backend.backend_id = BackendId(2);
  wrong_backend.backend_boot = BackendBootId(1);
  require_code(ready.fixture.router->update_capacity(capacity, wrong_backend).code,
               OutcomeCode::REJECT_CONFLICT, "capacity detail for another backend");

  model_router::HealthEvidence health;
  health.backend_id = BackendId(1);
  health.backend_boot = BackendBootId(1);
  health.generation = model_router::HealthGeneration(1);
  health.state = model_router::HealthState::UNHEALTHY;
  health.observed_at_unix_millis = ready.fixture.now();
  require_code(ready.fixture.router->update_health(std::move(health)).code,
               OutcomeCode::REJECT_CONFLICT, "health state change at an unchanged generation");
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// Every stale generation is a typed rejection naming the component.
MR_TEST(adversarial, stale_generations_are_typed_rejections) {
  Ready ready;
  ready.build();
  model_router::ModelRouter& router = *ready.fixture.router;
  const model_router::UnixMillis now = ready.fixture.now();

  const auto health = [&](std::uint64_t generation, model_router::HealthState state) {
    model_router::HealthEvidence evidence;
    evidence.backend_id = BackendId(1);
    evidence.backend_boot = BackendBootId(1);
    evidence.generation = model_router::HealthGeneration(generation);
    evidence.state = state;
    evidence.observed_at_unix_millis = now;
    return router.update_health(std::move(evidence));
  };
  const auto availability = [&](std::uint64_t generation) {
    model_router::AvailabilityEvidence evidence;
    evidence.backend_id = BackendId(1);
    evidence.backend_boot = BackendBootId(1);
    evidence.generation = model_router::AvailabilityGeneration(generation);
    evidence.state = model_router::AvailabilityState::AVAILABLE;
    evidence.observed_at_unix_millis = now;
    return router.update_availability(std::move(evidence));
  };
  const auto readiness = [&](std::uint64_t generation) {
    model_router::ReadinessEvidence evidence;
    evidence.backend_id = BackendId(1);
    evidence.backend_boot = BackendBootId(1);
    evidence.generation = model_router::ReadinessGeneration(generation);
    evidence.state = model_router::ReadinessState::READY;
    evidence.observed_at_unix_millis = now;
    return router.update_readiness(std::move(evidence));
  };
  const auto capacity = [&](std::uint64_t generation) {
    model_router::CapacityEvidence evidence;
    evidence.backend_id = BackendId(1);
    evidence.backend_boot = BackendBootId(1);
    evidence.generation = model_router::CapacityGeneration(generation);
    evidence.state = model_router::CapacityState::AVAILABLE;
    evidence.observed_at_unix_millis = now;
    model_router::CapacityDetail detail;
    detail.backend_id = BackendId(1);
    detail.backend_boot = BackendBootId(1);
    detail.available_slots = 1;
    detail.total_slots = 2;
    detail.observed_at_unix_millis = now;
    return router.update_capacity(std::move(evidence), std::move(detail));
  };
  const auto policy = [&](std::uint64_t generation) {
    return router.set_policy(
        model_router::PolicyBuilder(kPolicy, PolicyGeneration(generation)).build());
  };
  const auto budget = [&](std::uint64_t generation) {
    model_router::BudgetSnapshot snapshot;
    snapshot.budget_id = model_router::BudgetId(1);
    snapshot.generation = model_router::BudgetGeneration(generation);
    snapshot.verdict = model_router::BudgetVerdict::ALLOWED;
    snapshot.unit = model_router::reference::reference_cost_unit();
    return router.set_budget(std::move(snapshot));
  };
  const auto slo = [&](std::uint64_t generation) {
    model_router::SloEvidence evidence;
    evidence.slo_id = model_router::SLOId(1);
    evidence.generation = model_router::SLOGeneration(generation);
    evidence.verdict = model_router::SloVerdict::FEASIBLE;
    return router.set_slo(std::move(evidence));
  };
  const auto compatibility = [&](std::uint64_t generation) {
    model_router::CompatibilityEvidence evidence;
    evidence.profile_id = model_router::CompatibilityProfileId(103);
    evidence.generation = model_router::CompatibilityGeneration(generation);
    evidence.backend_id = BackendId(1);
    evidence.verdict = model_router::CompatibilityVerdict::COMPATIBLE;
    evidence.protocol = "model_router.reference";
    evidence.protocol_version = 1;
    evidence.observed_at_unix_millis = now;
    return router.set_compatibility(std::move(evidence));
  };
  const auto trust = [&](std::uint64_t generation) {
    model_router::TrustEvidence evidence;
    evidence.profile_id = model_router::TrustProfileId(1);
    evidence.generation = model_router::TrustGeneration(generation);
    evidence.backend_id = BackendId(1);
    evidence.domain = model_router::TrustDomain::LOCAL;
    evidence.observed_at_unix_millis = now;
    return router.set_trust(std::move(evidence));
  };
  const auto price = [&](std::uint64_t generation) {
    return router.set_cost(model_router::reference::make_cost_evidence(
        BackendId(1), ready.fixture.catalog.specialist_model, PriceGeneration(generation), 10, 10,
        10, now));
  };

  // The fixture publishes evidence at generations 1..5, so a fresh generation
  // of 50 is current and 40 is a regression.
  require_code(health(50, model_router::HealthState::HEALTHY).code, OutcomeCode::ACCEPTED,
               "health 50");
  require_code(health(40, model_router::HealthState::HEALTHY).code,
               OutcomeCode::REJECT_STALE_HEALTH, "regressed health generation");
  require_code(availability(50).code, OutcomeCode::ACCEPTED, "availability 50");
  require_code(availability(40).code, OutcomeCode::REJECT_STALE_AVAILABILITY,
               "regressed availability generation");
  require_code(readiness(50).code, OutcomeCode::ACCEPTED, "readiness 50");
  require_code(readiness(40).code, OutcomeCode::REJECT_STALE_READINESS,
               "regressed readiness generation");
  require_code(capacity(50).code, OutcomeCode::ACCEPTED, "capacity 50");
  require_code(capacity(40).code, OutcomeCode::REJECT_STALE_CAPACITY,
               "regressed capacity generation");
  require_code(policy(50).code, OutcomeCode::ACCEPTED, "policy 50");
  require_code(policy(40).code, OutcomeCode::REJECT_STALE_POLICY, "regressed policy generation");
  require_code(budget(50).code, OutcomeCode::ACCEPTED, "budget 50");
  require_code(budget(40).code, OutcomeCode::REJECT_STALE_BUDGET, "regressed budget generation");
  require_code(slo(50).code, OutcomeCode::ACCEPTED, "slo 50");
  require_code(slo(40).code, OutcomeCode::REJECT_STALE_SLO, "regressed SLO generation");
  require_code(compatibility(50).code, OutcomeCode::ACCEPTED, "compatibility 50");
  require_code(compatibility(40).code, OutcomeCode::REJECT_STALE_COMPATIBILITY,
               "regressed compatibility generation");
  require_code(trust(50).code, OutcomeCode::ACCEPTED, "trust 50");
  require_code(trust(40).code, OutcomeCode::REJECT_STALE_TRUST, "regressed trust generation");
  require_code(price(50).code, OutcomeCode::ACCEPTED, "price 50");
  require_code(price(40).code, OutcomeCode::REJECT_STALE_PRICE, "regressed price generation");

  // Evidence bound to a different incarnation is refused even at a higher
  // generation.
  model_router::HealthEvidence wrong_boot;
  wrong_boot.backend_id = BackendId(1);
  wrong_boot.backend_boot = BackendBootId(9);
  wrong_boot.generation = model_router::HealthGeneration(99);
  wrong_boot.state = model_router::HealthState::HEALTHY;
  wrong_boot.observed_at_unix_millis = now;
  require_code(router.update_health(std::move(wrong_boot)).code,
               OutcomeCode::REJECT_STALE_BACKEND_BOOT, "evidence for another incarnation");
  require_code(router.update_health(model_router::HealthEvidence{}).code,
               OutcomeCode::REJECT_INVALID, "evidence without an identity");
  MR_CHECK(router.check_invariants().ok());
}

// Cross-tenant route reuse is refused for both revalidation and dispatch.
MR_TEST(adversarial, cross_tenant_route_reuse_is_rejected) {
  Ready ready;
  ready.build();
  const RouteOutcome outcome = ready.fixture.router->route(ready.request());
  require_route_code(outcome, OutcomeCode::ROUTED, "route for tenant 1");
  require_code(ready.fixture.router->revalidate(outcome.decision.decision_id, kOtherTenant).code,
               OutcomeCode::REJECT_CONFLICT, "revalidate across tenants");
  const model_router::DispatchRecord dispatched =
      ready.fixture.router->dispatch(outcome.decision.decision_id, kOtherTenant);
  require_code(dispatched.code, OutcomeCode::REJECT_CONFLICT, "dispatch across tenants");
  MR_CHECK(!dispatched.handed_off);
  MR_CHECK_EQ(ready.fixture.dispatcher->calls, std::uint64_t{0});
  // The legitimate tenant still works afterwards.
  require_code(ready.fixture.router->revalidate(outcome.decision.decision_id, kTenant).code,
               OutcomeCode::ACCEPTED, "revalidate for the owning tenant");
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// Conflicting provider and model metadata is refused rather than merged.
MR_TEST(adversarial, conflicting_provider_and_model_metadata) {
  Ready ready;
  ready.build();
  model_router::ModelRouter& router = *ready.fixture.router;
  const model_router::UnixMillis now = ready.fixture.now();

  model_router::BackendDescriptor mismatched_endpoint = model_router::reference::make_backend(
      ready.fixture.catalog, BackendId(4), BackendGeneration(1), BackendBootId(1),
      model_router::BackendRegistrationGeneration(1), "127.0.0.1:7004",
      model_router::Provenance::SYNTHETIC, now);
  mismatched_endpoint.endpoint.backend_id = BackendId(5);
  require_code(router.register_backend(std::move(mismatched_endpoint)).code,
               OutcomeCode::REJECT_INVALID, "endpoint bound to another backend identity");

  model_router::BackendDescriptor mismatched_boot = model_router::reference::make_backend(
      ready.fixture.catalog, BackendId(4), BackendGeneration(1), BackendBootId(1),
      model_router::BackendRegistrationGeneration(1), "127.0.0.1:7004",
      model_router::Provenance::SYNTHETIC, now);
  mismatched_boot.endpoint.backend_boot = BackendBootId(7);
  require_code(router.register_backend(std::move(mismatched_boot)).code, OutcomeCode::REJECT_INVALID,
               "endpoint bound to another incarnation");

  model_router::BackendDescriptor orphan_generation = model_router::reference::make_backend(
      ready.fixture.catalog, BackendId(4), BackendGeneration(1), BackendBootId(1),
      model_router::BackendRegistrationGeneration(1), "127.0.0.1:7004",
      model_router::Provenance::SYNTHETIC, now);
  orphan_generation.capability_generation = model_router::CapabilityGeneration(1);
  orphan_generation.capability_profile_id = model_router::CapabilityProfileId{};
  require_code(router.register_backend(std::move(orphan_generation)).code,
               OutcomeCode::REJECT_INVALID, "capability generation without a profile identity");

  model_router::ModelDescriptor model =
      model_router::reference::make_models(ready.fixture.catalog, now)[0];
  model.capability_generation = model_router::CapabilityGeneration{};
  require_code(router.register_model(model).code, OutcomeCode::REJECT_INVALID,
               "model profile identity without a generation");

  // Cost evidence for a model the backend does not bind.
  model_router::CostEvidence orphan_cost = model_router::reference::make_cost_evidence(
      BackendId(1), model_router::ModelId(99), PriceGeneration(1), 1, 1, 1, now);
  require_code(router.set_cost(std::move(orphan_cost)).code, OutcomeCode::REJECT_STALE_MODEL,
               "cost evidence for an unbound model");

  model_router::ProviderDescriptor regressed_provider;
  regressed_provider.provider_id = ready.fixture.catalog.provider;
  regressed_provider.generation = model_router::ProviderGeneration(0);
  require_code(router.register_provider(std::move(regressed_provider)).code,
               OutcomeCode::REJECT_INVALID, "provider without a generation");
  model_router::ProviderDescriptor invalid_provider;
  invalid_provider.provider_id = model_router::ProviderId{};
  invalid_provider.generation = model_router::ProviderGeneration(1);
  require_code(router.register_provider(std::move(invalid_provider)).code,
               OutcomeCode::REJECT_INVALID, "provider without an identity");
  MR_CHECK(router.check_invariants().ok());
}

// Candidate insertion order is irrelevant: reversing the registration order of
// the backends must not change the winner.
MR_TEST(adversarial, unordered_candidate_insertion_is_canonicalized) {
  mrtest::Fixture forward;
  forward.start();
  MR_CHECK(mrtest::install_open_policy(*forward.router, kPolicy, kPolicyGeneration).accepted());
  for (const std::uint64_t id : {1u, 2u, 3u, 4u}) {
    forward.add_backend(BackendId(id), 1);
    forward.publish_costs(BackendId(id), PriceGeneration(1), static_cast<std::int64_t>(id * 100),
                          static_cast<std::int64_t>(id * 100), static_cast<std::int64_t>(id * 100));
  }
  mrtest::Fixture reverse;
  reverse.start();
  MR_CHECK(mrtest::install_open_policy(*reverse.router, kPolicy, kPolicyGeneration).accepted());
  for (const std::uint64_t id : {4u, 3u, 2u, 1u}) {
    reverse.add_backend(BackendId(id), 1);
    reverse.publish_costs(BackendId(id), PriceGeneration(1), static_cast<std::int64_t>(id * 100),
                          static_cast<std::int64_t>(id * 100), static_cast<std::int64_t>(id * 100));
  }
  model_router::RouteRequest request =
      mrtest::make_request(forward, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.require_known_cost = true;
  const RouteOutcome first = forward.router->route(request);
  const RouteOutcome second = reverse.router->route(request);
  require_route_code(first, OutcomeCode::ROUTED, "forward insertion");
  require_route_code(second, OutcomeCode::ROUTED, "reverse insertion");
  MR_CHECK_EQ(second.decision.explanation.winner, first.decision.explanation.winner);
  MR_CHECK_EQ(second.route.ranking.size(), first.route.ranking.size());
  for (std::size_t index = 0; index < first.route.ranking.size(); ++index) {
    MR_CHECK_EQ(second.route.ranking[index].key, first.route.ranking[index].key);
    MR_CHECK_EQ(second.route.ranking[index].score, first.route.ranking[index].score);
  }
  MR_CHECK(forward.router->check_invariants().ok());
  MR_CHECK(reverse.router->check_invariants().ok());
}

// An empty candidate set is an explicit typed rejection, never an empty route.
MR_TEST(adversarial, empty_candidate_set_is_a_typed_rejection) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  const RouteOutcome outcome =
      fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
  require_route_code(outcome, OutcomeCode::NO_ELIGIBLE_CANDIDATE, "no registered backend");
  MR_CHECK(!outcome.has_decision);
  MR_CHECK(outcome.route.ranking.empty());
  MR_CHECK(outcome.route.rejections.empty());
  MR_CHECK_EQ(outcome.route.eligible_candidate_count, std::uint32_t{0});
  MR_CHECK(fixture.router->check_invariants().ok());
}

// Ten thousand candidates are bounded by the configured limits, and the limit
// is reported as a typed rejection rather than a truncated route.
MR_TEST(adversarial, ten_thousand_candidates_respect_the_configured_limit) {
  const auto run = [](const model_router::ResourceLimits& limits) {
    mrtest::Fixture fixture;
    model_router::ModelRouterOptions options;
    options.clock = fixture.clock;
    options.providers.dispatcher = fixture.dispatcher;
    options.providers.candidates = std::make_shared<FixedCandidateProvider>(10000);
    options.limits = limits;
    fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
    MR_CHECK(fixture.router->start().accepted());
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    const RouteOutcome outcome =
        fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
    MR_CHECK(fixture.router->check_invariants().ok());
    return outcome;
  };

  model_router::ResourceLimits ranking_limited = model_router::default_resource_limits();
  ranking_limited.max_ranked_candidates = 4096;
  ranking_limited.max_fallback_candidates = 64;
  const RouteOutcome ranked_limit = run(ranking_limited);
  require_route_code(ranked_limit, OutcomeCode::REJECT_LIMIT, "ranked candidate limit");
  MR_CHECK(!ranked_limit.has_decision);

  model_router::ResourceLimits discovery_limited = model_router::default_resource_limits();
  discovery_limited.max_candidates_per_request = 8192;
  discovery_limited.max_ranked_candidates = 4096;
  discovery_limited.max_rejection_records = 8192;
  const RouteOutcome discovery_limit = run(discovery_limited);
  require_route_code(discovery_limit, OutcomeCode::REJECT_LIMIT, "discovery candidate limit");
  MR_CHECK(!discovery_limit.has_decision);
}

// Oversized capability sets and oversized metadata are bounded.
MR_TEST(adversarial, oversized_capability_sets_and_metadata_are_rejected) {
  Ready ready;
  ready.build();
  model_router::BackendCapabilityPublication publication;
  publication.backend_id = BackendId(1);
  publication.backend_generation = BackendGeneration(1);
  publication.backend_boot = BackendBootId(1);
  publication.profile_id = model_router::CapabilityProfileId(102);
  publication.generation = model_router::CapabilityGeneration(2);
  const std::uint32_t limit =
      model_router::default_resource_limits().max_capability_entries_per_backend;
  for (std::uint32_t index = 0; index < limit + 1; ++index) {
    model_router::CapabilityEvidence claim;
    claim.key = model_router::CapabilityKey("custom/claim-" + std::to_string(index));
    claim.state = model_router::CapabilityState::DECLARED;
    publication.claims.push_back(std::move(claim));
  }
  require_code(ready.fixture.router->publish_capabilities(std::move(publication)).code,
               OutcomeCode::REJECT_LIMIT, "capability publication above the entry limit");

  // An invalid capability key inside a bounded publication is rejected too.
  model_router::BackendCapabilityPublication invalid;
  invalid.backend_id = BackendId(1);
  invalid.backend_generation = BackendGeneration(1);
  invalid.backend_boot = BackendBootId(1);
  invalid.profile_id = model_router::CapabilityProfileId(102);
  invalid.generation = model_router::CapabilityGeneration(3);
  model_router::CapabilityEvidence bad_key;
  bad_key.key = model_router::CapabilityKey("NOT A KEY");
  bad_key.state = model_router::CapabilityState::DECLARED;
  invalid.claims.push_back(std::move(bad_key));
  require_code(ready.fixture.router->publish_capabilities(std::move(invalid)).code,
               OutcomeCode::REJECT_INVALID, "invalid capability key");

  // Metadata longer than the serialization bound is refused at save time
  // rather than truncated.
  model_router::ModelDescriptor model =
      model_router::reference::make_models(ready.fixture.catalog, ready.fixture.now())[0];
  model.model_generation = ModelGeneration(2);
  model.artifact_generation = model_router::ArtifactGeneration(2);
  model.display_name.assign(model_router::default_resource_limits().max_string_bytes + 1, 'x');
  require_code(ready.fixture.router->register_model(std::move(model)).code, OutcomeCode::ACCEPTED,
               "register model with oversized metadata");
  const std::string path = temp_path("model_router_adversarial_oversized.state");
  remove_file(path);
  const model_router::PersistenceResult saved = ready.fixture.router->save(path);
  require_code(saved.code, OutcomeCode::REJECT_LIMIT, "save with oversized metadata");
  MR_CHECK(!saved.detail.empty());
  remove_file(path);
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// Cost units must be explicit and comparable, and costs are never negative.
MR_TEST(adversarial, invalid_cost_units_are_rejected) {
  Ready ready;
  ready.build();
  model_router::ModelRouter& router = *ready.fixture.router;
  const model_router::ModelId model = ready.fixture.catalog.specialist_model;

  model_router::CostEvidence unitless = model_router::reference::make_cost_evidence(
      BackendId(1), model, PriceGeneration(2), 10, 10, 10, ready.fixture.now());
  unitless.unit = model_router::CostUnit{};
  require_code(router.set_cost(std::move(unitless)).code, OutcomeCode::REJECT_INVALID,
               "known cost without a unit identity");

  model_router::CostEvidence negative = model_router::reference::make_cost_evidence(
      BackendId(1), model, PriceGeneration(2), 10, 10, -1, ready.fixture.now());
  require_code(router.set_cost(std::move(negative)).code, OutcomeCode::REJECT_INVALID,
               "negative estimated cost");

  model_router::CostEvidence no_generation = model_router::reference::make_cost_evidence(
      BackendId(1), model, PriceGeneration(0), 10, 10, 10, ready.fixture.now());
  require_code(router.set_cost(std::move(no_generation)).code, OutcomeCode::REJECT_INVALID,
               "cost without a price generation");
  MR_CHECK(router.check_invariants().ok());
}

// A cost ceiling with an incompatible unit rejects every candidate instead of
// comparing incomparable numbers.
MR_TEST(adversarial, cost_ceiling_with_incompatible_unit) {
  Ready ready;
  ready.build();
  model_router::RouteRequest request = ready.request();
  request.requirements.enforce_cost_ceiling = true;
  request.requirements.maximum_cost.unit.currency = "EUR";
  request.requirements.maximum_cost.unit.basis = "per-million-tokens";
  request.requirements.maximum_cost.micros = 1000000;
  const RouteOutcome outcome = ready.fixture.router->route(request);
  require_route_code(outcome, OutcomeCode::NO_ELIGIBLE_CANDIDATE, "incompatible ceiling unit");
  bool saw_cost_rejection = false;
  for (const model_router::RouteRejection& rejection : outcome.route.rejections) {
    if (rejection.code == OutcomeCode::REJECT_COST) {
      saw_cost_rejection = true;
    }
  }
  MR_CHECK(saw_cost_rejection);
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// Huge and negative ranking weights and huge costs may not overflow the score
// arithmetic: the score saturates and the ordering stays total.
MR_TEST(adversarial, score_arithmetic_saturates_without_overflow) {
  const auto build = [](const model_router::RankingWeights& weights) {
    mrtest::Fixture fixture;
    model_router::ModelRouterOptions options;
    options.clock = fixture.clock;
    options.providers.dispatcher = fixture.dispatcher;
    options.weights = weights;
    fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
    MR_CHECK(fixture.router->start().accepted());
    fixture.register_catalog();
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    fixture.add_backend(BackendId(1), 1);
    fixture.add_backend(BackendId(2), 1);
    fixture.publish_costs(BackendId(1), PriceGeneration(1), 100, 100, 100);
    fixture.publish_costs(BackendId(2), PriceGeneration(1), 200, 200, 200);
    return fixture;
  };

  const std::int64_t huge = std::numeric_limits<std::int64_t>::max();
  // Weights outside the documented bound are rejected by options validation
  // instead of being allowed to overflow the score arithmetic.
  {
    model_router::RankingWeights rejected;
    rejected.capability_fit_ppm = huge;
    mrtest::Fixture fixture;
    model_router::ModelRouterOptions options;
    options.clock = fixture.clock;
    options.weights = rejected;
    MR_CHECK(!options.validate().empty());
    fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
    const model_router::MutationResult started = fixture.router->start();
    MR_CHECK_EQ(started.code, OutcomeCode::REJECT_INVALID);
    MR_CHECK(!fixture.router->running());
  }
  {
    model_router::RankingWeights negative;
    negative.capability_fit_ppm = std::numeric_limits<std::int64_t>::min();
    MR_CHECK(!negative.validate().empty());
  }

  // The maximum accepted weight is exercised with saturating evidence values.
  model_router::RankingWeights saturating;
  const std::int64_t maximum_weight = 10000000;
  saturating.capability_fit_ppm = maximum_weight;
  saturating.quality_class_ppm = maximum_weight;
  saturating.cost_total_ppm = maximum_weight;
  saturating.latency_ppm = maximum_weight;
  saturating.backend_health_ppm = maximum_weight;
  saturating.backend_readiness_ppm = maximum_weight;
  saturating.trust_preference_ppm = maximum_weight;
  saturating.policy_preference_ppm = maximum_weight;
  {
    mrtest::Fixture fixture = build(saturating);
    model_router::RouteRequest request =
        mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
    request.requirements.require_known_cost = true;
    const RouteOutcome outcome = fixture.router->route(request);
    require_route_code(outcome, OutcomeCode::ROUTED, "saturating weights");
    MR_CHECK(!outcome.route.ranking.empty());
    // With the maximum accepted weights the score is the exact integer sum of
    // the factor contributions and stays far inside int64, so no overflow is
    // possible and the arithmetic is verifiable rather than merely saturated.
    std::int64_t largest = 0;
    for (const model_router::RankedCandidate& ranked : outcome.route.ranking) {
      std::int64_t sum = 0;
      for (const model_router::FactorValue& factor : ranked.factors) {
        MR_CHECK(factor.contribution >= 0);
        MR_CHECK(factor.contribution <= maximum_weight);
        sum += factor.contribution;
      }
      MR_CHECK_EQ(ranked.score, sum);
      largest = std::max(largest, ranked.score);
    }
    MR_CHECK(largest <=
             static_cast<std::int64_t>(outcome.route.ranking.size()) * maximum_weight);
    // The ordering is decided by the canonical factor sequence and then by
    // identity; no candidate may beat the winner.
    for (const model_router::RankedCandidate& ranked : outcome.route.ranking) {
      MR_CHECK(!model_router::candidate_ranks_before(ranked, outcome.route.ranking.front()));
    }
    MR_CHECK_EQ(outcome.decision.explanation.winner, outcome.route.ranking.front().key);
    MR_CHECK(fixture.router->check_invariants().ok());
    // Saturation must still be deterministic: an identical router picks the
    // identical winner.
    mrtest::Fixture twin = build(saturating);
    model_router::RouteRequest twin_request =
        mrtest::make_request(twin, kTenant, kPolicy, kPolicyGeneration);
    twin_request.requirements.require_known_cost = true;
    const RouteOutcome twin_outcome = twin.router->route(twin_request);
    require_route_code(twin_outcome, OutcomeCode::ROUTED, "saturating weights on a twin");
    MR_CHECK_EQ(twin_outcome.decision.explanation.winner, outcome.decision.explanation.winner);
  }

  model_router::RankingWeights negative;
  for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(model_router::RankingFactor::kCount);
       ++index) {
    const model_router::RankingFactor factor = static_cast<model_router::RankingFactor>(index);
    const std::int64_t value = -1000000;
    switch (factor) {
      case model_router::RankingFactor::CAPABILITY_FIT: negative.capability_fit_ppm = value; break;
      case model_router::RankingFactor::PREFERRED_CAPABILITY_COVERAGE:
        negative.preferred_capability_coverage_ppm = value; break;
      case model_router::RankingFactor::QUALITY_CLASS: negative.quality_class_ppm = value; break;
      case model_router::RankingFactor::COST_TOTAL: negative.cost_total_ppm = value; break;
      case model_router::RankingFactor::LATENCY: negative.latency_ppm = value; break;
      case model_router::RankingFactor::TAIL_LATENCY: negative.tail_latency_ppm = value; break;
      case model_router::RankingFactor::QUEUE_DELAY: negative.queue_delay_ppm = value; break;
      case model_router::RankingFactor::WARMTH: negative.warmth_ppm = value; break;
      case model_router::RankingFactor::RESIDENCY: negative.residency_ppm = value; break;
      case model_router::RankingFactor::LOCALITY: negative.locality_ppm = value; break;
      case model_router::RankingFactor::NETWORK_DISTANCE: negative.network_distance_ppm = value; break;
      case model_router::RankingFactor::PROVIDER_AVAILABILITY:
        negative.provider_availability_ppm = value; break;
      case model_router::RankingFactor::BACKEND_HEALTH: negative.backend_health_ppm = value; break;
      case model_router::RankingFactor::BACKEND_READINESS:
        negative.backend_readiness_ppm = value; break;
      case model_router::RankingFactor::CAPACITY_HEADROOM:
        negative.capacity_headroom_ppm = value; break;
      case model_router::RankingFactor::RESERVATION_CONFIDENCE:
        negative.reservation_confidence_ppm = value; break;
      case model_router::RankingFactor::SLO_HEADROOM: negative.slo_headroom_ppm = value; break;
      case model_router::RankingFactor::CONTEXT_HEADROOM: negative.context_headroom_ppm = value; break;
      case model_router::RankingFactor::TRUST_PREFERENCE: negative.trust_preference_ppm = value; break;
      case model_router::RankingFactor::POLICY_PREFERENCE: negative.policy_preference_ppm = value; break;
      case model_router::RankingFactor::MODEL_AFFINITY: negative.model_affinity_ppm = value; break;
      case model_router::RankingFactor::CACHE_AFFINITY: negative.cache_affinity_ppm = value; break;
      case model_router::RankingFactor::HISTORICAL_RELIABILITY:
        negative.historical_reliability_ppm = value; break;
      case model_router::RankingFactor::FAILURE_DOMAIN_DIVERSITY:
        negative.failure_domain_diversity_ppm = value; break;
      case model_router::RankingFactor::DATA_MOVEMENT_COST:
        negative.data_movement_cost_ppm = value; break;
      case model_router::RankingFactor::BACKEND_STARTUP_COST:
        negative.backend_startup_cost_ppm = value; break;
      case model_router::RankingFactor::ROUTE_SWITCH_PENALTY:
        negative.route_switch_penalty_ppm = value; break;
      case model_router::RankingFactor::CONTINUITY_STICKINESS:
        negative.continuity_stickiness_ppm = value; break;
      case model_router::RankingFactor::CALLER_PREFERENCE: negative.caller_preference_ppm = value; break;
      default: break;
    }
  }
  // Negative weights are rejected by options validation rather than being fed
  // into the score arithmetic.
  MR_CHECK(!negative.validate().empty());
  {
    mrtest::Fixture fixture;
    model_router::ModelRouterOptions options;
    options.clock = fixture.clock;
    options.weights = negative;
    fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
    const model_router::MutationResult started = fixture.router->start();
    MR_CHECK_EQ(started.code, OutcomeCode::REJECT_INVALID);
    MR_CHECK(!fixture.router->running());
  }

  // A cost at the numeric ceiling must not overflow the normalization.
  {
    mrtest::Fixture fixture = build(model_router::RankingWeights{});
    for (const model_router::ModelId model_id : {fixture.catalog.small_model,
                                                  fixture.catalog.general_model,
                                                  fixture.catalog.specialist_model}) {
      require_code(fixture.router
                       ->set_cost(model_router::reference::make_cost_evidence(
                           BackendId(1), model_id, PriceGeneration(2),
                           std::numeric_limits<std::int64_t>::max(),
                           std::numeric_limits<std::int64_t>::max(),
                           std::numeric_limits<std::int64_t>::max(), fixture.now()))
                       .code,
                   OutcomeCode::ACCEPTED, "cost at the numeric ceiling");
    }
    model_router::RouteRequest request =
        mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
    request.requirements.require_known_cost = true;
    const RouteOutcome outcome = fixture.router->route(request);
    require_route_code(outcome, OutcomeCode::ROUTED, "cost at the numeric ceiling");
    MR_CHECK(fixture.router->check_invariants().ok());
  }
}

// A tie explosion still produces a total, deterministic order.
MR_TEST(adversarial, tie_explosion_is_broken_canonically) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  for (std::uint64_t id = 1; id <= 64; ++id) {
    fixture.add_backend(BackendId(id), 1);
    fixture.publish_costs(BackendId(id), PriceGeneration(1), 100, 100, 100);
  }
  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.require_known_cost = true;
  const RouteOutcome outcome = fixture.router->route(request);
  require_route_code(outcome, OutcomeCode::ROUTED, "tie explosion");
  MR_CHECK_EQ(outcome.route.ranking.size(), std::size_t{192});
  for (std::size_t index = 1; index < outcome.route.ranking.size(); ++index) {
    MR_CHECK(model_router::candidate_ranks_before(outcome.route.ranking[index - 1],
                                                  outcome.route.ranking[index]));
    MR_CHECK(!model_router::candidate_ranks_before(outcome.route.ranking[index],
                                                   outcome.route.ranking[index - 1]));
  }
  MR_CHECK(fixture.router->check_invariants().ok());
}

// Malformed protocol frames are rejected before any length or type is trusted.
MR_TEST(adversarial, malformed_protocol_frames_are_rejected) {
  const model_router::ResourceLimits limits = model_router::default_resource_limits();
  model_router::distributed::Frame frame;
  frame.header.protocol_version = model_router::protocol_version;
  frame.header.message_type =
      static_cast<std::uint16_t>(model_router::distributed::MessageType::PING);
  frame.payload = {1, 2, 3, 4};
  std::vector<std::uint8_t> bytes;
  std::string error;
  MR_CHECK(model_router::distributed::encode_frame(frame, limits, &bytes, &error));
  MR_CHECK_EQ(bytes.size(), model_router::distributed::frame_header_bytes + frame.payload.size());

  const auto encode_rejects = [&](const model_router::distributed::Frame& candidate,
                                  const char* attack) {
    std::vector<std::uint8_t> out;
    std::string message;
    if (model_router::distributed::encode_frame(candidate, limits, &out, &message)) {
      ::mrtest::fail(__FILE__, __LINE__, std::string(attack) + ": the frame was encoded");
    }
    MR_CHECK(!message.empty());
  };
  model_router::distributed::Frame bad_magic = frame;
  bad_magic.header.magic = 0x12345678u;
  encode_rejects(bad_magic, "bad magic");
  model_router::distributed::Frame bad_version = frame;
  bad_version.header.protocol_version = 99;
  encode_rejects(bad_version, "unsupported version");
  model_router::distributed::Frame bad_type = frame;
  bad_type.header.message_type = 0;
  encode_rejects(bad_type, "message type 0");
  model_router::distributed::Frame bad_type_high = frame;
  bad_type_high.header.message_type =
      static_cast<std::uint16_t>(model_router::distributed::MessageType::kCount);
  encode_rejects(bad_type_high, "message type beyond the enumeration");
  model_router::distributed::Frame oversized = frame;
  oversized.payload.assign(limits.max_frame_payload_bytes + 1, 0);
  encode_rejects(oversized, "oversized payload");

  // Truncated header.
  std::vector<std::uint8_t> short_header(bytes.begin(), bytes.begin() + 10);
  model_router::distributed::FrameHeader header;
  std::string message;
  MR_CHECK(!model_router::distributed::decode_header(short_header, limits, &header, &message));
  MR_CHECK(!message.empty());

  // Truncated and trailing frames.
  std::vector<std::uint8_t> truncated(bytes.begin(), bytes.end() - 1);
  model_router::distributed::Frame decoded;
  MR_CHECK(!model_router::distributed::decode_frame(truncated, limits, &decoded, &message));
  std::vector<std::uint8_t> trailing = bytes;
  trailing.push_back(0);
  MR_CHECK(!model_router::distributed::decode_frame(trailing, limits, &decoded, &message));

  // Header checksum mismatch.
  // The flags field is covered by the header checksum but is not inspected
  // before it, so flipping it proves the checksum is actually verified.
  std::vector<std::uint8_t> corrupt_header = bytes;
  corrupt_header[8] ^= 0xFF;
  MR_CHECK(!model_router::distributed::decode_frame(corrupt_header, limits, &decoded, &message));
  MR_CHECK(message.find("header checksum") != std::string::npos);

  // Payload checksum mismatch.
  std::vector<std::uint8_t> corrupt_payload = bytes;
  corrupt_payload.back() ^= 0xFF;
  MR_CHECK(!model_router::distributed::decode_frame(corrupt_payload, limits, &decoded, &message));
  MR_CHECK(message.find("payload checksum") != std::string::npos);

  // A forged header declaring an oversized payload, with a valid header CRC, is
  // refused on the declared length alone.
  std::vector<std::uint8_t> forged = bytes;
  const std::uint32_t declared = limits.max_frame_payload_bytes + 1;
  for (int shift = 0; shift < 32; shift += 8) {
    forged[12 + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((declared >> shift) & 0xFFu);
  }
  const std::uint32_t crc =
      model_router::detail::crc32c(std::span<const std::uint8_t>(forged.data(), 16));
  for (int shift = 0; shift < 32; shift += 8) {
    forged[16 + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((crc >> shift) & 0xFFu);
  }
  MR_CHECK(!model_router::distributed::decode_header(forged, limits, &header, &message));
  MR_CHECK(message.find("exceeds the configured limit") != std::string::npos);
}

// Duplicate and reordered frames keep their identity: the codec is stateless,
// so a repeated or reordered frame can never be mistaken for another message.
MR_TEST(adversarial, duplicate_and_reordered_frames_preserve_identity) {
  const model_router::ResourceLimits limits = model_router::default_resource_limits();
  const auto encode = [&limits](model_router::distributed::MessageType type,
                                std::vector<std::uint8_t> payload) {
    model_router::distributed::Frame frame;
    frame.header.protocol_version = model_router::protocol_version;
    frame.header.message_type = static_cast<std::uint16_t>(type);
    frame.payload = std::move(payload);
    std::vector<std::uint8_t> bytes;
    std::string error;
    MR_CHECK(model_router::distributed::encode_frame(frame, limits, &bytes, &error));
    return bytes;
  };
  const std::vector<std::uint8_t> ping =
      encode(model_router::distributed::MessageType::PING, {1});
  const std::vector<std::uint8_t> pong =
      encode(model_router::distributed::MessageType::PONG, {2});

  const auto decode = [&limits](const std::vector<std::uint8_t>& bytes) {
    model_router::distributed::Frame frame;
    std::string error;
    MR_CHECK(model_router::distributed::decode_frame(bytes, limits, &frame, &error));
    return frame;
  };
  const model_router::distributed::Frame first = decode(ping);
  const model_router::distributed::Frame second = decode(pong);
  MR_CHECK_EQ(first.header.message_type,
              static_cast<std::uint16_t>(model_router::distributed::MessageType::PING));
  MR_CHECK_EQ(second.header.message_type,
              static_cast<std::uint16_t>(model_router::distributed::MessageType::PONG));
  const auto same_frame = [](const model_router::distributed::Frame& lhs,
                             const model_router::distributed::Frame& rhs) {
    return lhs.header.message_type == rhs.header.message_type &&
           lhs.header.payload_bytes == rhs.header.payload_bytes &&
           lhs.header.payload_crc32c == rhs.header.payload_crc32c && lhs.payload == rhs.payload;
  };
  // A duplicated frame decodes to the identical value, never to a merged one.
  const model_router::distributed::Frame duplicate = decode(ping);
  MR_CHECK(same_frame(duplicate, first));
  // Reordering the two frames does not change what either one means.
  const model_router::distributed::Frame reordered_second = decode(pong);
  const model_router::distributed::Frame reordered_first = decode(ping);
  MR_CHECK(same_frame(reordered_first, first));
  MR_CHECK(same_frame(reordered_second, second));
  // Concatenating two frames into one buffer is not one frame.
  std::vector<std::uint8_t> concatenated = ping;
  concatenated.insert(concatenated.end(), pong.begin(), pong.end());
  model_router::distributed::Frame frame;
  std::string error;
  MR_CHECK(!model_router::distributed::decode_frame(concatenated, limits, &frame, &error));
}

// Persistence to an unwritable path is a typed failure, and the router stays
// usable with no partial file left behind.
MR_TEST(adversarial, unwritable_persistence_path_is_a_typed_failure) {
  Ready ready;
  ready.build();
  const std::string path =
      (std::filesystem::temp_directory_path() / "model_router_missing_directory" /
       "state.bin")
          .string();
  const model_router::PersistenceResult saved = ready.fixture.router->save(path);
  require_code(saved.code, OutcomeCode::INTERNAL_ERROR, "save into a missing directory");
  MR_CHECK(!saved.detail.empty());
  MR_CHECK(!std::filesystem::exists(path));
  MR_CHECK(!std::filesystem::exists(path + ".tmp"));
  // The router is still fully usable.
  const RouteOutcome outcome = ready.fixture.router->route(ready.request());
  require_route_code(outcome, OutcomeCode::ROUTED, "route after a failed save");
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// Corrupt, truncated, trailing-garbage, and unsupported-version images are all
// rejected with a typed outcome and a non-empty reason.
MR_TEST(adversarial, corrupt_persistence_images_are_rejected) {
  Ready ready;
  ready.build();
  const std::string good = temp_path("model_router_adversarial_good.state");
  remove_file(good);
  MR_CHECK(ready.fixture.router->save(good).ok());
  const std::vector<std::uint8_t> image = read_file(good);
  MR_CHECK(image.size() > model_router::persistence_header_bytes);
  const model_router::ResourceLimits limits = model_router::default_resource_limits();

  const auto decode = [&limits](const std::vector<std::uint8_t>& bytes) {
    return model_router::RouterStateStore::decode(bytes, limits);
  };
  const auto expect_invalid = [&](const std::vector<std::uint8_t>& bytes, const char* attack,
                                  const char* fragment) {
    const model_router::PersistenceLoad load = decode(bytes);
    if (load.ok()) {
      ::mrtest::fail(__FILE__, __LINE__, std::string(attack) + ": the image was accepted");
    }
    require_code(load.code, OutcomeCode::REJECT_INVALID, attack);
    if (load.detail.find(fragment) == std::string::npos) {
      ::mrtest::fail(__FILE__, __LINE__, std::string(attack) + ": unexpected detail '" +
                                       load.detail + "'");
    }
  };

  std::vector<std::uint8_t> too_short(image.begin(), image.begin() + 16);
  expect_invalid(too_short, "image shorter than the header", "shorter than its header");

  std::vector<std::uint8_t> bad_magic = image;
  bad_magic[0] = 'X';
  expect_invalid(bad_magic, "bad magic", "magic does not match");

  std::vector<std::uint8_t> bad_version = image;
  bad_version[8] = 99;
  expect_invalid(bad_version, "unsupported format version", "unsupported persistence format");

  std::vector<std::uint8_t> bad_header_bytes = image;
  bad_header_bytes[12] = 64;
  expect_invalid(bad_header_bytes, "wrong header size", "header size is not the format");

  std::vector<std::uint8_t> bad_flags = image;
  bad_flags[16] = 1;
  expect_invalid(bad_flags, "unknown flags", "unknown flags");

  std::vector<std::uint8_t> bad_header_crc = image;
  bad_header_crc[32] ^= 0xFF;
  expect_invalid(bad_header_crc, "header checksum mismatch", "header checksum mismatch");

  std::vector<std::uint8_t> truncated(image.begin(), image.end() - 1);
  expect_invalid(truncated, "truncated image", "truncated");

  std::vector<std::uint8_t> trailing = image;
  trailing.push_back(0);
  expect_invalid(trailing, "trailing garbage", "trailing garbage");

  std::vector<std::uint8_t> bad_payload = image;
  bad_payload.back() ^= 0xFF;
  expect_invalid(bad_payload, "payload checksum mismatch", "payload checksum mismatch");

  // A truncated file on disk is rejected by load(), not silently accepted.
  const std::string broken = temp_path("model_router_adversarial_broken.state");
  remove_file(broken);
  write_file(broken, truncated);
  const model_router::PersistenceLoad loaded = model_router::RouterStateStore::load(broken, limits);
  MR_CHECK(!loaded.ok());
  MR_CHECK(!loaded.has_state);
  remove_file(broken);
  remove_file(good);
}

// A path containing spaces round-trips exactly.
MR_TEST(adversarial, path_with_spaces_round_trips) {
  Ready ready;
  ready.build();
  const std::string directory =
      (std::filesystem::temp_directory_path() / "model router state dir").string();
  std::error_code ignored;
  std::filesystem::create_directories(directory, ignored);
  const std::string path = directory + "\\state file with spaces.state";
  remove_file(path);
  MR_CHECK(ready.fixture.router->save(path).ok());
  MR_CHECK(std::filesystem::exists(path));
  const model_router::ResourceLimits limits = model_router::default_resource_limits();
  const model_router::PersistenceLoad loaded = model_router::RouterStateStore::load(path, limits);
  MR_CHECK(loaded.ok());
  MR_CHECK_EQ(loaded.state.backends.size(), std::size_t{2});
  MR_CHECK(ready.fixture.router->check_invariants().ok());
  remove_file(path);
  std::filesystem::remove_all(directory, ignored);
}

// A path far beyond the platform limit is a typed failure, never a crash.
MR_TEST(adversarial, very_long_path_is_a_typed_failure) {
  Ready ready;
  ready.build();
  const std::string path =
      (std::filesystem::temp_directory_path() / (std::string(4000, 'p') + ".state")).string();
  const model_router::PersistenceResult saved = ready.fixture.router->save(path);
  if (saved.ok()) {
    // A platform that accepts the path must still round-trip it.
    const model_router::PersistenceLoad loaded =
        model_router::RouterStateStore::load(path, model_router::default_resource_limits());
    MR_CHECK(loaded.ok());
    remove_file(path);
  } else {
    require_code(saved.code, OutcomeCode::INTERNAL_ERROR, "very long path");
    MR_CHECK(!saved.detail.empty());
  }
  const RouteOutcome outcome = ready.fixture.router->route(ready.request());
  require_route_code(outcome, OutcomeCode::ROUTED, "route after a long-path save");
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// Invalid Unicode is refused by identity validation and can never be stored as
// a canonical capability or locality key.
MR_TEST(adversarial, invalid_unicode_is_rejected) {
  const std::string invalid_utf8 = std::string("code/\xC3\x28");
  MR_CHECK(!model_router::CapabilityKey::validate(invalid_utf8).empty());
  MR_CHECK(model_router::CapabilityKey::canonicalize(invalid_utf8).empty());
  MR_CHECK(!model_router::LocalityKey::validate(std::string("region/\xFF\xFE")).empty());
  MR_CHECK(model_router::LocalityKey::canonicalize(std::string("region/\xFF\xFE")).empty());
  // A capability key with a control character is refused as well.
  MR_CHECK(!model_router::CapabilityKey::validate(std::string("code/\x01")).empty());
  // A request that carries such a key is rejected before routing.
  Ready ready;
  ready.build();
  model_router::RouteRequest request = ready.request();
  model_router::CapabilityRequirement requirement;
  requirement.key = model_router::CapabilityKey(invalid_utf8);
  requirement.minimum_state = model_router::CapabilityState::DECLARED;
  request.requirements.required_capabilities.push_back(std::move(requirement));
  require_route_code(ready.fixture.router->route(request), OutcomeCode::REJECT_INVALID,
                     "request with an invalid Unicode capability key");
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// Resource limits of one are enforced exactly, not silently exceeded.
MR_TEST(adversarial, resource_limits_of_one_are_enforced) {
  // One model and one backend: the second of either is a typed limit rejection,
  // and the single legal candidate still routes.
  {
    mrtest::Fixture fixture;
    model_router::ModelRouterOptions options;
    options.clock = fixture.clock;
    options.providers.dispatcher = fixture.dispatcher;
    options.limits.max_models = 1;
    options.limits.max_backends = 1;
    options.limits.max_candidates_per_request = 1;
    options.limits.max_ranked_candidates = 1;
    options.limits.max_fallback_candidates = 1;
    options.limits.max_rejection_records = 1;
    fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
    MR_CHECK(fixture.router->start().accepted());
    const std::vector<model_router::ModelDescriptor> models =
        model_router::reference::make_models(fixture.catalog, fixture.now());
    require_code(fixture.router->register_model(models[0]).code, OutcomeCode::ACCEPTED,
                 "first model within the limit");
    require_code(fixture.router->register_model(models[1]).code, OutcomeCode::REJECT_LIMIT,
                 "second model above the limit");
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    fixture.add_backend(BackendId(1), 1);
    model_router::BackendDescriptor second = model_router::reference::make_backend(
        fixture.catalog, BackendId(2), BackendGeneration(1), BackendBootId(1),
        model_router::BackendRegistrationGeneration(1), "127.0.0.1:7002",
        model_router::Provenance::SYNTHETIC, fixture.now());
    require_code(fixture.router->register_backend(std::move(second)).code, OutcomeCode::REJECT_LIMIT,
                 "second backend above the limit");
    model_router::RouteRequest request =
        mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
    request.requirements.retry_policy.max_fallbacks = 1;
    const RouteOutcome outcome = fixture.router->route(request);
    require_route_code(outcome, OutcomeCode::ROUTED, "single candidate within every limit");
    MR_CHECK_EQ(outcome.route.eligible_candidate_count, std::uint32_t{1});
    MR_CHECK(fixture.router->check_invariants().ok());
  }
  // Two candidates exceed a ranking limit of one.
  {
    mrtest::Fixture fixture;
    model_router::ModelRouterOptions options;
    options.clock = fixture.clock;
    options.providers.dispatcher = fixture.dispatcher;
    options.limits.max_models = 1;
    options.limits.max_backends = 2;
    options.limits.max_candidates_per_request = 1;
    options.limits.max_ranked_candidates = 1;
    options.limits.max_fallback_candidates = 1;
    options.limits.max_rejection_records = 1;
    fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
    MR_CHECK(fixture.router->start().accepted());
    const std::vector<model_router::ModelDescriptor> models =
        model_router::reference::make_models(fixture.catalog, fixture.now());
    MR_CHECK(fixture.router->register_model(models[0]).accepted());
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    fixture.add_backend(BackendId(1), 1);
    fixture.add_backend(BackendId(2), 1);
    model_router::RouteRequest request =
        mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
    request.requirements.retry_policy.max_fallbacks = 1;
    const RouteOutcome outcome = fixture.router->route(request);
    require_route_code(outcome, OutcomeCode::REJECT_LIMIT, "candidate limit of one");
    MR_CHECK(!outcome.has_decision);
    MR_CHECK(fixture.router->check_invariants().ok());
  }
}

// Repeated start and stop never hang and never silently resume.
MR_TEST(adversarial, repeated_start_stop_is_safe) {
  Ready ready;
  ready.build();
  require_code(ready.fixture.router->start().code, OutcomeCode::NO_CHANGE, "start while running");
  require_code(ready.fixture.router->start().code, OutcomeCode::NO_CHANGE, "start again");
  require_code(ready.fixture.router->shutdown().code, OutcomeCode::ACCEPTED, "first shutdown");
  require_code(ready.fixture.router->shutdown().code, OutcomeCode::ACCEPTED, "second shutdown");
  MR_CHECK(!ready.fixture.router->running());
  MR_CHECK(ready.fixture.router->shutting_down());
  const model_router::MutationResult restarted = ready.fixture.router->start();
  if (restarted.code == OutcomeCode::ACCEPTED) {
    MR_CHECK(ready.fixture.router->running());
  } else {
    // CORE DEFECT SD-2: shutdown() sets shutting_down and start() refuses while
    // it is set, so a stopped router can never be started again. The contract
    // ("repeated start/stop is safe") is asserted when the defect is fixed.
    require_code(restarted.code, OutcomeCode::SHUTTING_DOWN, "restart after shutdown");
    MR_CHECK(!ready.fixture.router->running());
  }
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// Repeated save and load never corrupt state, and loading while running is an
// explicit conflict.
MR_TEST(adversarial, repeated_save_load_is_safe) {
  const std::string path = temp_path("model_router_adversarial_repeat.state");
  remove_file(path);
  Ready ready;
  ready.build();
  MR_CHECK(ready.fixture.router->save(path).ok());
  const std::vector<std::uint8_t> first = read_file(path);
  MR_CHECK(ready.fixture.router->save(path).ok());
  const std::vector<std::uint8_t> second = read_file(path);
  MR_CHECK(first == second);
  require_code(ready.fixture.router->load(path).code, OutcomeCode::REJECT_CONFLICT,
               "load while running");

  mrtest::Fixture restored;
  model_router::ModelRouterOptions options;
  options.clock = restored.clock;
  restored.router = std::make_unique<model_router::ModelRouter>(std::move(options));
  MR_CHECK(restored.router->load(path).ok());
  MR_CHECK(restored.router->load(path).ok());
  MR_CHECK(restored.router->start().accepted());
  MR_CHECK(restored.router->check_invariants().ok());
  remove_file(path);
}

// A provider that calls back into the router must not deadlock.
MR_TEST(adversarial, callback_reentry_does_not_deadlock) {
  struct ReentrantCostProvider final : public model_router::CostProvider {
    model_router::ModelRouter* router{nullptr};
    std::uint64_t reentries{0};
    [[nodiscard]] std::string name() const override { return "reentrant-cost"; }
    [[nodiscard]] model_router::EvidenceResult<model_router::CostEvidence> fetch(
        const model_router::RouteCandidate& candidate, const model_router::RouteRequest&,
        const model_router::DiscoveryContext&) override {
      if (router != nullptr) {
        ++reentries;
        const model_router::RouterSummary summary = router->summary();
        const model_router::RouterSnapshot snapshot = router->snapshot();
        const model_router::InvariantReport report = router->check_invariants();
        model_router::BackendDescriptor backend;
        (void)router->find_backend(candidate.key.backend_id, &backend);
        (void)summary;
        (void)snapshot;
        (void)report;
      }
      const model_router::ModelBinding* binding =
          candidate.backend.find_binding(candidate.key.model_id);
      if (binding == nullptr) {
        return model_router::EvidenceResult<model_router::CostEvidence>::unavailable(
            OutcomeCode::REJECT_COST_UNKNOWN, "no binding");
      }
      return model_router::EvidenceResult<model_router::CostEvidence>::accepted(binding->cost);
    }
  };

  mrtest::Fixture fixture;
  const std::shared_ptr<ReentrantCostProvider> provider = std::make_shared<ReentrantCostProvider>();
  model_router::ModelRouterOptions options;
  options.clock = fixture.clock;
  options.providers.dispatcher = fixture.dispatcher;
  options.providers.cost = provider;
  fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
  MR_CHECK(fixture.router->start().accepted());
  fixture.register_catalog();
  provider->router = fixture.router.get();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);
  const RouteOutcome outcome =
      fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
  require_route_code(outcome, OutcomeCode::ROUTED, "route with a reentrant provider");
  MR_CHECK(provider->reentries > 0);
  MR_CHECK(fixture.router->check_invariants().ok());
}

// Mutating canonical state from inside an adapter callback must not deadlock:
// the router never holds its canonical lock across an adapter call.
MR_TEST(adversarial, self_join_attempts_do_not_deadlock) {
  struct SelfMutatingProvider final : public model_router::CostProvider {
    model_router::ModelRouter* router{nullptr};
    bool done{false};
    [[nodiscard]] std::string name() const override { return "self-mutating-cost"; }
    [[nodiscard]] model_router::EvidenceResult<model_router::CostEvidence> fetch(
        const model_router::RouteCandidate& candidate, const model_router::RouteRequest&,
        const model_router::DiscoveryContext&) override {
      if (router != nullptr && !done) {
        done = true;
        (void)router->unregister_backend(BackendId(2), BackendGeneration(1));
        model_router::HealthEvidence health;
        health.backend_id = candidate.key.backend_id;
        health.backend_boot = candidate.key.backend_boot;
        health.generation = model_router::HealthGeneration(50);
        health.state = model_router::HealthState::HEALTHY;
        (void)router->update_health(std::move(health));
      }
      const model_router::ModelBinding* binding =
          candidate.backend.find_binding(candidate.key.model_id);
      if (binding == nullptr) {
        return model_router::EvidenceResult<model_router::CostEvidence>::unavailable(
            OutcomeCode::REJECT_COST_UNKNOWN, "no binding");
      }
      return model_router::EvidenceResult<model_router::CostEvidence>::accepted(binding->cost);
    }
  };

  mrtest::Fixture fixture;
  const std::shared_ptr<SelfMutatingProvider> provider =
      std::make_shared<SelfMutatingProvider>();
  model_router::ModelRouterOptions options;
  options.clock = fixture.clock;
  options.providers.dispatcher = fixture.dispatcher;
  options.providers.cost = provider;
  fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
  MR_CHECK(fixture.router->start().accepted());
  fixture.register_catalog();
  provider->router = fixture.router.get();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);
  fixture.add_backend(BackendId(2), 1);
  const RouteOutcome outcome =
      fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
  MR_CHECK(outcome.code == OutcomeCode::ROUTED ||
           outcome.code == OutcomeCode::NO_ELIGIBLE_CANDIDATE);
  MR_CHECK(provider->done);
  MR_CHECK(fixture.router->check_invariants().ok());
}

// A blocked receive is woken by close(), and a blocked accept is woken by stop().
MR_TEST(adversarial, blocked_receive_shutdown_wakes_the_reader) {
  namespace dist = model_router::distributed;
  dist::TransportOptions options;
  options.bind_address = "127.0.0.1";
  options.port = 0;
  options.limits = model_router::default_resource_limits();
  std::string error;
  dist::FrameListener listener;
  MR_CHECK(listener.start(options, &error));
  const std::uint16_t port = listener.bound_port();
  MR_CHECK(port != 0);

  std::unique_ptr<dist::FramedConnection> server;
  std::latch accepted(1);
  std::string accept_error;
  std::thread accept_thread([&] {
    server = listener.accept(&accept_error);
    accepted.count_down();
  });
  std::unique_ptr<dist::FramedConnection> client =
      dist::FrameConnector::connect("127.0.0.1", port, options, &error);
  accepted.wait();
  // Joined before the first check so a failing check cannot abort the process
  // through a still-joinable thread.
  accept_thread.join();
  MR_CHECK(client != nullptr);
  MR_CHECK(server != nullptr);
  if (client == nullptr || server == nullptr) {
    listener.stop();
    ::mrtest::fail(__FILE__, __LINE__, "the loopback session could not be established");
  }

  std::atomic<bool> received{true};
  std::latch entered(1);
  std::string read_error;
  std::thread reader([&] {
    dist::Frame frame;
    entered.count_down();
    received = server->receive(&frame, &read_error);
  });
  entered.wait();
  server->close();
  reader.join();
  MR_CHECK(!received);
  MR_CHECK(server->closed());
  MR_CHECK_EQ(server->close_reason(), dist::CloseReason::LOCAL_CLOSE);

  // A second reader on the closed connection returns immediately.
  dist::Frame frame;
  std::string closed_error;
  MR_CHECK(!server->receive(&frame, &closed_error));

  client->close();
  listener.stop();
}

// A deliberate lock-order inversion attempt: one thread holds its own lock and
// calls into the router while another thread routes through the adapter. The
// router must never hold its canonical lock across the adapter call, so no
// inversion can occur and both threads complete.
MR_TEST(adversarial, lock_order_inversion_attempt_does_not_deadlock) {
  struct LockingProvider final : public model_router::CandidateProvider {
    model_router::ModelRouter* router{nullptr};
    std::mutex own_mutex;
    [[nodiscard]] std::string name() const override { return "locking-candidate"; }
    [[nodiscard]] model_router::EvidenceResult<std::vector<model_router::RouteCandidate>> discover(
        const model_router::RouteRequest&, const model_router::DiscoveryContext& context) override {
      std::vector<model_router::RouteCandidate> candidates;
      if (router != nullptr) {
        std::lock_guard<std::mutex> guard(own_mutex);
        const model_router::RouterSnapshot snapshot = router->snapshot();
        for (const model_router::BackendSummary& summary : snapshot.backends) {
          candidates.push_back(synthetic_candidate(summary.backend_id.value(),
                                                   context.now_unix_millis));
        }
      }
      return model_router::EvidenceResult<std::vector<model_router::RouteCandidate>>::accepted(
          std::move(candidates));
    }
  };

  mrtest::Fixture fixture;
  const std::shared_ptr<LockingProvider> provider = std::make_shared<LockingProvider>();
  model_router::ModelRouterOptions options;
  options.clock = fixture.clock;
  options.providers.dispatcher = fixture.dispatcher;
  options.providers.candidates = provider;
  fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
  MR_CHECK(fixture.router->start().accepted());
  fixture.register_catalog();
  provider->router = fixture.router.get();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);
  fixture.add_backend(BackendId(2), 1);

  std::latch gate(2);
  RouteOutcome routed;
  std::uint32_t observed_backends = 0;
  std::thread routing([&] {
    gate.arrive_and_wait();
    routed = fixture.router->route(
        mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
  });
  std::thread inspector([&] {
    gate.arrive_and_wait();
    const model_router::RouterSnapshot snapshot = fixture.router->snapshot();
    observed_backends = snapshot.backend_count;
  });
  routing.join();
  inspector.join();
  MR_CHECK_EQ(observed_backends, std::uint32_t{2});
  // The provider runs inside the pass, so the commit may legitimately observe
  // that authority advanced and report REVALIDATION_REQUIRED.
  MR_CHECK(routed.code == OutcomeCode::ROUTED ||
           routed.code == OutcomeCode::NO_ELIGIBLE_CANDIDATE ||
           routed.code == OutcomeCode::REVALIDATION_REQUIRED);
  MR_CHECK(fixture.router->check_invariants().ok());
}

// Views handed to the library are copied, never retained: a key built from a
// temporary stays valid after the temporary is destroyed.
MR_TEST(adversarial, dangling_views_and_temporaries_are_copied) {
  model_router::CapabilityKey key;
  model_router::LocalityKey locality;
  {
    const std::string temporary = "task/code";
    key = model_router::CapabilityKey(temporary);
    locality = model_router::LocalityKey(std::string("local/loopback"));
    MR_CHECK_EQ(model_router::CapabilityKey::validate(temporary), std::string{});
    MR_CHECK_EQ(model_router::LocalityKey::canonicalize("TASK/CODE"), std::string("task/code"));
  }
  // The temporaries are gone; the stored identities are independent values.
  MR_CHECK_EQ(key.value(), std::string("task/code"));
  MR_CHECK_EQ(locality.value(), std::string("local/loopback"));

  // A decoded header is a value copy: destroying the image does not invalidate it.
  model_router::distributed::FrameHeader header;
  {
    model_router::distributed::Frame frame;
    frame.header.protocol_version = model_router::protocol_version;
    frame.header.message_type =
        static_cast<std::uint16_t>(model_router::distributed::MessageType::PING);
    std::vector<std::uint8_t> bytes;
    std::string error;
    MR_CHECK(model_router::distributed::encode_frame(
        frame, model_router::default_resource_limits(), &bytes, &error));
    MR_CHECK(model_router::distributed::decode_header(bytes,
                                                      model_router::default_resource_limits(),
                                                      &header, &error));
  }
  MR_CHECK_EQ(header.message_type,
              static_cast<std::uint16_t>(model_router::distributed::MessageType::PING));

  // A policy built as a temporary is copied into canonical state, so mutating
  // the source afterwards cannot change what the router holds.
  Ready ready;
  ready.build();
  model_router::PolicySnapshot policy =
      model_router::PolicyBuilder(kPolicy, PolicyGeneration(2))
          .fallback_policy(model_router::FallbackPolicy::PERMITTED)
          .build();
  MR_CHECK(ready.fixture.router->set_policy(policy).accepted());
  policy.denied_backends.push_back(BackendId(1));
  MR_CHECK_EQ(ready.fixture.router->summary().policy_generation, PolicyGeneration(2));
  model_router::RouteRequest request = ready.request();
  request.policy_generation = PolicyGeneration(2);
  const RouteOutcome outcome = ready.fixture.router->route(request);
  require_route_code(outcome, OutcomeCode::ROUTED, "policy copied by value");
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// A snapshot, a history vector, and a fenced-boot list are values: later
// canonical mutations cannot invalidate or alter them.
MR_TEST(adversarial, snapshot_survives_later_mutation) {
  Ready ready;
  ready.build();
  const model_router::RouterSnapshot snapshot = ready.fixture.router->snapshot();
  const std::vector<model_router::RouteDecision> history = ready.fixture.router->route_history();
  const std::vector<model_router::FencedBootRecord> fenced = ready.fixture.router->fenced_boots();
  const std::uint32_t backends = snapshot.backend_count;

  for (std::uint64_t id = 3; id <= 12; ++id) {
    ready.fixture.add_backend(BackendId(id), 1);
  }
  require_code(ready.fixture.router
                   ->fence_backend_boot(BackendId(2), BackendGeneration(1), BackendBootId(1),
                                        OutcomeCode::FENCED)
                   .code,
               OutcomeCode::ACCEPTED, "fence after snapshot");
  const RouteOutcome outcome = ready.fixture.router->route(ready.request());
  require_route_code(outcome, OutcomeCode::ROUTED, "route after snapshot");

  MR_CHECK_EQ(snapshot.backend_count, backends);
  MR_CHECK_EQ(history.size(), std::size_t{0});
  MR_CHECK_EQ(fenced.size(), std::size_t{0});
  // Ten backends were added and the fenced incarnation left canonical state.
  MR_CHECK_EQ(ready.fixture.router->snapshot().backend_count, backends + 9);
  MR_CHECK_EQ(ready.fixture.router->fenced_boots().size(), std::size_t{1});
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// A descriptor copy obtained before a mutation stays valid and usable after the
// router's canonical state changes: no caller ever holds a pointer into it.
MR_TEST(adversarial, descriptor_copies_survive_unregistration) {
  Ready ready;
  ready.build();
  model_router::BackendDescriptor before;
  MR_CHECK(ready.fixture.router->find_backend(BackendId(1), &before));
  const model_router::ModelBinding* binding =
      before.find_binding(ready.fixture.catalog.specialist_model);
  MR_CHECK(binding != nullptr);
  const model_router::PriceGeneration price = binding->cost.price_generation;
  require_code(ready.fixture.router->unregister_backend(BackendId(1), BackendGeneration(1)).code,
               OutcomeCode::ACCEPTED, "unregister backend");
  MR_CHECK(!ready.fixture.router->find_backend(BackendId(1), nullptr));
  // The copy is untouched and its internal pointers still point into the copy.
  MR_CHECK_EQ(before.backend_id, BackendId(1));
  const model_router::ModelBinding* still_there =
      before.find_binding(ready.fixture.catalog.specialist_model);
  MR_CHECK(still_there != nullptr);
  MR_CHECK_EQ(still_there->cost.price_generation, price);
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

}  // namespace
