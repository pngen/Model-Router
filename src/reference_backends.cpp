// Model Router - deterministic SYNTHETIC reference model profiles.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/reference_profiles.hpp"

#include <algorithm>

#include "model_router/version.hpp"

namespace model_router::reference {
namespace {

[[nodiscard]] CapabilityEvidence claim(std::string_view key, CapabilityState state,
                                       CapabilityProfileId profile_id,
                                       CapabilityGeneration generation, UnixMillis now,
                                       std::string source) {
  CapabilityEvidence evidence;
  evidence.key = CapabilityKey(std::string(key));
  evidence.state = state;
  evidence.generation = generation;
  evidence.profile_id = profile_id;
  evidence.observed_at_unix_millis = now;
  evidence.expires_at_unix_millis = kNoExpiry;
  evidence.source = std::move(source);
  return evidence;
}

void add_claim(CapabilityProfile* profile, std::string_view key, CapabilityState state,
               CapabilityProfileId profile_id, CapabilityGeneration generation, UnixMillis now,
               std::string source) {
  profile->set(claim(key, state, profile_id, generation, now, std::move(source)), 256);
}

}  // namespace

Catalog make_catalog() {
  Catalog catalog;
  catalog.provider = ProviderId(1);
  catalog.provider_generation = ProviderGeneration(1);

  catalog.small_model = ModelId(1);
  catalog.small_generation = ModelGeneration(1);
  catalog.small_artifact = ArtifactGeneration(1);

  catalog.general_model = ModelId(2);
  catalog.general_generation = ModelGeneration(1);
  catalog.general_artifact = ArtifactGeneration(1);

  catalog.specialist_model = ModelId(3);
  catalog.specialist_generation = ModelGeneration(1);
  catalog.specialist_artifact = ArtifactGeneration(1);

  catalog.locality = LocalityKey("local/loopback");
  return catalog;
}

std::vector<ModelDescriptor> make_models(const Catalog& catalog, UnixMillis now) {
  std::vector<ModelDescriptor> models;

  // Model Small: cheap, low latency, lower capability profile.
  {
    ModelDescriptor model;
    model.model_id = catalog.small_model;
    model.model_generation = catalog.small_generation;
    model.model_version_id = ModelVersionId(1);
    model.artifact_generation = catalog.small_artifact;
    model.family_id = ModelFamilyId(1);
    model.context_limit_tokens = 32768;
    model.max_output_tokens = 4096;
    model.input_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
    model.output_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
    model.quality_class = 20;
    model.lifecycle = ModelLifecycle::CURRENT;
    model.capability_profile_id = CapabilityProfileId(1);
    model.capability_generation = CapabilityGeneration(1);
    model.provenance = Provenance::SYNTHETIC;
    model.registered_at_unix_millis = now;
    model.display_name = "reference-small";
    add_claim(&model.capabilities, capability_keys::text_generation, CapabilityState::VERIFIED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::streaming, CapabilityState::DECLARED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::structured_output, CapabilityState::DECLARED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::local_execution, CapabilityState::VERIFIED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    models.push_back(std::move(model));
  }

  // Model General: broader capability, medium cost.
  {
    ModelDescriptor model;
    model.model_id = catalog.general_model;
    model.model_generation = catalog.general_generation;
    model.model_version_id = ModelVersionId(2);
    model.artifact_generation = catalog.general_artifact;
    model.family_id = ModelFamilyId(2);
    model.context_limit_tokens = 131072;
    model.max_output_tokens = 16384;
    model.input_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
    model.output_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
    model.quality_class = 60;
    model.lifecycle = ModelLifecycle::CURRENT;
    model.capability_profile_id = CapabilityProfileId(2);
    model.capability_generation = CapabilityGeneration(1);
    model.provenance = Provenance::SYNTHETIC;
    model.registered_at_unix_millis = now;
    model.display_name = "reference-general";
    add_claim(&model.capabilities, capability_keys::text_generation, CapabilityState::VERIFIED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::streaming, CapabilityState::VERIFIED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::structured_output, CapabilityState::VERIFIED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::json_schema, CapabilityState::VERIFIED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::tool_calling, CapabilityState::VERIFIED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::reasoning, CapabilityState::DECLARED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::long_context, CapabilityState::DECLARED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::local_execution, CapabilityState::VERIFIED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    models.push_back(std::move(model));
  }

  // Model Specialist: a specific capability (code and vision input).
  {
    ModelDescriptor model;
    model.model_id = catalog.specialist_model;
    model.model_generation = catalog.specialist_generation;
    model.model_version_id = ModelVersionId(3);
    model.artifact_generation = catalog.specialist_artifact;
    model.family_id = ModelFamilyId(3);
    model.context_limit_tokens = 65536;
    model.max_output_tokens = 8192;
    model.input_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT) |
                                         static_cast<std::uint32_t>(Modality::IMAGE));
    model.output_modalities = ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
    model.quality_class = 80;
    model.lifecycle = ModelLifecycle::CURRENT;
    model.capability_profile_id = CapabilityProfileId(3);
    model.capability_generation = CapabilityGeneration(1);
    model.provenance = Provenance::SYNTHETIC;
    model.registered_at_unix_millis = now;
    model.display_name = "reference-specialist";
    add_claim(&model.capabilities, capability_keys::text_generation, CapabilityState::VERIFIED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::code, CapabilityState::VERIFIED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::vision_input, CapabilityState::VERIFIED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::multimodal, CapabilityState::DECLARED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::streaming, CapabilityState::DECLARED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::structured_output, CapabilityState::OBSERVED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::tool_calling, CapabilityState::DECLARED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    add_claim(&model.capabilities, capability_keys::local_execution, CapabilityState::VERIFIED,
              model.capability_profile_id, model.capability_generation, now, "reference-profile");
    models.push_back(std::move(model));
  }

  std::sort(models.begin(), models.end(), [](const ModelDescriptor& lhs, const ModelDescriptor& rhs) {
    return lhs.model_id < rhs.model_id;
  });
  return models;
}

BackendDescriptor make_backend(const Catalog& catalog, BackendId backend_id,
                               BackendGeneration backend_generation, BackendBootId backend_boot,
                               BackendRegistrationGeneration registration,
                               std::string endpoint_reference, Provenance provenance,
                               UnixMillis now) {
  BackendDescriptor backend;
  backend.backend_id = backend_id;
  backend.backend_generation = backend_generation;
  backend.backend_boot = backend_boot;
  backend.backend_registration_generation = registration;
  backend.provider_id = catalog.provider;
  backend.provider_generation = catalog.provider_generation;
  backend.endpoint.endpoint_id = EndpointId(backend_id.value() * 100 + 1);
  backend.endpoint.endpoint_generation = EndpointGeneration(1);
  backend.endpoint.backend_id = backend_id;
  backend.endpoint.backend_boot = backend_boot;
  backend.endpoint.reference = std::move(endpoint_reference);
  backend.endpoint.protocol = "model_router.reference";
  backend.endpoint.protocol_version = protocol_version;
  backend.trust_profile_id = TrustProfileId(1);
  backend.trust_generation = TrustGeneration(1);
  backend.trust_domain = TrustDomain::LOCAL;
  backend.locality = catalog.locality;
  backend.network_distance = 1;
  // The profile identities are assigned here; the evidence generations stay
  // unset until the backend incarnation actually publishes current evidence.
  backend.capability_profile_id = CapabilityProfileId(backend_id.value() * 100 + 2);
  backend.compatibility_profile_id = CompatibilityProfileId(backend_id.value() * 100 + 3);
  backend.offline_capable = true;
  backend.provenance = provenance;
  backend.registered_at_unix_millis = now;

  const auto add_binding = [&backend, &catalog, now](ModelId model_id,
                                                     ModelGeneration model_generation,
                                                     ArtifactGeneration artifact_generation,
                                                     std::uint32_t context_limit,
                                                     QualityClass quality) {
    ModelBinding binding;
    binding.model_id = model_id;
    binding.model_generation = model_generation;
    binding.artifact_generation = artifact_generation;
    binding.context_limit_tokens = context_limit;
    binding.quality_class = quality;
    binding.bound_at_unix_millis = now;
    backend.model_bindings.push_back(std::move(binding));
  };
  add_binding(catalog.small_model, catalog.small_generation, catalog.small_artifact, 32768, 20);
  add_binding(catalog.general_model, catalog.general_generation, catalog.general_artifact, 131072,
              60);
  add_binding(catalog.specialist_model, catalog.specialist_generation, catalog.specialist_artifact,
              65536, 80);
  backend.canonicalize();
  return backend;
}

CostUnit reference_cost_unit() {
  CostUnit unit;
  unit.currency = "USD";
  unit.basis = "per-million-tokens";
  return unit;
}

CostEvidence make_cost_evidence(BackendId backend_id, ModelId model_id,
                                PriceGeneration price_generation,
                                std::int64_t input_micros_per_unit,
                                std::int64_t output_micros_per_unit,
                                std::int64_t estimated_total_micros, UnixMillis now) {
  CostEvidence evidence;
  evidence.evidence_id = CostEvidenceId(backend_id.value() * 1000 + model_id.value());
  evidence.price_generation = price_generation;
  evidence.backend_id = backend_id;
  evidence.model_id = model_id;
  evidence.unit = reference_cost_unit();
  evidence.input_micros_per_unit = input_micros_per_unit;
  evidence.output_micros_per_unit = output_micros_per_unit;
  evidence.request_minimum_micros = 0;
  evidence.estimated_total_micros = estimated_total_micros;
  evidence.knownness = CostKnownness::QUOTED;
  evidence.effective_at_unix_millis = now;
  evidence.expires_at_unix_millis = kNoExpiry;
  evidence.source = "reference-price-table";
  return evidence;
}

}  // namespace model_router::reference
