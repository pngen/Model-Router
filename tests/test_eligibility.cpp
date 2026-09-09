// Model Router - hard eligibility predicates and typed rejections.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Every predicate here requires affirmative proof: UNKNOWN evidence never
// passes, and each rejection carries the exact OutcomeCode that names the
// predicate that failed.

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "eligibility.hpp"
#include "router_fixture.hpp"

using model_router::ArtifactGeneration;
using model_router::AvailabilityState;
using model_router::BackendBootId;
using model_router::BackendCapabilityPublication;
using model_router::BackendDescriptor;
using model_router::BackendGeneration;
using model_router::BackendId;
using model_router::BackendRegistrationGeneration;
using model_router::BudgetGeneration;
using model_router::BudgetId;
using model_router::BudgetSnapshot;
using model_router::BudgetVerdict;
using model_router::CapabilityEvidence;
using model_router::CapabilityGeneration;
using model_router::CapabilityKey;
using model_router::CapabilityProfileId;
using model_router::CapabilityRequirement;
using model_router::CapabilityState;
using model_router::CapacityEvidence;
using model_router::CapacityState;
using model_router::CompatibilityEvidence;
using model_router::CompatibilityGeneration;
using model_router::CompatibilityProfileId;
using model_router::CompatibilityVerdict;
using model_router::CostEvidence;
using model_router::CostEvidenceId;
using model_router::CostKnownness;
using model_router::EndpointId;
using model_router::HealthState;
using model_router::LocalityKey;
using model_router::ModelBinding;
using model_router::ModelDescriptor;
using model_router::ModelFamilyId;
using model_router::ModelGeneration;
using model_router::ModelId;
using model_router::ModelLifecycle;
using model_router::ModelVersionId;
using model_router::Modality;
using model_router::ModalitySet;
using model_router::OutcomeCode;
using model_router::PolicyGeneration;
using model_router::PolicyId;
using model_router::PolicySnapshot;
using model_router::PriceGeneration;
using model_router::Provenance;
using model_router::ProviderGeneration;
using model_router::ProviderId;
using model_router::ReadinessState;
using model_router::ReservationEvidence;
using model_router::ReservationGeneration;
using model_router::ReservationId;
using model_router::ResidencyState;
using model_router::RouteOutcome;
using model_router::RouteRejection;
using model_router::RouteRequest;
using model_router::RouteRequestGeneration;
using model_router::RouteRequestId;
using model_router::SloEvidence;
using model_router::SLOGeneration;
using model_router::SLOId;
using model_router::SloVerdict;
using model_router::TenantId;
using model_router::TrustDomain;

namespace {

constexpr TenantId kTenant(1);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);
constexpr BackendId kBackend(1);
constexpr BackendBootId kBoot(1);
constexpr BackendId kSecondBackend(2);

/// A started router with the reference catalog, an open policy, and one fully
/// evidenced backend.
struct Rig {
  mrtest::Fixture fixture;

  Rig() {
    fixture.start();
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    fixture.add_backend(kBackend, kBoot.value());
  }

  [[nodiscard]] model_router::ModelRouter& router() { return *fixture.router; }
  [[nodiscard]] RouteRequest request() const {
    return mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  }
  [[nodiscard]] model_router::UnixMillis now() const { return fixture.now(); }
};

[[nodiscard]] CapabilityRequirement require(std::string key, CapabilityState minimum) {
  CapabilityRequirement requirement;
  requirement.key = CapabilityKey(std::move(key));
  requirement.minimum_state = minimum;
  return requirement;
}

[[nodiscard]] const RouteRejection* rejection_for(const RouteOutcome& outcome, ModelId model_id) {
  for (const RouteRejection& rejection : outcome.route.rejections) {
    if (rejection.model_id == model_id.value()) {
      return &rejection;
    }
  }
  return nullptr;
}

void expect_rejected(const RouteOutcome& outcome, ModelId model_id, OutcomeCode code) {
  const RouteRejection* rejection = rejection_for(outcome, model_id);
  MR_CHECK(rejection != nullptr);
  if (rejection != nullptr) {
    MR_CHECK_EQ(rejection->code, code);
  }
}

/// Every discovered candidate was rejected for the same typed reason.
void expect_all_rejected(const RouteOutcome& outcome, OutcomeCode code) {
  MR_CHECK_EQ(outcome.code, OutcomeCode::NO_ELIGIBLE_CANDIDATE);
  MR_CHECK(!outcome.has_decision);
  MR_CHECK(!outcome.route.rejections.empty());
  for (const RouteRejection& rejection : outcome.route.rejections) {
    MR_CHECK_EQ(rejection.code, code);
  }
}

[[nodiscard]] BackendDescriptor make_incarnation(const mrtest::Fixture& fixture, BackendId backend_id,
                                                 BackendBootId boot,
                                                 BackendRegistrationGeneration registration) {
  return model_router::reference::make_backend(
      fixture.catalog, backend_id, BackendGeneration(1), boot, registration,
      "127.0.0.1:" + std::to_string(7000 + backend_id.value()), Provenance::SYNTHETIC,
      fixture.now());
}

/// Registers one extra backend incarnation and publishes current evidence.
void add_backend(mrtest::Fixture& fixture, const BackendDescriptor& backend) {
  const BackendId backend_id = backend.backend_id;
  const BackendBootId boot = backend.backend_boot;
  MR_CHECK(fixture.router->register_backend(backend).accepted());
  fixture.publish_current(backend_id, boot);
}

[[nodiscard]] BackendCapabilityPublication make_publication(BackendId backend_id, BackendBootId boot,
                                                            CapabilityGeneration generation,
                                                            std::string key, CapabilityState state,
                                                            model_router::UnixMillis observed,
                                                            model_router::UnixMillis expires =
                                                                model_router::kNoExpiry) {
  BackendCapabilityPublication publication;
  publication.backend_id = backend_id;
  publication.backend_generation = BackendGeneration(1);
  publication.backend_boot = boot;
  publication.profile_id = CapabilityProfileId(backend_id.value() * 100 + 2);
  publication.generation = generation;
  CapabilityEvidence evidence;
  evidence.key = CapabilityKey(std::move(key));
  evidence.state = state;
  evidence.observed_at_unix_millis = observed;
  evidence.expires_at_unix_millis = expires;
  evidence.source = "eligibility-test";
  publication.claims.push_back(std::move(evidence));
  return publication;
}

[[nodiscard]] CostEvidence make_cost(BackendId backend_id, ModelId model_id,
                                     PriceGeneration price_generation, std::int64_t total,
                                     model_router::UnixMillis effective,
                                     model_router::UnixMillis expires) {
  CostEvidence evidence;
  evidence.evidence_id = CostEvidenceId(backend_id.value() * 1000 + model_id.value());
  evidence.price_generation = price_generation;
  evidence.backend_id = backend_id;
  evidence.model_id = model_id;
  evidence.unit = model_router::reference::reference_cost_unit();
  evidence.input_micros_per_unit = total;
  evidence.output_micros_per_unit = total;
  evidence.estimated_total_micros = total;
  evidence.knownness = CostKnownness::QUOTED;
  evidence.effective_at_unix_millis = effective;
  evidence.expires_at_unix_millis = expires;
  evidence.source = "eligibility-test";
  return evidence;
}

/// Publishes one price for every model bound by the named backend.
void publish_costs(mrtest::Fixture& fixture, BackendId backend_id, std::int64_t total,
                   model_router::UnixMillis expires) {
  const ModelId models[] = {fixture.catalog.small_model, fixture.catalog.general_model,
                            fixture.catalog.specialist_model};
  for (const ModelId model_id : models) {
    MR_CHECK(fixture.router
                 ->set_cost(make_cost(backend_id, model_id, PriceGeneration(1), total, fixture.now(),
                                      expires))
                 .accepted());
  }
}

/// A direct eligibility input with one fully current candidate.
struct DirectCase {
  RouteRequest request{};
  model_router::RouteCandidate candidate{};
  PolicySnapshot policy{};
  BudgetSnapshot budget{};
  model_router::detail::CandidateEvidence evidence{};
  model_router::UnixMillis now{1000};
};

[[nodiscard]] DirectCase make_direct_case() {
  DirectCase direct;
  direct.request.request_id = RouteRequestId(1);
  direct.request.request_generation = RouteRequestGeneration(1);
  direct.request.tenant = kTenant;
  direct.request.name_space = model_router::NamespaceId(1);
  direct.request.policy_id = kPolicy;
  direct.request.policy_generation = kPolicyGeneration;
  direct.request.created_at_unix_millis = direct.now;
  direct.request.requirements.required_input_modalities =
      ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  direct.request.requirements.required_output_modalities =
      ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  direct.policy = model_router::PolicyBuilder(kPolicy, kPolicyGeneration).build();

  ModelDescriptor model;
  model.model_id = ModelId(1);
  model.model_generation = ModelGeneration(1);
  model.model_version_id = ModelVersionId(1);
  model.artifact_generation = ArtifactGeneration(1);
  model.family_id = ModelFamilyId(1);
  model.context_limit_tokens = 100000;
  model.max_output_tokens = 10000;
  model.input_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  model.output_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  model.quality_class = 50;
  model.lifecycle = ModelLifecycle::CURRENT;
  direct.candidate.model = model;

  BackendDescriptor backend;
  backend.backend_id = kBackend;
  backend.backend_generation = BackendGeneration(1);
  backend.backend_boot = kBoot;
  backend.backend_registration_generation = BackendRegistrationGeneration(1);
  backend.provider_id = ProviderId(1);
  backend.provider_generation = ProviderGeneration(1);
  backend.endpoint.endpoint_id = EndpointId(1);
  backend.endpoint.endpoint_generation = model_router::EndpointGeneration(1);
  backend.endpoint.backend_id = kBackend;
  backend.endpoint.backend_boot = kBoot;
  backend.endpoint.reference = "127.0.0.1:7001";
  backend.trust_profile_id = model_router::TrustProfileId(1);
  backend.trust_generation = model_router::TrustGeneration(1);
  backend.trust_domain = TrustDomain::LOCAL;
  backend.locality = LocalityKey("local/loopback");
  backend.offline_capable = true;
  ModelBinding binding;
  binding.model_id = ModelId(1);
  binding.model_generation = ModelGeneration(1);
  binding.artifact_generation = ArtifactGeneration(1);
  binding.context_limit_tokens = 100000;
  binding.quality_class = 50;
  backend.model_bindings.push_back(std::move(binding));
  backend.health.state = HealthState::HEALTHY;
  backend.health.generation = model_router::HealthGeneration(1);
  backend.health.backend_boot = kBoot;
  backend.availability.state = AvailabilityState::AVAILABLE;
  backend.availability.generation = model_router::AvailabilityGeneration(1);
  backend.availability.backend_boot = kBoot;
  backend.readiness.state = ReadinessState::READY;
  backend.readiness.generation = model_router::ReadinessGeneration(1);
  backend.readiness.backend_boot = kBoot;
  backend.residency.state = ResidencyState::RESIDENT;
  backend.residency.generation = model_router::ResidencyGeneration(1);
  backend.residency.backend_boot = kBoot;
  backend.capacity.state = CapacityState::AVAILABLE;
  backend.capacity.generation = model_router::CapacityGeneration(1);
  backend.capacity.backend_boot = kBoot;
  backend.canonicalize();
  direct.candidate.backend = backend;
  direct.candidate.endpoint = backend.endpoint;

  direct.candidate.key.model_id = ModelId(1);
  direct.candidate.key.model_generation = ModelGeneration(1);
  direct.candidate.key.artifact_generation = ArtifactGeneration(1);
  direct.candidate.key.backend_id = kBackend;
  direct.candidate.key.backend_generation = BackendGeneration(1);
  direct.candidate.key.backend_boot = kBoot;
  direct.candidate.key.endpoint_id = EndpointId(1);
  direct.candidate.key.endpoint_generation = model_router::EndpointGeneration(1);
  direct.candidate.context_limit_tokens = 100000;
  direct.candidate.quality_class = 50;
  return direct;
}

[[nodiscard]] model_router::detail::EligibilityVerdict evaluate(const DirectCase& direct) {
  model_router::detail::EligibilityInput input;
  input.request = &direct.request;
  input.candidate = &direct.candidate;
  input.policy = &direct.policy;
  input.budget = &direct.budget;
  input.evidence = &direct.evidence;
  input.now = direct.now;
  return model_router::detail::evaluate_hard_eligibility(input);
}

}  // namespace

MR_TEST(eligibility, an_unconstrained_request_routes_to_the_best_ranked_candidate) {
  Rig rig;
  const RouteOutcome outcome = rig.router().route(rig.request());
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK(outcome.has_decision);
  MR_CHECK(outcome.route.rejections.empty());
  MR_CHECK_EQ(outcome.route.eligible_candidate_count, std::uint32_t{3});
  MR_CHECK_EQ(outcome.route.ranking.size(), std::size_t{3});
}

MR_TEST(eligibility, missing_required_capability_is_a_capability_rejection) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.required_capabilities.push_back(require("task/nonexistent", CapabilityState::DECLARED));
  const RouteOutcome outcome = rig.router().route(request);
  expect_all_rejected(outcome, OutcomeCode::REJECT_CAPABILITY);
  MR_CHECK_EQ(outcome.route.rejections.size(), std::size_t{3});
  MR_CHECK(outcome.route.rejections.front().detail.find("task/nonexistent") != std::string::npos);
  MR_CHECK(outcome.route.rejections.front().detail.find("no evidence") != std::string::npos);
}

MR_TEST(eligibility, capability_weaker_than_the_required_minimum_is_rejected) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.required_capabilities.push_back(
      require("output/structured", CapabilityState::VERIFIED));
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  // Only the general model proves VERIFIED structured output.
  MR_CHECK_EQ(outcome.decision.authority.model_id, rig.fixture.catalog.general_model);
  expect_rejected(outcome, rig.fixture.catalog.small_model, OutcomeCode::REJECT_CAPABILITY);
  expect_rejected(outcome, rig.fixture.catalog.specialist_model, OutcomeCode::REJECT_CAPABILITY);
  MR_CHECK(rejection_for(outcome, rig.fixture.catalog.general_model) == nullptr);
  const RouteRejection* weak = rejection_for(outcome, rig.fixture.catalog.small_model);
  MR_CHECK(weak != nullptr);
  if (weak != nullptr) {
    MR_CHECK(weak->detail.find("weaker than the required minimum evidence state") !=
             std::string::npos);
  }
}

MR_TEST(eligibility, revoked_capability_never_satisfies_a_requirement) {
  Rig rig;
  MR_CHECK(rig.router()
               .publish_capabilities(make_publication(
                   kBackend, kBoot, CapabilityGeneration(1),
                   std::string(model_router::capability_keys::cuda_execution),
                   CapabilityState::REVOKED, rig.now()))
               .accepted());
  RouteRequest request = rig.request();
  request.requirements.required_capabilities.push_back(
      require(std::string(model_router::capability_keys::cuda_execution), CapabilityState::DECLARED));
  const RouteOutcome outcome = rig.router().route(request);
  expect_all_rejected(outcome, OutcomeCode::REJECT_CAPABILITY);
  MR_CHECK(outcome.route.rejections.front().detail.find("REVOKED") != std::string::npos);
}

MR_TEST(eligibility, stale_capability_state_never_satisfies_a_requirement) {
  Rig rig;
  MR_CHECK(rig.router()
               .publish_capabilities(make_publication(
                   kBackend, kBoot, CapabilityGeneration(1),
                   std::string(model_router::capability_keys::cuda_execution),
                   CapabilityState::STALE, rig.now()))
               .accepted());
  RouteRequest request = rig.request();
  request.requirements.required_capabilities.push_back(
      require(std::string(model_router::capability_keys::cuda_execution), CapabilityState::DECLARED));
  const RouteOutcome outcome = rig.router().route(request);
  expect_all_rejected(outcome, OutcomeCode::REJECT_CAPABILITY);
  MR_CHECK(outcome.route.rejections.front().detail.find("STALE") != std::string::npos);
}

MR_TEST(eligibility, expired_capability_evidence_stops_satisfying) {
  Rig rig;
  const model_router::UnixMillis observed = rig.now();
  MR_CHECK(rig.router()
               .publish_capabilities(make_publication(
                   kBackend, kBoot, CapabilityGeneration(1),
                   std::string(model_router::capability_keys::cuda_execution),
                   CapabilityState::VERIFIED, observed, observed + 500))
               .accepted());
  RouteRequest request = rig.request();
  request.requirements.required_capabilities.push_back(
      require(std::string(model_router::capability_keys::cuda_execution), CapabilityState::VERIFIED));
  MR_CHECK_EQ(rig.router().route(request).code, OutcomeCode::ROUTED);

  rig.fixture.clock->advance(500);
  request.request_id = model_router::allocate_id<model_router::RouteRequestTag>();
  expect_all_rejected(rig.router().route(request), OutcomeCode::REJECT_CAPABILITY);
}

MR_TEST(eligibility, insufficient_context_limit_is_rejected) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.min_context_tokens = 100000;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.model_id, rig.fixture.catalog.general_model);
  expect_rejected(outcome, rig.fixture.catalog.small_model, OutcomeCode::REJECT_CONTEXT_LIMIT);
  expect_rejected(outcome, rig.fixture.catalog.specialist_model, OutcomeCode::REJECT_CONTEXT_LIMIT);
}

MR_TEST(eligibility, max_output_length_beyond_the_model_limit_is_rejected) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.min_context_tokens = 0;
  request.requirements.max_output_tokens = 10000;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.model_id, rig.fixture.catalog.general_model);
  const RouteRejection* rejection = rejection_for(outcome, rig.fixture.catalog.small_model);
  MR_CHECK(rejection != nullptr);
  if (rejection != nullptr) {
    MR_CHECK_EQ(rejection->code, OutcomeCode::REJECT_CONTEXT_LIMIT);
    MR_CHECK(rejection->detail.find("maximum output length") != std::string::npos);
  }
  expect_rejected(outcome, rig.fixture.catalog.specialist_model, OutcomeCode::REJECT_CONTEXT_LIMIT);
}

MR_TEST(eligibility, unsupported_required_input_modality_is_rejected) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.required_input_modalities =
      ModalitySet(static_cast<std::uint32_t>(Modality::IMAGE));
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.model_id, rig.fixture.catalog.specialist_model);
  const RouteRejection* rejection = rejection_for(outcome, rig.fixture.catalog.small_model);
  MR_CHECK(rejection != nullptr);
  if (rejection != nullptr) {
    MR_CHECK_EQ(rejection->code, OutcomeCode::REJECT_MODALITY);
    MR_CHECK(rejection->detail.find("input modality") != std::string::npos);
  }
  expect_rejected(outcome, rig.fixture.catalog.general_model, OutcomeCode::REJECT_MODALITY);
}

MR_TEST(eligibility, unsupported_required_output_modality_is_rejected) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.required_output_modalities =
      ModalitySet(static_cast<std::uint32_t>(Modality::IMAGE));
  const RouteOutcome outcome = rig.router().route(request);
  expect_all_rejected(outcome, OutcomeCode::REJECT_MODALITY);
  MR_CHECK(outcome.route.rejections.front().detail.find("output modality") != std::string::npos);
}

MR_TEST(eligibility, required_structured_output_is_rejected_without_proof) {
  Rig rig;
  // The bare model declares no structured-output capability at all.
  ModelDescriptor bare;
  bare.model_id = ModelId(90);
  bare.model_generation = ModelGeneration(1);
  bare.model_version_id = ModelVersionId(90);
  bare.artifact_generation = ArtifactGeneration(1);
  bare.family_id = ModelFamilyId(90);
  bare.context_limit_tokens = 4096;
  bare.max_output_tokens = 512;
  bare.input_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  bare.output_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  bare.quality_class = 10;
  bare.lifecycle = ModelLifecycle::CURRENT;
  bare.provenance = Provenance::SYNTHETIC;
  bare.registered_at_unix_millis = rig.now();
  bare.display_name = "bare-model";
  MR_CHECK(rig.router().register_model(bare).accepted());

  BackendDescriptor backend = make_incarnation(rig.fixture, kSecondBackend, kBoot,
                                               BackendRegistrationGeneration(1));
  backend.model_bindings.clear();
  ModelBinding binding;
  binding.model_id = bare.model_id;
  binding.model_generation = bare.model_generation;
  binding.artifact_generation = bare.artifact_generation;
  binding.bound_at_unix_millis = rig.now();
  backend.model_bindings.push_back(std::move(binding));
  backend.canonicalize();
  add_backend(rig.fixture, backend);

  RouteRequest request = rig.request();
  request.requirements.require_structured_output = true;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  const RouteRejection* rejection = rejection_for(outcome, bare.model_id);
  MR_CHECK(rejection != nullptr);
  if (rejection != nullptr) {
    MR_CHECK_EQ(rejection->code, OutcomeCode::REJECT_STRUCTURED_OUTPUT);
  }
  MR_CHECK(rejection_for(outcome, rig.fixture.catalog.general_model) == nullptr);
}

MR_TEST(eligibility, required_json_schema_is_rejected_without_proof) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.require_json_schema = true;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.model_id, rig.fixture.catalog.general_model);
  expect_rejected(outcome, rig.fixture.catalog.small_model, OutcomeCode::REJECT_STRUCTURED_OUTPUT);
  expect_rejected(outcome, rig.fixture.catalog.specialist_model,
                  OutcomeCode::REJECT_STRUCTURED_OUTPUT);
}

MR_TEST(eligibility, required_tool_calling_is_rejected_without_proof) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.require_tool_calling = true;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  expect_rejected(outcome, rig.fixture.catalog.small_model, OutcomeCode::REJECT_TOOL_CALLING);
  MR_CHECK(rejection_for(outcome, rig.fixture.catalog.general_model) == nullptr);
  MR_CHECK(rejection_for(outcome, rig.fixture.catalog.specialist_model) == nullptr);
}

MR_TEST(eligibility, required_streaming_is_rejected_without_proof) {
  Rig rig;
  ModelDescriptor bare;
  bare.model_id = ModelId(91);
  bare.model_generation = ModelGeneration(1);
  bare.model_version_id = ModelVersionId(91);
  bare.artifact_generation = ArtifactGeneration(1);
  bare.family_id = ModelFamilyId(91);
  bare.context_limit_tokens = 4096;
  bare.max_output_tokens = 512;
  bare.input_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  bare.output_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  bare.quality_class = 10;
  bare.lifecycle = ModelLifecycle::CURRENT;
  bare.provenance = Provenance::SYNTHETIC;
  bare.registered_at_unix_millis = rig.now();
  bare.display_name = "bare-model";
  MR_CHECK(rig.router().register_model(bare).accepted());

  BackendDescriptor backend = make_incarnation(rig.fixture, kSecondBackend, kBoot,
                                               BackendRegistrationGeneration(1));
  backend.model_bindings.clear();
  ModelBinding binding;
  binding.model_id = bare.model_id;
  binding.model_generation = bare.model_generation;
  binding.artifact_generation = bare.artifact_generation;
  binding.bound_at_unix_millis = rig.now();
  backend.model_bindings.push_back(std::move(binding));
  backend.canonicalize();
  add_backend(rig.fixture, backend);

  RouteRequest request = rig.request();
  request.requirements.require_streaming = true;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  const RouteRejection* rejection = rejection_for(outcome, bare.model_id);
  MR_CHECK(rejection != nullptr);
  if (rejection != nullptr) {
    MR_CHECK_EQ(rejection->code, OutcomeCode::REJECT_STREAMING);
  }
}

MR_TEST(eligibility, quality_below_the_floor_is_rejected) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.quality_floor = 70;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.model_id, rig.fixture.catalog.specialist_model);
  expect_rejected(outcome, rig.fixture.catalog.small_model, OutcomeCode::REJECT_QUALITY);
  expect_rejected(outcome, rig.fixture.catalog.general_model, OutcomeCode::REJECT_QUALITY);
}

MR_TEST(eligibility, unclassified_quality_never_satisfies_a_quality_floor) {
  Rig rig;
  ModelDescriptor unclassified;
  unclassified.model_id = ModelId(92);
  unclassified.model_generation = ModelGeneration(1);
  unclassified.model_version_id = ModelVersionId(92);
  unclassified.artifact_generation = ArtifactGeneration(1);
  unclassified.family_id = ModelFamilyId(92);
  unclassified.context_limit_tokens = 4096;
  unclassified.max_output_tokens = 512;
  unclassified.input_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  unclassified.output_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  unclassified.quality_class = model_router::kUnclassifiedQuality;
  unclassified.lifecycle = ModelLifecycle::CURRENT;
  unclassified.provenance = Provenance::SYNTHETIC;
  unclassified.registered_at_unix_millis = rig.now();
  unclassified.display_name = "unclassified-model";
  MR_CHECK(rig.router().register_model(unclassified).accepted());

  BackendDescriptor backend = make_incarnation(rig.fixture, kSecondBackend, kBoot,
                                               BackendRegistrationGeneration(1));
  backend.model_bindings.clear();
  ModelBinding binding;
  binding.model_id = unclassified.model_id;
  binding.model_generation = unclassified.model_generation;
  binding.artifact_generation = unclassified.artifact_generation;
  binding.bound_at_unix_millis = rig.now();
  backend.model_bindings.push_back(std::move(binding));
  backend.canonicalize();
  add_backend(rig.fixture, backend);

  RouteRequest request = rig.request();
  request.requirements.quality_floor = 10;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  const RouteRejection* rejection = rejection_for(outcome, unclassified.model_id);
  MR_CHECK(rejection != nullptr);
  if (rejection != nullptr) {
    MR_CHECK_EQ(rejection->code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
  }
}

MR_TEST(eligibility, model_allowlist_rejects_unlisted_models) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.model_allowlist.push_back(rig.fixture.catalog.specialist_model);
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.model_id, rig.fixture.catalog.specialist_model);
  expect_rejected(outcome, rig.fixture.catalog.small_model, OutcomeCode::REJECT_AFFINITY);
  expect_rejected(outcome, rig.fixture.catalog.general_model, OutcomeCode::REJECT_AFFINITY);
}

MR_TEST(eligibility, model_denylist_rejects_listed_models) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.model_denylist.push_back(rig.fixture.catalog.general_model);
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  expect_rejected(outcome, rig.fixture.catalog.general_model, OutcomeCode::REJECT_AFFINITY);
  MR_CHECK(rejection_for(outcome, rig.fixture.catalog.small_model) == nullptr);
}

MR_TEST(eligibility, family_allowlist_rejects_other_families) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.family_allowlist.push_back(ModelFamilyId(2));
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.model_id, rig.fixture.catalog.general_model);
  expect_rejected(outcome, rig.fixture.catalog.small_model, OutcomeCode::REJECT_AFFINITY);
  expect_rejected(outcome, rig.fixture.catalog.specialist_model, OutcomeCode::REJECT_AFFINITY);
}

MR_TEST(eligibility, family_denylist_rejects_listed_families) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.family_denylist.push_back(ModelFamilyId(3));
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  expect_rejected(outcome, rig.fixture.catalog.specialist_model, OutcomeCode::REJECT_AFFINITY);
  MR_CHECK(rejection_for(outcome, rig.fixture.catalog.general_model) == nullptr);
}

MR_TEST(eligibility, provider_allowlist_rejects_unlisted_providers) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.provider_allowlist.push_back(ProviderId(2));
  expect_all_rejected(rig.router().route(request), OutcomeCode::REJECT_AFFINITY);

  request.requirements.provider_allowlist.clear();
  request.requirements.provider_allowlist.push_back(rig.fixture.catalog.provider);
  MR_CHECK_EQ(rig.router().route(request).code, OutcomeCode::ROUTED);
}

MR_TEST(eligibility, provider_denylist_rejects_listed_providers) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.provider_denylist.push_back(rig.fixture.catalog.provider);
  expect_all_rejected(rig.router().route(request), OutcomeCode::REJECT_AFFINITY);
}

MR_TEST(eligibility, backend_allowlist_rejects_unlisted_backends) {
  Rig rig;
  add_backend(rig.fixture,
              make_incarnation(rig.fixture, kSecondBackend, kBoot, BackendRegistrationGeneration(1)));
  RouteRequest request = rig.request();
  request.requirements.backend_allowlist.push_back(kSecondBackend);
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, kSecondBackend);
  for (const RouteRejection& rejection : outcome.route.rejections) {
    MR_CHECK_EQ(rejection.code, OutcomeCode::REJECT_AFFINITY);
    MR_CHECK_EQ(rejection.backend_id, kBackend.value());
  }
  MR_CHECK_EQ(outcome.route.rejections.size(), std::size_t{3});
}

MR_TEST(eligibility, backend_denylist_rejects_listed_backends) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.backend_denylist.push_back(kBackend);
  expect_all_rejected(rig.router().route(request), OutcomeCode::REJECT_AFFINITY);
}

MR_TEST(eligibility, local_only_rejects_remote_placement) {
  Rig rig;
  BackendDescriptor remote = make_incarnation(rig.fixture, kSecondBackend, kBoot,
                                              BackendRegistrationGeneration(1));
  remote.trust_domain = TrustDomain::PUBLIC_REMOTE;
  add_backend(rig.fixture, remote);

  RouteRequest request = rig.request();
  request.requirements.local_only = true;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, kBackend);
  MR_CHECK_EQ(outcome.route.rejections.size(), std::size_t{3});
  for (const RouteRejection& rejection : outcome.route.rejections) {
    MR_CHECK_EQ(rejection.code, OutcomeCode::REJECT_LOCALITY);
    MR_CHECK_EQ(rejection.backend_id, kSecondBackend.value());
    MR_CHECK(rejection.detail.find("local-only") != std::string::npos);
  }
}

MR_TEST(eligibility, remote_placement_forbidden_rejects_a_remote_backend) {
  Rig rig;
  BackendDescriptor remote = make_incarnation(rig.fixture, kSecondBackend, kBoot,
                                              BackendRegistrationGeneration(1));
  remote.trust_domain = TrustDomain::TRUSTED_REMOTE;
  add_backend(rig.fixture, remote);

  RouteRequest request = rig.request();
  request.requirements.offline_only = true;
  request.requirements.remote_allowed = false;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, kBackend);
  for (const RouteRejection& rejection : outcome.route.rejections) {
    MR_CHECK_EQ(rejection.code, OutcomeCode::REJECT_LOCALITY);
    MR_CHECK(rejection.detail.find("forbids remote placement") != std::string::npos);
  }
}

MR_TEST(eligibility, offline_only_rejects_a_backend_without_offline_execution) {
  Rig rig;
  BackendDescriptor online = make_incarnation(rig.fixture, kSecondBackend, kBoot,
                                              BackendRegistrationGeneration(1));
  online.offline_capable = false;
  add_backend(rig.fixture, online);

  RouteRequest request = rig.request();
  request.requirements.offline_only = true;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, kBackend);
  MR_CHECK_EQ(outcome.route.rejections.size(), std::size_t{3});
  for (const RouteRejection& rejection : outcome.route.rejections) {
    MR_CHECK_EQ(rejection.code, OutcomeCode::REJECT_LOCALITY);
    MR_CHECK(rejection.detail.find("offline-only") != std::string::npos);
  }
}

MR_TEST(eligibility, required_locality_rejects_a_backend_in_another_locality) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.required_localities.push_back(LocalityKey("eu/west"));
  expect_all_rejected(rig.router().route(request), OutcomeCode::REJECT_RESIDENCY);

  request.requirements.required_localities.clear();
  request.requirements.required_localities.push_back(rig.fixture.catalog.locality);
  MR_CHECK_EQ(rig.router().route(request).code, OutcomeCode::ROUTED);
}

MR_TEST(eligibility, denied_locality_rejects_a_backend_in_that_locality) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.denied_localities.push_back(rig.fixture.catalog.locality);
  const RouteOutcome outcome = rig.router().route(request);
  expect_all_rejected(outcome, OutcomeCode::REJECT_RESIDENCY);
  MR_CHECK(outcome.route.rejections.front().detail.find("denied") != std::string::npos);
}

MR_TEST(eligibility, minimum_trust_rejects_a_less_trusted_backend) {
  Rig rig;
  BackendDescriptor remote = make_incarnation(rig.fixture, kSecondBackend, kBoot,
                                              BackendRegistrationGeneration(1));
  remote.trust_domain = TrustDomain::PUBLIC_REMOTE;
  add_backend(rig.fixture, remote);

  RouteRequest request = rig.request();
  request.requirements.minimum_trust = TrustDomain::PRIVATE_NETWORK;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, kBackend);
  MR_CHECK_EQ(outcome.route.rejections.size(), std::size_t{3});
  for (const RouteRejection& rejection : outcome.route.rejections) {
    MR_CHECK_EQ(rejection.code, OutcomeCode::REJECT_TRUST);
  }
}

MR_TEST(eligibility, unknown_trust_never_satisfies_a_trust_floor) {
  Rig rig;
  BackendDescriptor unknown = make_incarnation(rig.fixture, kSecondBackend, kBoot,
                                               BackendRegistrationGeneration(1));
  unknown.trust_domain = TrustDomain::UNKNOWN;
  add_backend(rig.fixture, unknown);

  RouteRequest request = rig.request();
  request.requirements.minimum_trust = TrustDomain::LOCAL;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, kBackend);
  MR_CHECK_EQ(outcome.route.rejections.size(), std::size_t{3});
  for (const RouteRejection& rejection : outcome.route.rejections) {
    MR_CHECK_EQ(rejection.code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
    MR_CHECK(rejection.detail.find("trust is UNKNOWN") != std::string::npos);
  }
}

MR_TEST(eligibility, policy_denial_of_a_model_is_rejected) {
  Rig rig;
  MR_CHECK(rig.router()
               .set_policy(model_router::PolicyBuilder(kPolicy, PolicyGeneration(2))
                               .deny_model(rig.fixture.catalog.specialist_model)
                               .build())
               .accepted());
  RouteRequest request = rig.request();
  request.policy_generation = PolicyGeneration(2);
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  expect_rejected(outcome, rig.fixture.catalog.specialist_model, OutcomeCode::REJECT_POLICY);
  MR_CHECK(rejection_for(outcome, rig.fixture.catalog.small_model) == nullptr);
}

MR_TEST(eligibility, policy_denial_of_a_provider_is_rejected) {
  Rig rig;
  MR_CHECK(rig.router()
               .set_policy(model_router::PolicyBuilder(kPolicy, PolicyGeneration(2))
                               .deny_provider(rig.fixture.catalog.provider)
                               .build())
               .accepted());
  RouteRequest request = rig.request();
  request.policy_generation = PolicyGeneration(2);
  expect_all_rejected(rig.router().route(request), OutcomeCode::REJECT_POLICY);
}

MR_TEST(eligibility, policy_denial_of_a_backend_is_rejected) {
  Rig rig;
  MR_CHECK(rig.router()
               .set_policy(model_router::PolicyBuilder(kPolicy, PolicyGeneration(2))
                               .deny_backend(kBackend)
                               .build())
               .accepted());
  RouteRequest request = rig.request();
  request.policy_generation = PolicyGeneration(2);
  expect_all_rejected(rig.router().route(request), OutcomeCode::REJECT_POLICY);
}

MR_TEST(eligibility, policy_denial_of_a_locality_is_rejected) {
  Rig rig;
  MR_CHECK(rig.router()
               .set_policy(model_router::PolicyBuilder(kPolicy, PolicyGeneration(2))
                               .deny_locality(rig.fixture.catalog.locality)
                               .build())
               .accepted());
  RouteRequest request = rig.request();
  request.policy_generation = PolicyGeneration(2);
  const RouteOutcome outcome = rig.router().route(request);
  expect_all_rejected(outcome, OutcomeCode::REJECT_LOCALITY);
  MR_CHECK(outcome.route.rejections.front().detail.find("policy denies the deployment locality") !=
           std::string::npos);
}

MR_TEST(eligibility, policy_allowlist_denies_unlisted_models) {
  Rig rig;
  MR_CHECK(rig.router()
               .set_policy(model_router::PolicyBuilder(kPolicy, PolicyGeneration(2))
                               .allow_model(rig.fixture.catalog.general_model)
                               .build())
               .accepted());
  RouteRequest request = rig.request();
  request.policy_generation = PolicyGeneration(2);
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.model_id, rig.fixture.catalog.general_model);
  expect_rejected(outcome, rig.fixture.catalog.small_model, OutcomeCode::REJECT_POLICY);
  expect_rejected(outcome, rig.fixture.catalog.specialist_model, OutcomeCode::REJECT_POLICY);
  MR_CHECK(outcome.route.rejections.front().detail.find("policy allowlist") != std::string::npos);
}

MR_TEST(eligibility, budget_denial_is_a_hard_rejection) {
  Rig rig;
  BudgetSnapshot budget;
  budget.budget_id = BudgetId(1);
  budget.generation = BudgetGeneration(1);
  budget.verdict = BudgetVerdict::DENIED;
  budget.unit = model_router::reference::reference_cost_unit();
  MR_CHECK(rig.router().set_budget(std::move(budget)).accepted());
  RouteRequest request = rig.request();
  request.budget_id = BudgetId(1);
  request.budget_generation = BudgetGeneration(1);
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::REJECT_BUDGET);
  MR_CHECK(!outcome.has_decision);
  MR_CHECK(outcome.route.rejections.empty());
}

MR_TEST(eligibility, unknown_budget_never_authorizes_a_route) {
  Rig rig;
  BudgetSnapshot budget;
  budget.budget_id = BudgetId(1);
  budget.generation = BudgetGeneration(1);
  budget.verdict = BudgetVerdict::UNKNOWN;
  MR_CHECK(rig.router().set_budget(std::move(budget)).accepted());
  RouteRequest request = rig.request();
  request.budget_id = BudgetId(1);
  request.budget_generation = BudgetGeneration(1);
  MR_CHECK_EQ(rig.router().route(request).code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
}

MR_TEST(eligibility, absent_budget_evidence_never_authorizes_a_route) {
  Rig rig;
  RouteRequest request = rig.request();
  request.budget_id = BudgetId(7);
  request.budget_generation = BudgetGeneration(1);
  MR_CHECK_EQ(rig.router().route(request).code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
}

MR_TEST(eligibility, budget_generation_mismatch_is_stale) {
  Rig rig;
  BudgetSnapshot budget;
  budget.budget_id = BudgetId(1);
  budget.generation = BudgetGeneration(1);
  budget.verdict = BudgetVerdict::ALLOWED;
  budget.unit = model_router::reference::reference_cost_unit();
  MR_CHECK(rig.router().set_budget(std::move(budget)).accepted());
  RouteRequest request = rig.request();
  request.budget_id = BudgetId(1);
  request.budget_generation = BudgetGeneration(2);
  MR_CHECK_EQ(rig.router().route(request).code, OutcomeCode::REJECT_STALE_BUDGET);
}

MR_TEST(eligibility, required_cost_without_evidence_is_rejected) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.require_known_cost = true;
  expect_all_rejected(rig.router().route(request), OutcomeCode::REJECT_COST_UNKNOWN);
}

MR_TEST(eligibility, stale_cost_evidence_is_rejected) {
  Rig rig;
  publish_costs(rig.fixture, kBackend, 500, rig.now() + 500);
  RouteRequest request = rig.request();
  request.requirements.require_known_cost = true;
  MR_CHECK_EQ(rig.router().route(request).code, OutcomeCode::ROUTED);

  rig.fixture.clock->advance(500);
  request.request_id = model_router::allocate_id<model_router::RouteRequestTag>();
  expect_all_rejected(rig.router().route(request), OutcomeCode::REJECT_STALE_PRICE);
}

MR_TEST(eligibility, cost_unit_mismatch_against_the_ceiling_is_rejected) {
  Rig rig;
  publish_costs(rig.fixture, kBackend, 500, model_router::kNoExpiry);
  RouteRequest request = rig.request();
  request.requirements.enforce_cost_ceiling = true;
  request.requirements.maximum_cost.unit.currency = "EUR";
  request.requirements.maximum_cost.unit.basis = "per-million-tokens";
  request.requirements.maximum_cost.micros = 1000000;
  const RouteOutcome outcome = rig.router().route(request);
  expect_all_rejected(outcome, OutcomeCode::REJECT_COST);
  MR_CHECK(outcome.route.rejections.front().detail.find("not compatible") != std::string::npos);
}

MR_TEST(eligibility, cost_over_the_ceiling_is_rejected) {
  Rig rig;
  publish_costs(rig.fixture, kBackend, 900, model_router::kNoExpiry);
  RouteRequest request = rig.request();
  request.requirements.enforce_cost_ceiling = true;
  request.requirements.maximum_cost.unit = model_router::reference::reference_cost_unit();
  request.requirements.maximum_cost.micros = 500;
  const RouteOutcome outcome = rig.router().route(request);
  expect_all_rejected(outcome, OutcomeCode::REJECT_COST);
  MR_CHECK(outcome.route.rejections.front().detail.find("exceeds the ceiling") != std::string::npos);

  request.requirements.maximum_cost.micros = 900;
  request.request_id = model_router::allocate_id<model_router::RouteRequestTag>();
  MR_CHECK_EQ(rig.router().route(request).code, OutcomeCode::ROUTED);
}

MR_TEST(eligibility, infeasible_slo_for_a_candidate_is_rejected) {
  Rig rig;
  SloEvidence slo;
  slo.slo_id = SLOId(1);
  slo.generation = SLOGeneration(1);
  slo.verdict = SloVerdict::FEASIBLE;
  // The published latency is 1000 micros, so this target is infeasible.
  slo.latency_target_micros = 500;
  slo.observed_at_unix_millis = rig.now();
  MR_CHECK(rig.router().set_slo(std::move(slo)).accepted());

  RouteRequest request = rig.request();
  request.slo_id = SLOId(1);
  request.slo_generation = SLOGeneration(1);
  request.requirements.require_slo = true;
  const RouteOutcome outcome = rig.router().route(request);
  expect_all_rejected(outcome, OutcomeCode::REJECT_SLO);
}

MR_TEST(eligibility, unknown_candidate_slo_never_satisfies_a_required_slo) {
  Rig rig;
  SloEvidence slo;
  slo.slo_id = SLOId(1);
  slo.generation = SLOGeneration(1);
  slo.verdict = SloVerdict::FEASIBLE;
  // Without a latency target there is no per-candidate feasibility verdict.
  slo.latency_target_micros = SloEvidence::kUnspecified;
  slo.observed_at_unix_millis = rig.now();
  MR_CHECK(rig.router().set_slo(std::move(slo)).accepted());

  RouteRequest request = rig.request();
  request.slo_id = SLOId(1);
  request.slo_generation = SLOGeneration(1);
  request.requirements.require_slo = true;
  const RouteOutcome outcome = rig.router().route(request);
  expect_all_rejected(outcome, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
  MR_CHECK(outcome.route.rejections.front().detail.find("feasibility is UNKNOWN") !=
           std::string::npos);
}

MR_TEST(eligibility, slo_generation_mismatch_is_stale) {
  Rig rig;
  SloEvidence slo;
  slo.slo_id = SLOId(1);
  slo.generation = SLOGeneration(1);
  slo.verdict = SloVerdict::FEASIBLE;
  slo.latency_target_micros = 5000;
  MR_CHECK(rig.router().set_slo(std::move(slo)).accepted());
  RouteRequest request = rig.request();
  request.slo_id = SLOId(1);
  request.slo_generation = SLOGeneration(2);
  request.requirements.require_slo = true;
  MR_CHECK_EQ(rig.router().route(request).code, OutcomeCode::REJECT_STALE_SLO);
}

MR_TEST(eligibility, latency_ceiling_with_unknown_latency_is_rejected) {
  Rig rig;
  BackendDescriptor second = make_incarnation(rig.fixture, kSecondBackend, kBoot,
                                              BackendRegistrationGeneration(1));
  MR_CHECK(rig.router().register_backend(std::move(second)).accepted());
  // Every dynamic fact is published except latency.
  const model_router::UnixMillis current = rig.now();
  MR_CHECK(rig.fixture.publisher
               .health(rig.router(), kSecondBackend, kBoot, HealthState::HEALTHY, current)
               .accepted());
  MR_CHECK(rig.fixture.publisher
               .availability(rig.router(), kSecondBackend, kBoot, AvailabilityState::AVAILABLE,
                             current)
               .accepted());
  MR_CHECK(rig.fixture.publisher
               .readiness(rig.router(), kSecondBackend, kBoot, ReadinessState::READY, current)
               .accepted());
  MR_CHECK(rig.fixture.publisher
               .residency(rig.router(), kSecondBackend, kBoot, ResidencyState::RESIDENT, current)
               .accepted());
  MR_CHECK(rig.fixture.publisher
               .capacity(rig.router(), kSecondBackend, kBoot, CapacityState::AVAILABLE, 8, 16,
                         current)
               .accepted());

  RouteRequest request = rig.request();
  request.requirements.max_latency_micros = 5000;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, kBackend);
  MR_CHECK_EQ(outcome.route.rejections.size(), std::size_t{3});
  for (const RouteRejection& rejection : outcome.route.rejections) {
    MR_CHECK_EQ(rejection.code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
    MR_CHECK(rejection.detail.find("latency") != std::string::npos);
  }
}

MR_TEST(eligibility, latency_ceiling_exceeded_is_rejected) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.max_latency_micros = 500;
  const RouteOutcome outcome = rig.router().route(request);
  expect_all_rejected(outcome, OutcomeCode::REJECT_SLO);
  MR_CHECK(outcome.route.rejections.front().detail.find("exceeds the request ceiling") !=
           std::string::npos);
}

MR_TEST(eligibility, unavailable_backend_is_rejected) {
  Rig rig;
  MR_CHECK(rig.fixture.publisher
               .availability(rig.router(), kBackend, kBoot, AvailabilityState::UNAVAILABLE,
                             rig.now())
               .accepted());
  expect_all_rejected(rig.router().route(rig.request()), OutcomeCode::REJECT_UNAVAILABLE);
}

MR_TEST(eligibility, unhealthy_backend_is_rejected) {
  Rig rig;
  MR_CHECK(rig.fixture.publisher
               .health(rig.router(), kBackend, kBoot, HealthState::UNHEALTHY, rig.now())
               .accepted());
  expect_all_rejected(rig.router().route(rig.request()), OutcomeCode::REJECT_UNHEALTHY);
}

MR_TEST(eligibility, draining_backend_is_rejected) {
  Rig rig;
  MR_CHECK(rig.fixture.publisher
               .health(rig.router(), kBackend, kBoot, HealthState::DRAINING, rig.now())
               .accepted());
  expect_all_rejected(rig.router().route(rig.request()), OutcomeCode::REJECT_DRAINING);
}

MR_TEST(eligibility, retired_backend_incarnation_is_rejected) {
  Rig rig;
  MR_CHECK(rig.fixture.publisher
               .health(rig.router(), kBackend, kBoot, HealthState::RETIRED, rig.now())
               .accepted());
  expect_all_rejected(rig.router().route(rig.request()), OutcomeCode::REJECT_RETIRED);
}

MR_TEST(eligibility, not_ready_backend_is_rejected) {
  Rig rig;
  MR_CHECK(rig.fixture.publisher
               .readiness(rig.router(), kBackend, kBoot, ReadinessState::NOT_READY, rig.now())
               .accepted());
  expect_all_rejected(rig.router().route(rig.request()), OutcomeCode::REJECT_NOT_READY);
}

MR_TEST(eligibility, required_residency_rejects_a_non_resident_model) {
  Rig rig;
  MR_CHECK(rig.fixture.publisher
               .residency(rig.router(), kBackend, kBoot, ResidencyState::NOT_RESIDENT, rig.now())
               .accepted());
  RouteRequest request = rig.request();
  request.requirements.require_resident = true;
  expect_all_rejected(rig.router().route(request), OutcomeCode::REJECT_RESIDENCY);
}

MR_TEST(eligibility, required_capacity_rejects_a_saturated_backend) {
  Rig rig;
  MR_CHECK(rig.fixture.publisher
               .capacity(rig.router(), kBackend, kBoot, CapacityState::SATURATED, 0, 16, rig.now())
               .accepted());
  RouteRequest request = rig.request();
  request.requirements.require_capacity = true;
  expect_all_rejected(rig.router().route(request), OutcomeCode::REJECT_CAPACITY);
}

MR_TEST(eligibility, required_reservation_without_one_is_rejected) {
  Rig rig;
  RouteRequest request = rig.request();
  request.requirements.require_reservation = true;
  const RouteOutcome outcome = rig.router().route(request);
  expect_all_rejected(outcome, OutcomeCode::REJECT_RESERVATION);
  MR_CHECK(outcome.route.rejections.front().detail.find("absent") != std::string::npos);
}

MR_TEST(eligibility, retired_model_generation_is_rejected) {
  Rig rig;
  MR_CHECK(rig.router()
               .retire_model(rig.fixture.catalog.specialist_model,
                             rig.fixture.catalog.specialist_generation)
               .accepted());
  const RouteOutcome outcome = rig.router().route(rig.request());
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_NE(outcome.decision.authority.model_id, rig.fixture.catalog.specialist_model);
  expect_rejected(outcome, rig.fixture.catalog.specialist_model, OutcomeCode::REJECT_RETIRED);
}

MR_TEST(eligibility, missing_dynamic_evidence_never_satisfies_a_live_requirement) {
  Rig rig;
  BackendDescriptor bare = make_incarnation(rig.fixture, kSecondBackend, kBoot,
                                            BackendRegistrationGeneration(1));
  MR_CHECK(rig.router().register_backend(std::move(bare)).accepted());
  const RouteOutcome outcome = rig.router().route(rig.request());
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, kBackend);
  MR_CHECK_EQ(outcome.route.rejections.size(), std::size_t{3});
  for (const RouteRejection& rejection : outcome.route.rejections) {
    MR_CHECK_EQ(rejection.code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
    MR_CHECK_EQ(rejection.backend_id, kSecondBackend.value());
  }
}

MR_TEST(eligibility, stale_live_evidence_is_rejected_with_its_own_code) {
  Rig rig;
  const model_router::UnixMillis current = rig.now();
  MR_CHECK(rig.fixture.publisher
               .availability(rig.router(), kBackend, kBoot, AvailabilityState::AVAILABLE, current,
                             current + 100)
               .accepted());
  rig.fixture.clock->advance(100);
  expect_all_rejected(rig.router().route(rig.request()), OutcomeCode::REJECT_STALE_AVAILABILITY);

  Rig second;
  const model_router::UnixMillis second_now = second.now();
  MR_CHECK(second.fixture.publisher
               .health(second.router(), kBackend, kBoot, HealthState::HEALTHY, second_now,
                       second_now + 100)
               .accepted());
  second.fixture.clock->advance(100);
  expect_all_rejected(second.router().route(second.request()), OutcomeCode::REJECT_STALE_HEALTH);

  Rig third;
  const model_router::UnixMillis third_now = third.now();
  MR_CHECK(third.fixture.publisher
               .readiness(third.router(), kBackend, kBoot, ReadinessState::READY, third_now,
                          third_now + 100)
               .accepted());
  third.fixture.clock->advance(100);
  expect_all_rejected(third.router().route(third.request()), OutcomeCode::REJECT_STALE_READINESS);

  Rig fourth;
  const model_router::UnixMillis fourth_now = fourth.now();
  MR_CHECK(fourth.fixture.publisher
               .residency(fourth.router(), kBackend, kBoot, ResidencyState::RESIDENT, fourth_now,
                          fourth_now + 100)
               .accepted());
  fourth.fixture.clock->advance(100);
  RouteRequest resident_request = fourth.request();
  resident_request.requirements.require_resident = true;
  expect_all_rejected(fourth.router().route(resident_request),
                      OutcomeCode::REJECT_STALE_RESIDENCY);

  Rig fifth;
  const model_router::UnixMillis fifth_now = fifth.now();
  CapacityEvidence capacity;
  capacity.backend_id = kBackend;
  capacity.backend_boot = kBoot;
  capacity.generation = model_router::CapacityGeneration(9);
  capacity.state = CapacityState::AVAILABLE;
  capacity.observed_at_unix_millis = fifth_now;
  capacity.expires_at_unix_millis = fifth_now + 100;
  capacity.source = "eligibility-test";
  model_router::CapacityDetail detail;
  detail.backend_id = kBackend;
  detail.backend_boot = kBoot;
  detail.available_slots = 8;
  detail.total_slots = 16;
  detail.observed_at_unix_millis = fifth_now;
  MR_CHECK(fifth.router().update_capacity(std::move(capacity), std::move(detail)).accepted());
  fifth.fixture.clock->advance(100);
  RouteRequest capacity_request = fifth.request();
  capacity_request.requirements.require_capacity = true;
  expect_all_rejected(fifth.router().route(capacity_request),
                      OutcomeCode::REJECT_STALE_CAPACITY);
}

MR_TEST(eligibility, direct_predicates_reject_identity_and_incarnation_mismatches) {
  const DirectCase baseline = make_direct_case();
  MR_CHECK(evaluate(baseline).eligible());

  DirectCase backend_boot = baseline;
  backend_boot.candidate.key.backend_boot = BackendBootId(2);
  MR_CHECK_EQ(evaluate(backend_boot).code, OutcomeCode::REJECT_STALE_BACKEND_BOOT);

  DirectCase backend_generation = baseline;
  backend_generation.candidate.key.backend_generation = BackendGeneration(2);
  MR_CHECK_EQ(evaluate(backend_generation).code, OutcomeCode::REJECT_STALE_BACKEND_BOOT);

  DirectCase model_generation = baseline;
  model_generation.candidate.key.model_generation = ModelGeneration(2);
  MR_CHECK_EQ(evaluate(model_generation).code, OutcomeCode::REJECT_STALE_MODEL);

  DirectCase artifact_generation = baseline;
  artifact_generation.candidate.key.artifact_generation = ArtifactGeneration(2);
  MR_CHECK_EQ(evaluate(artifact_generation).code, OutcomeCode::REJECT_STALE_MODEL);

  DirectCase endpoint_generation = baseline;
  endpoint_generation.candidate.key.endpoint_generation = model_router::EndpointGeneration(2);
  MR_CHECK_EQ(evaluate(endpoint_generation).code, OutcomeCode::REJECT_STALE_ENDPOINT);

  DirectCase endpoint_identity = baseline;
  endpoint_identity.candidate.key.endpoint_id = EndpointId(2);
  MR_CHECK_EQ(evaluate(endpoint_identity).code, OutcomeCode::REJECT_STALE_ENDPOINT);

  DirectCase unbound = baseline;
  unbound.candidate.backend.model_bindings.clear();
  MR_CHECK_EQ(evaluate(unbound).code, OutcomeCode::REJECT_STALE_MODEL);

  DirectCase rebound = baseline;
  rebound.candidate.backend.model_bindings.front().model_generation = ModelGeneration(2);
  MR_CHECK_EQ(evaluate(rebound).code, OutcomeCode::REJECT_STALE_MODEL);

  DirectCase incomplete = baseline;
  incomplete.candidate.key.endpoint_id = EndpointId(0);
  MR_CHECK_EQ(evaluate(incomplete).code, OutcomeCode::REJECT_INVALID);

  DirectCase no_model = baseline;
  no_model.candidate.model.lifecycle = ModelLifecycle::RETIRED;
  MR_CHECK_EQ(evaluate(no_model).code, OutcomeCode::REJECT_RETIRED);

  DirectCase expired_model = baseline;
  expired_model.candidate.model.expires_at_unix_millis = baseline.now;
  MR_CHECK_EQ(evaluate(expired_model).code, OutcomeCode::REJECT_STALE_MODEL);

  // A missing input is never an implicit acceptance.
  model_router::detail::EligibilityInput missing;
  MR_CHECK_EQ(model_router::detail::evaluate_hard_eligibility(missing).code,
              OutcomeCode::REJECT_INVALID);
  model_router::detail::EligibilityInput partial;
  partial.request = &baseline.request;
  MR_CHECK_EQ(model_router::detail::evaluate_hard_eligibility(partial).code,
              OutcomeCode::REJECT_INVALID);
}

MR_TEST(eligibility, evidence_from_another_incarnation_is_rejected) {
  const DirectCase baseline = make_direct_case();

  DirectCase availability = baseline;
  availability.candidate.backend.availability.backend_boot = BackendBootId(2);
  MR_CHECK_EQ(evaluate(availability).code, OutcomeCode::REJECT_STALE_BACKEND_BOOT);

  DirectCase health = baseline;
  health.candidate.backend.health.backend_boot = BackendBootId(2);
  MR_CHECK_EQ(evaluate(health).code, OutcomeCode::REJECT_STALE_BACKEND_BOOT);

  DirectCase readiness = baseline;
  readiness.candidate.backend.readiness.backend_boot = BackendBootId(2);
  MR_CHECK_EQ(evaluate(readiness).code, OutcomeCode::REJECT_STALE_BACKEND_BOOT);

  DirectCase residency = baseline;
  residency.request.requirements.require_resident = true;
  residency.candidate.backend.residency.backend_boot = BackendBootId(2);
  MR_CHECK_EQ(evaluate(residency).code, OutcomeCode::REJECT_STALE_BACKEND_BOOT);

  DirectCase capacity = baseline;
  capacity.request.requirements.require_capacity = true;
  capacity.candidate.backend.capacity.backend_boot = BackendBootId(2);
  MR_CHECK_EQ(evaluate(capacity).code, OutcomeCode::REJECT_STALE_BACKEND_BOOT);

  // Evidence without a generation is UNKNOWN, never a favorable value.
  DirectCase no_generation = baseline;
  no_generation.candidate.backend.health.generation = model_router::HealthGeneration(0);
  MR_CHECK_EQ(evaluate(no_generation).code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);

  DirectCase unknown_state = baseline;
  unknown_state.candidate.backend.availability.state = AvailabilityState::UNKNOWN;
  MR_CHECK_EQ(evaluate(unknown_state).code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
}

MR_TEST(eligibility, direct_predicates_reject_unknown_or_inconsistent_external_evidence) {
  const DirectCase baseline = make_direct_case();
  MR_CHECK(evaluate(baseline).eligible());

  // A policy snapshot that is not current can never authorize a candidate.
  DirectCase stale_policy = baseline;
  stale_policy.policy.generation = PolicyGeneration(0);
  MR_CHECK_EQ(evaluate(stale_policy).code, OutcomeCode::REJECT_STALE_POLICY);

  DirectCase expired_policy = baseline;
  expired_policy.policy.expires_at_unix_millis = baseline.now;
  MR_CHECK_EQ(evaluate(expired_policy).code, OutcomeCode::REJECT_STALE_POLICY);

  // Budget: UNKNOWN, denied, stale, and mismatched are all typed.
  DirectCase unknown_budget = baseline;
  unknown_budget.request.budget_id = BudgetId(1);
  unknown_budget.request.budget_generation = BudgetGeneration(1);
  MR_CHECK_EQ(evaluate(unknown_budget).code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);

  DirectCase denied_budget = unknown_budget;
  denied_budget.budget.budget_id = BudgetId(1);
  denied_budget.budget.generation = BudgetGeneration(1);
  denied_budget.budget.verdict = BudgetVerdict::DENIED;
  MR_CHECK_EQ(evaluate(denied_budget).code, OutcomeCode::REJECT_BUDGET);

  DirectCase expired_budget = unknown_budget;
  expired_budget.budget.budget_id = BudgetId(1);
  expired_budget.budget.generation = BudgetGeneration(1);
  expired_budget.budget.verdict = BudgetVerdict::ALLOWED;
  expired_budget.budget.expires_at_unix_millis = baseline.now;
  MR_CHECK_EQ(evaluate(expired_budget).code, OutcomeCode::REJECT_STALE_BUDGET);

  DirectCase wrong_budget_identity = unknown_budget;
  wrong_budget_identity.budget.budget_id = BudgetId(2);
  wrong_budget_identity.budget.generation = BudgetGeneration(1);
  wrong_budget_identity.budget.verdict = BudgetVerdict::ALLOWED;
  MR_CHECK_EQ(evaluate(wrong_budget_identity).code, OutcomeCode::REJECT_STALE_BUDGET);

  DirectCase wrong_budget_generation = unknown_budget;
  wrong_budget_generation.budget.budget_id = BudgetId(1);
  wrong_budget_generation.budget.generation = BudgetGeneration(2);
  wrong_budget_generation.budget.verdict = BudgetVerdict::ALLOWED;
  MR_CHECK_EQ(evaluate(wrong_budget_generation).code, OutcomeCode::REJECT_STALE_BUDGET);

  // Cost: unknown, stale generation, unit mismatch, and over-ceiling.
  DirectCase unknown_cost = baseline;
  unknown_cost.request.requirements.require_known_cost = true;
  MR_CHECK_EQ(evaluate(unknown_cost).code, OutcomeCode::REJECT_COST_UNKNOWN);

  DirectCase stale_price = baseline;
  stale_price.request.requirements.require_known_cost = true;
  stale_price.evidence.cost = make_cost(kBackend, ModelId(1), PriceGeneration(0), 100, baseline.now,
                                        model_router::kNoExpiry);
  MR_CHECK_EQ(evaluate(stale_price).code, OutcomeCode::REJECT_STALE_PRICE);

  DirectCase expired_price = baseline;
  expired_price.request.requirements.require_known_cost = true;
  expired_price.evidence.cost = make_cost(kBackend, ModelId(1), PriceGeneration(1), 100,
                                          baseline.now - 100, baseline.now);
  MR_CHECK_EQ(evaluate(expired_price).code, OutcomeCode::REJECT_STALE_PRICE);

  DirectCase unitless_price = baseline;
  unitless_price.request.requirements.require_known_cost = true;
  unitless_price.evidence.cost = make_cost(kBackend, ModelId(1), PriceGeneration(1), 100,
                                           baseline.now, model_router::kNoExpiry);
  unitless_price.evidence.cost.unit = model_router::CostUnit{};
  MR_CHECK_EQ(evaluate(unitless_price).code, OutcomeCode::REJECT_STALE_PRICE);

  DirectCase ceiling = baseline;
  ceiling.request.requirements.enforce_cost_ceiling = true;
  ceiling.request.requirements.maximum_cost.unit = model_router::reference::reference_cost_unit();
  ceiling.request.requirements.maximum_cost.micros = 50;
  ceiling.evidence.cost = make_cost(kBackend, ModelId(1), PriceGeneration(1), 100, baseline.now,
                                    model_router::kNoExpiry);
  MR_CHECK_EQ(evaluate(ceiling).code, OutcomeCode::REJECT_COST);

  DirectCase ceiling_unit = ceiling;
  ceiling_unit.request.requirements.maximum_cost.unit.currency = "EUR";
  MR_CHECK_EQ(evaluate(ceiling_unit).code, OutcomeCode::REJECT_COST);

  // SLO: unknown, infeasible, identity and generation mismatch.
  DirectCase unknown_slo = baseline;
  unknown_slo.request.requirements.require_slo = true;
  unknown_slo.request.slo_id = SLOId(1);
  unknown_slo.request.slo_generation = SLOGeneration(1);
  MR_CHECK_EQ(evaluate(unknown_slo).code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);

  DirectCase infeasible_slo = unknown_slo;
  infeasible_slo.evidence.slo.slo_id = SLOId(1);
  infeasible_slo.evidence.slo.generation = SLOGeneration(1);
  infeasible_slo.evidence.slo.verdict = SloVerdict::INFEASIBLE;
  MR_CHECK_EQ(evaluate(infeasible_slo).code, OutcomeCode::REJECT_SLO);

  DirectCase wrong_slo_identity = unknown_slo;
  wrong_slo_identity.evidence.slo.slo_id = SLOId(2);
  wrong_slo_identity.evidence.slo.generation = SLOGeneration(1);
  wrong_slo_identity.evidence.slo.verdict = SloVerdict::FEASIBLE;
  MR_CHECK_EQ(evaluate(wrong_slo_identity).code, OutcomeCode::REJECT_STALE_SLO);

  DirectCase wrong_slo_generation = unknown_slo;
  wrong_slo_generation.evidence.slo.slo_id = SLOId(1);
  wrong_slo_generation.evidence.slo.generation = SLOGeneration(2);
  wrong_slo_generation.evidence.slo.verdict = SloVerdict::FEASIBLE;
  MR_CHECK_EQ(evaluate(wrong_slo_generation).code, OutcomeCode::REJECT_STALE_SLO);

  DirectCase unknown_candidate_slo = unknown_slo;
  unknown_candidate_slo.evidence.slo.slo_id = SLOId(1);
  unknown_candidate_slo.evidence.slo.generation = SLOGeneration(1);
  unknown_candidate_slo.evidence.slo.verdict = SloVerdict::FEASIBLE;
  unknown_candidate_slo.evidence.slo_verdict_known = false;
  MR_CHECK_EQ(evaluate(unknown_candidate_slo).code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);

  DirectCase infeasible_candidate_slo = unknown_candidate_slo;
  infeasible_candidate_slo.evidence.slo_verdict_known = true;
  infeasible_candidate_slo.evidence.slo_verdict = SloVerdict::INFEASIBLE;
  MR_CHECK_EQ(evaluate(infeasible_candidate_slo).code, OutcomeCode::REJECT_SLO);

  // Reservation: absent, expired, and bound to another incarnation.
  DirectCase missing_reservation = baseline;
  missing_reservation.request.requirements.require_reservation = true;
  MR_CHECK_EQ(evaluate(missing_reservation).code, OutcomeCode::REJECT_RESERVATION);

  DirectCase expired_reservation = missing_reservation;
  expired_reservation.evidence.reservation_present = true;
  expired_reservation.evidence.reservation.reservation_id = ReservationId(1);
  expired_reservation.evidence.reservation.generation = ReservationGeneration(1);
  expired_reservation.evidence.reservation.backend_id = kBackend;
  expired_reservation.evidence.reservation.backend_boot = kBoot;
  expired_reservation.evidence.reservation.expires_at_unix_millis = baseline.now;
  MR_CHECK_EQ(evaluate(expired_reservation).code, OutcomeCode::REJECT_STALE_RESERVATION);

  DirectCase foreign_reservation = missing_reservation;
  foreign_reservation.evidence.reservation_present = true;
  foreign_reservation.evidence.reservation.reservation_id = ReservationId(1);
  foreign_reservation.evidence.reservation.generation = ReservationGeneration(1);
  foreign_reservation.evidence.reservation.backend_id = kSecondBackend;
  foreign_reservation.evidence.reservation.backend_boot = kBoot;
  MR_CHECK_EQ(evaluate(foreign_reservation).code, OutcomeCode::REJECT_RESERVATION);

  DirectCase live_reservation = missing_reservation;
  live_reservation.evidence.reservation_present = true;
  live_reservation.evidence.reservation.reservation_id = ReservationId(1);
  live_reservation.evidence.reservation.generation = ReservationGeneration(1);
  live_reservation.evidence.reservation.backend_id = kBackend;
  live_reservation.evidence.reservation.backend_boot = kBoot;
  live_reservation.evidence.reservation.request_id = baseline.request.request_id;
  MR_CHECK(evaluate(live_reservation).eligible());

  // Latency: unknown, stale, and over the ceiling.
  DirectCase unknown_latency = baseline;
  unknown_latency.request.requirements.require_known_latency = true;
  MR_CHECK_EQ(evaluate(unknown_latency).code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);

  DirectCase unknown_ceiling_latency = baseline;
  unknown_ceiling_latency.request.requirements.max_latency_micros = 5000;
  MR_CHECK_EQ(evaluate(unknown_ceiling_latency).code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);

  DirectCase stale_latency = unknown_ceiling_latency;
  stale_latency.candidate.backend.latency.dispatch_micros = 1000;
  stale_latency.candidate.backend.latency.expires_at_unix_millis = baseline.now;
  MR_CHECK_EQ(evaluate(stale_latency).code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);

  DirectCase fast_enough = unknown_ceiling_latency;
  fast_enough.candidate.backend.latency.dispatch_micros = 1000;
  MR_CHECK(evaluate(fast_enough).eligible());

  DirectCase slow = unknown_ceiling_latency;
  slow.candidate.backend.latency.dispatch_micros = 10000;
  MR_CHECK_EQ(evaluate(slow).code, OutcomeCode::REJECT_SLO);

  // Compatibility: required but unknown, and explicitly incompatible.
  DirectCase unknown_compatibility = baseline;
  unknown_compatibility.request.requirements.require_known_compatibility = true;
  MR_CHECK_EQ(evaluate(unknown_compatibility).code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);

  DirectCase incompatible = baseline;
  incompatible.evidence.compatibility.generation = CompatibilityGeneration(1);
  incompatible.evidence.compatibility.profile_id = CompatibilityProfileId(1);
  incompatible.evidence.compatibility.backend_id = kBackend;
  incompatible.evidence.compatibility.verdict = CompatibilityVerdict::INCOMPATIBLE;
  MR_CHECK_EQ(evaluate(incompatible).code, OutcomeCode::REJECT_COMPATIBILITY);

  DirectCase protocol_mismatch = baseline;
  protocol_mismatch.request.requirements.required_protocol = "model_router.reference";
  protocol_mismatch.evidence.compatibility.generation = CompatibilityGeneration(1);
  protocol_mismatch.evidence.compatibility.profile_id = CompatibilityProfileId(1);
  protocol_mismatch.evidence.compatibility.backend_id = kBackend;
  protocol_mismatch.evidence.compatibility.verdict = CompatibilityVerdict::COMPATIBLE;
  protocol_mismatch.evidence.compatibility.protocol = "other.protocol";
  MR_CHECK_EQ(evaluate(protocol_mismatch).code, OutcomeCode::REJECT_COMPATIBILITY);

  DirectCase protocol_match = protocol_mismatch;
  protocol_match.evidence.compatibility.protocol = "model_router.reference";
  MR_CHECK(evaluate(protocol_match).eligible());
}

MR_TEST(eligibility, rejections_are_typed_and_in_canonical_order) {
  Rig rig;
  add_backend(rig.fixture,
              make_incarnation(rig.fixture, kSecondBackend, kBoot, BackendRegistrationGeneration(1)));
  MR_CHECK(rig.fixture.publisher
               .health(rig.router(), kSecondBackend, kBoot, HealthState::UNHEALTHY, rig.now())
               .accepted());

  RouteRequest request = rig.request();
  request.requirements.required_capabilities.push_back(
      require(std::string(model_router::capability_keys::code), CapabilityState::DECLARED));
  request.requirements.quality_floor = 70;
  const RouteOutcome outcome = rig.router().route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, kBackend);
  MR_CHECK_EQ(outcome.decision.authority.model_id, rig.fixture.catalog.specialist_model);
  MR_CHECK_EQ(outcome.route.rejections.size(), std::size_t{5});

  // The rejection list is canonical: it never depends on discovery order.
  MR_CHECK(std::is_sorted(outcome.route.rejections.begin(), outcome.route.rejections.end(),
                          [](const RouteRejection& lhs, const RouteRejection& rhs) {
                            return lhs.canonical_less(rhs);
                          }));
  std::size_t capability = 0;
  std::size_t unhealthy = 0;
  for (const RouteRejection& rejection : outcome.route.rejections) {
    MR_CHECK(model_router::is_rejection(rejection.code));
    MR_CHECK(model_router::is_hard_rejection(rejection.code));
    MR_CHECK(!rejection.detail.empty());
    MR_CHECK(rejection.model_generation.valid());
    MR_CHECK(rejection.backend_generation.valid());
    MR_CHECK(rejection.backend_boot.valid());
    MR_CHECK_NE(rejection.endpoint_id, std::uint64_t{0});
    if (rejection.code == OutcomeCode::REJECT_CAPABILITY) {
      ++capability;
    }
    if (rejection.code == OutcomeCode::REJECT_UNHEALTHY) {
      ++unhealthy;
    }
  }
  // Two models lack the capability on the healthy backend; the whole unhealthy
  // backend fails the health predicate before capability is even considered.
  MR_CHECK_EQ(capability, std::size_t{2});
  MR_CHECK_EQ(unhealthy, std::size_t{3});
  // Each rejection names the candidate it belongs to.
  for (const RouteRejection& rejection : outcome.route.rejections) {
    bool found = false;
    for (const model_router::RankedCandidate& ranked : outcome.route.ranking) {
      if (ranked.key.model_id.value() == rejection.model_id &&
          ranked.key.backend_id.value() == rejection.backend_id) {
        found = true;
      }
    }
    MR_CHECK(!found);
  }
}
