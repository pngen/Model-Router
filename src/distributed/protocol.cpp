// Model Router - reference framed wire protocol.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Every length is written before the bytes it describes and validated against
// ResourceLimits before allocation. A frame whose magic, version, type, header
// checksum, or payload checksum does not match is rejected before any payload
// field is trusted.

#include "model_router/distributed/protocol.hpp"

#include <array>
#include <cstring>
#include <type_traits>

#include "model_router/detail/crc32c.hpp"
#include "model_router/version.hpp"

namespace model_router::distributed {
namespace {

constexpr std::array<std::string_view, static_cast<std::size_t>(MessageType::kCount)> kTypeNames = {
    "INVALID",         "HELLO",           "HELLO_ACK",       "BACKEND_REGISTER",
    "BACKEND_REGISTER_ACK", "CAPABILITY_PUBLISH", "HEALTH_UPDATE", "AVAILABILITY_UPDATE",
    "READINESS_UPDATE", "RESIDENCY_UPDATE", "CAPACITY_UPDATE", "LATENCY_UPDATE",
    "COST_UPDATE",     "ROUTE_REQUEST",   "ROUTE_RESPONSE",  "DISPATCH",
    "DISPATCH_RESULT", "COMPLETION",      "FENCE",           "PING",
    "PONG",            "BYE",             "ERROR_MESSAGE",   "SNAPSHOT_REQUEST",
    "SNAPSHOT_RESPONSE", "ADMIN_REQUEST", "ADMIN_RESPONSE"};

void put_header(std::vector<std::uint8_t>* out, const FrameHeader& header) {
  out->clear();
  out->reserve(frame_header_bytes);
  const auto put_u16 = [out](std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  };
  const auto put_u32 = [out](std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  };
  put_u32(header.magic);
  put_u16(header.protocol_version);
  put_u16(header.message_type);
  put_u32(header.flags);
  put_u32(header.payload_bytes);
  put_u32(header.header_crc32c);
  put_u32(header.payload_crc32c);
}

[[nodiscard]] std::uint32_t get_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(bytes[offset + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return value;
}

[[nodiscard]] std::uint16_t get_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset] |
                                    (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

void put_profile(PayloadWriter* writer, const CapabilityProfile& profile) {
  writer->put_id(profile.id());
  writer->put_generation(profile.generation());
  writer->put_u32(static_cast<std::uint32_t>(profile.entries().size()));
  for (const CapabilityEvidence& entry : profile.entries()) {
    writer->put_string(entry.key.value());
    writer->put_u8(static_cast<std::uint8_t>(entry.state));
    writer->put_generation(entry.generation);
    writer->put_id(entry.profile_id);
    writer->put_generation(entry.model_generation);
    writer->put_generation(entry.backend_generation);
    writer->put_generation(entry.backend_boot);
    writer->put_i64(entry.observed_at_unix_millis);
    writer->put_i64(entry.expires_at_unix_millis);
    writer->put_string(entry.source);
    writer->put_string(entry.detail);
  }
}

[[nodiscard]] bool get_profile(PayloadReader* reader, const ResourceLimits& limits,
                               CapabilityProfile* profile) {
  const CapabilityProfileId id = reader->get_id<CapabilityProfileId>();
  const CapabilityGeneration generation = reader->get_generation<CapabilityGeneration>();
  const std::uint32_t count = reader->get_u32();
  if (!reader->ok() || count > limits.max_capability_entries) {
    return false;
  }
  CapabilityProfile parsed(id, generation);
  for (std::uint32_t index = 0; index < count; ++index) {
    CapabilityEvidence entry;
    entry.key = CapabilityKey(reader->get_string());
    entry.state = static_cast<CapabilityState>(reader->get_u8());
    entry.generation = reader->get_generation<CapabilityGeneration>();
    entry.profile_id = reader->get_id<CapabilityProfileId>();
    entry.model_generation = reader->get_generation<ModelGeneration>();
    entry.backend_generation = reader->get_generation<BackendGeneration>();
    entry.backend_boot = reader->get_generation<BackendBootId>();
    entry.observed_at_unix_millis = reader->get_i64();
    entry.expires_at_unix_millis = reader->get_i64();
    entry.source = reader->get_string();
    entry.detail = reader->get_string();
    if (!reader->ok() ||
        static_cast<std::uint8_t>(entry.state) >= static_cast<std::uint8_t>(CapabilityState::kCount)) {
      return false;
    }
    if (!parsed.set(std::move(entry), limits.max_capability_entries)) {
      return false;
    }
  }
  *profile = std::move(parsed);
  return true;
}

void put_cost(PayloadWriter* writer, const CostEvidence& cost) {
  writer->put_id(cost.evidence_id);
  writer->put_generation(cost.price_generation);
  writer->put_id(cost.backend_id);
  writer->put_id(cost.model_id);
  writer->put_string(cost.unit.currency);
  writer->put_string(cost.unit.basis);
  writer->put_i64(cost.input_micros_per_unit);
  writer->put_i64(cost.output_micros_per_unit);
  writer->put_i64(cost.request_minimum_micros);
  writer->put_i64(cost.cache_read_micros_per_unit);
  writer->put_i64(cost.cache_write_micros_per_unit);
  writer->put_i64(cost.estimated_total_micros);
  writer->put_u8(static_cast<std::uint8_t>(cost.knownness));
  writer->put_i64(cost.effective_at_unix_millis);
  writer->put_i64(cost.expires_at_unix_millis);
  writer->put_string(cost.source);
}

[[nodiscard]] bool get_cost(PayloadReader* reader, CostEvidence* cost) {
  cost->evidence_id = reader->get_id<CostEvidenceId>();
  cost->price_generation = reader->get_generation<PriceGeneration>();
  cost->backend_id = reader->get_id<BackendId>();
  cost->model_id = reader->get_id<ModelId>();
  cost->unit.currency = reader->get_string();
  cost->unit.basis = reader->get_string();
  cost->input_micros_per_unit = reader->get_i64();
  cost->output_micros_per_unit = reader->get_i64();
  cost->request_minimum_micros = reader->get_i64();
  cost->cache_read_micros_per_unit = reader->get_i64();
  cost->cache_write_micros_per_unit = reader->get_i64();
  cost->estimated_total_micros = reader->get_i64();
  cost->knownness = static_cast<CostKnownness>(reader->get_u8());
  cost->effective_at_unix_millis = reader->get_i64();
  cost->expires_at_unix_millis = reader->get_i64();
  cost->source = reader->get_string();
  return reader->ok() &&
         static_cast<std::uint8_t>(cost->knownness) <
             static_cast<std::uint8_t>(CostKnownness::kCount);
}

void put_backend(PayloadWriter* writer, const BackendDescriptor& backend) {
  writer->put_id(backend.backend_id);
  writer->put_generation(backend.backend_generation);
  writer->put_generation(backend.backend_boot);
  writer->put_generation(backend.backend_registration_generation);
  writer->put_id(backend.provider_id);
  writer->put_generation(backend.provider_generation);
  writer->put_id(backend.endpoint.endpoint_id);
  writer->put_generation(backend.endpoint.endpoint_generation);
  writer->put_id(backend.endpoint.backend_id);
  writer->put_generation(backend.endpoint.backend_boot);
  writer->put_string(backend.endpoint.reference);
  writer->put_string(backend.endpoint.protocol);
  writer->put_u32(backend.endpoint.protocol_version);
  writer->put_id(backend.trust_profile_id);
  writer->put_generation(backend.trust_generation);
  writer->put_u8(static_cast<std::uint8_t>(backend.trust_domain));
  writer->put_string(backend.locality.value());
  writer->put_u32(backend.network_distance);
  writer->put_id(backend.capability_profile_id);
  writer->put_generation(backend.capability_generation);
  writer->put_id(backend.compatibility_profile_id);
  writer->put_generation(backend.compatibility_generation);
  writer->put_u32(static_cast<std::uint32_t>(backend.model_bindings.size()));
  for (const ModelBinding& binding : backend.model_bindings) {
    writer->put_id(binding.model_id);
    writer->put_generation(binding.model_generation);
    writer->put_generation(binding.artifact_generation);
    writer->put_u32(binding.context_limit_tokens);
    writer->put_u32(binding.quality_class);
    put_cost(writer, binding.cost);
    writer->put_i64(binding.bound_at_unix_millis);
  }
  writer->put_u32(backend.reliability_ppm);
  writer->put_bool(backend.offline_capable);
  writer->put_u8(static_cast<std::uint8_t>(backend.provenance));
  writer->put_i64(backend.registered_at_unix_millis);
  writer->put_i64(backend.expires_at_unix_millis);
}

[[nodiscard]] bool get_backend(PayloadReader* reader, const ResourceLimits& limits,
                               BackendDescriptor* backend) {
  backend->backend_id = reader->get_id<BackendId>();
  backend->backend_generation = reader->get_generation<BackendGeneration>();
  backend->backend_boot = reader->get_generation<BackendBootId>();
  backend->backend_registration_generation = reader->get_generation<BackendRegistrationGeneration>();
  backend->provider_id = reader->get_id<ProviderId>();
  backend->provider_generation = reader->get_generation<ProviderGeneration>();
  backend->endpoint.endpoint_id = reader->get_id<EndpointId>();
  backend->endpoint.endpoint_generation = reader->get_generation<EndpointGeneration>();
  backend->endpoint.backend_id = reader->get_id<BackendId>();
  backend->endpoint.backend_boot = reader->get_generation<BackendBootId>();
  backend->endpoint.reference = reader->get_string();
  backend->endpoint.protocol = reader->get_string();
  backend->endpoint.protocol_version = reader->get_u32();
  backend->trust_profile_id = reader->get_id<TrustProfileId>();
  backend->trust_generation = reader->get_generation<TrustGeneration>();
  backend->trust_domain = static_cast<TrustDomain>(reader->get_u8());
  backend->locality = LocalityKey(reader->get_string());
  backend->network_distance = reader->get_u32();
  backend->capability_profile_id = reader->get_id<CapabilityProfileId>();
  backend->capability_generation = reader->get_generation<CapabilityGeneration>();
  backend->compatibility_profile_id = reader->get_id<CompatibilityProfileId>();
  backend->compatibility_generation = reader->get_generation<CompatibilityGeneration>();
  const std::uint32_t bindings = reader->get_u32();
  if (!reader->ok() || bindings > limits.max_model_bindings_per_backend) {
    return false;
  }
  for (std::uint32_t index = 0; index < bindings; ++index) {
    ModelBinding binding;
    binding.model_id = reader->get_id<ModelId>();
    binding.model_generation = reader->get_generation<ModelGeneration>();
    binding.artifact_generation = reader->get_generation<ArtifactGeneration>();
    binding.context_limit_tokens = reader->get_u32();
    binding.quality_class = reader->get_u32();
    if (!get_cost(reader, &binding.cost)) {
      return false;
    }
    binding.bound_at_unix_millis = reader->get_i64();
    backend->model_bindings.push_back(std::move(binding));
  }
  backend->reliability_ppm = reader->get_u32();
  backend->offline_capable = reader->get_bool();
  backend->provenance = static_cast<Provenance>(reader->get_u8());
  backend->registered_at_unix_millis = reader->get_i64();
  backend->expires_at_unix_millis = reader->get_i64();
  return reader->ok();
}

void put_candidate_key(PayloadWriter* writer, const CandidateKey& key) {
  writer->put_id(key.model_id);
  writer->put_generation(key.model_generation);
  writer->put_generation(key.artifact_generation);
  writer->put_id(key.backend_id);
  writer->put_generation(key.backend_generation);
  writer->put_generation(key.backend_boot);
  writer->put_id(key.endpoint_id);
  writer->put_generation(key.endpoint_generation);
}

void get_candidate_key(PayloadReader* reader, CandidateKey* key) {
  key->model_id = reader->get_id<ModelId>();
  key->model_generation = reader->get_generation<ModelGeneration>();
  key->artifact_generation = reader->get_generation<ArtifactGeneration>();
  key->backend_id = reader->get_id<BackendId>();
  key->backend_generation = reader->get_generation<BackendGeneration>();
  key->backend_boot = reader->get_generation<BackendBootId>();
  key->endpoint_id = reader->get_id<EndpointId>();
  key->endpoint_generation = reader->get_generation<EndpointGeneration>();
}

void put_authority(PayloadWriter* writer, const RouteAuthority& authority) {
  writer->put_id(authority.router_id);
  writer->put_generation(authority.router_epoch);
  writer->put_generation(authority.coordinator_epoch);
  writer->put_id(authority.request_id);
  writer->put_generation(authority.request_generation);
  writer->put_id(authority.decision_id);
  writer->put_generation(authority.decision_generation);
  writer->put_id(authority.model_id);
  writer->put_generation(authority.model_generation);
  writer->put_generation(authority.artifact_generation);
  writer->put_id(authority.provider_id);
  writer->put_generation(authority.provider_generation);
  writer->put_id(authority.backend_id);
  writer->put_generation(authority.backend_generation);
  writer->put_generation(authority.backend_boot);
  writer->put_generation(authority.backend_registration_generation);
  writer->put_id(authority.endpoint_id);
  writer->put_generation(authority.endpoint_generation);
  writer->put_id(authority.capability_profile_id);
  writer->put_generation(authority.capability_generation);
  writer->put_id(authority.policy_id);
  writer->put_generation(authority.policy_generation);
  writer->put_id(authority.budget_id);
  writer->put_generation(authority.budget_generation);
  writer->put_id(authority.cost_evidence_id);
  writer->put_generation(authority.price_generation);
  writer->put_id(authority.slo_id);
  writer->put_generation(authority.slo_generation);
  writer->put_id(authority.compatibility_profile_id);
  writer->put_generation(authority.compatibility_generation);
  writer->put_id(authority.trust_profile_id);
  writer->put_generation(authority.trust_generation);
  writer->put_generation(authority.health_generation);
  writer->put_generation(authority.availability_generation);
  writer->put_generation(authority.readiness_generation);
  writer->put_generation(authority.residency_generation);
  writer->put_generation(authority.capacity_generation);
  writer->put_id(authority.reservation_id);
  writer->put_generation(authority.reservation_generation);
  writer->put_id(authority.tenant);
  writer->put_id(authority.name_space);
  writer->put_generation(authority.dispatch_generation);
  writer->put_i64(authority.issued_at_unix_millis);
  writer->put_i64(authority.expires_at_unix_millis);
}

void get_authority(PayloadReader* reader, RouteAuthority* authority) {
  authority->router_id = reader->get_id<RouterId>();
  authority->router_epoch = reader->get_generation<RouterEpoch>();
  authority->coordinator_epoch = reader->get_generation<CoordinatorEpoch>();
  authority->request_id = reader->get_id<RouteRequestId>();
  authority->request_generation = reader->get_generation<RouteRequestGeneration>();
  authority->decision_id = reader->get_id<RouteDecisionId>();
  authority->decision_generation = reader->get_generation<RouteDecisionGeneration>();
  authority->model_id = reader->get_id<ModelId>();
  authority->model_generation = reader->get_generation<ModelGeneration>();
  authority->artifact_generation = reader->get_generation<ArtifactGeneration>();
  authority->provider_id = reader->get_id<ProviderId>();
  authority->provider_generation = reader->get_generation<ProviderGeneration>();
  authority->backend_id = reader->get_id<BackendId>();
  authority->backend_generation = reader->get_generation<BackendGeneration>();
  authority->backend_boot = reader->get_generation<BackendBootId>();
  authority->backend_registration_generation =
      reader->get_generation<BackendRegistrationGeneration>();
  authority->endpoint_id = reader->get_id<EndpointId>();
  authority->endpoint_generation = reader->get_generation<EndpointGeneration>();
  authority->capability_profile_id = reader->get_id<CapabilityProfileId>();
  authority->capability_generation = reader->get_generation<CapabilityGeneration>();
  authority->policy_id = reader->get_id<PolicyId>();
  authority->policy_generation = reader->get_generation<PolicyGeneration>();
  authority->budget_id = reader->get_id<BudgetId>();
  authority->budget_generation = reader->get_generation<BudgetGeneration>();
  authority->cost_evidence_id = reader->get_id<CostEvidenceId>();
  authority->price_generation = reader->get_generation<PriceGeneration>();
  authority->slo_id = reader->get_id<SLOId>();
  authority->slo_generation = reader->get_generation<SLOGeneration>();
  authority->compatibility_profile_id = reader->get_id<CompatibilityProfileId>();
  authority->compatibility_generation = reader->get_generation<CompatibilityGeneration>();
  authority->trust_profile_id = reader->get_id<TrustProfileId>();
  authority->trust_generation = reader->get_generation<TrustGeneration>();
  authority->health_generation = reader->get_generation<HealthGeneration>();
  authority->availability_generation = reader->get_generation<AvailabilityGeneration>();
  authority->readiness_generation = reader->get_generation<ReadinessGeneration>();
  authority->residency_generation = reader->get_generation<ResidencyGeneration>();
  authority->capacity_generation = reader->get_generation<CapacityGeneration>();
  authority->reservation_id = reader->get_id<ReservationId>();
  authority->reservation_generation = reader->get_generation<ReservationGeneration>();
  authority->tenant = reader->get_id<TenantId>();
  authority->name_space = reader->get_id<NamespaceId>();
  authority->dispatch_generation = reader->get_generation<DispatchGeneration>();
  authority->issued_at_unix_millis = reader->get_i64();
  authority->expires_at_unix_millis = reader->get_i64();
}

void put_requirements(PayloadWriter* writer, const RouteRequirements& requirements) {
  writer->put_u32(static_cast<std::uint32_t>(requirements.required_capabilities.size()));
  for (const CapabilityRequirement& requirement : requirements.required_capabilities) {
    writer->put_string(requirement.key.value());
    writer->put_u8(static_cast<std::uint8_t>(requirement.minimum_state));
  }
  writer->put_u32(static_cast<std::uint32_t>(requirements.preferred_capabilities.size()));
  for (const CapabilityKey& key : requirements.preferred_capabilities) {
    writer->put_string(key.value());
  }
  writer->put_u8(static_cast<std::uint8_t>(requirements.minimum_capability_evidence));
  writer->put_u32(requirements.min_context_tokens);
  writer->put_u32(requirements.max_output_tokens);
  writer->put_u32(requirements.required_input_modalities.bits());
  writer->put_u32(requirements.required_output_modalities.bits());
  writer->put_bool(requirements.require_structured_output);
  writer->put_bool(requirements.require_json_schema);
  writer->put_bool(requirements.require_tool_calling);
  writer->put_bool(requirements.require_streaming);
  writer->put_bool(requirements.require_logprobs);
  writer->put_bool(requirements.require_deterministic_seed);
  writer->put_u32(requirements.quality_floor);
  writer->put_u32(static_cast<std::uint32_t>(requirements.model_allowlist.size()));
  for (const ModelId id : requirements.model_allowlist) {
    writer->put_id(id);
  }
  writer->put_u32(static_cast<std::uint32_t>(requirements.model_denylist.size()));
  for (const ModelId id : requirements.model_denylist) {
    writer->put_id(id);
  }
  writer->put_u32(static_cast<std::uint32_t>(requirements.family_allowlist.size()));
  for (const ModelFamilyId id : requirements.family_allowlist) {
    writer->put_id(id);
  }
  writer->put_u32(static_cast<std::uint32_t>(requirements.family_denylist.size()));
  for (const ModelFamilyId id : requirements.family_denylist) {
    writer->put_id(id);
  }
  writer->put_u32(static_cast<std::uint32_t>(requirements.provider_allowlist.size()));
  for (const ProviderId id : requirements.provider_allowlist) {
    writer->put_id(id);
  }
  writer->put_u32(static_cast<std::uint32_t>(requirements.provider_denylist.size()));
  for (const ProviderId id : requirements.provider_denylist) {
    writer->put_id(id);
  }
  writer->put_u32(static_cast<std::uint32_t>(requirements.backend_allowlist.size()));
  for (const BackendId id : requirements.backend_allowlist) {
    writer->put_id(id);
  }
  writer->put_u32(static_cast<std::uint32_t>(requirements.backend_denylist.size()));
  for (const BackendId id : requirements.backend_denylist) {
    writer->put_id(id);
  }
  writer->put_bool(requirements.local_only);
  writer->put_bool(requirements.offline_only);
  writer->put_bool(requirements.remote_allowed);
  writer->put_u32(static_cast<std::uint32_t>(requirements.required_localities.size()));
  for (const LocalityKey& key : requirements.required_localities) {
    writer->put_string(key.value());
  }
  writer->put_u32(static_cast<std::uint32_t>(requirements.denied_localities.size()));
  for (const LocalityKey& key : requirements.denied_localities) {
    writer->put_string(key.value());
  }
  writer->put_u8(static_cast<std::uint8_t>(requirements.minimum_trust));
  writer->put_bool(requirements.require_known_cost);
  writer->put_bool(requirements.enforce_cost_ceiling);
  writer->put_string(requirements.maximum_cost.unit.currency);
  writer->put_string(requirements.maximum_cost.unit.basis);
  writer->put_i64(requirements.maximum_cost.micros);
  writer->put_u32(requirements.max_latency_micros);
  writer->put_u32(requirements.deadline_micros);
  writer->put_bool(requirements.require_slo);
  writer->put_bool(requirements.require_available);
  writer->put_bool(requirements.require_healthy);
  writer->put_bool(requirements.require_ready);
  writer->put_bool(requirements.require_resident);
  writer->put_bool(requirements.require_capacity);
  writer->put_bool(requirements.require_reservation);
  writer->put_bool(requirements.require_known_latency);
  writer->put_bool(requirements.require_known_compatibility);
  writer->put_string(requirements.required_protocol);
  writer->put_u32(requirements.required_protocol_version);
  writer->put_id(requirements.affinity_model);
  writer->put_id(requirements.affinity_provider);
  writer->put_id(requirements.affinity_backend);
  writer->put_bool(requirements.stickiness.prefer_same_model_family);
  writer->put_bool(requirements.stickiness.prefer_same_provider);
  writer->put_bool(requirements.stickiness.prefer_same_backend);
  writer->put_bool(requirements.stickiness.prefer_context_locality);
  writer->put_id(requirements.stickiness.sticky_family);
  writer->put_id(requirements.stickiness.sticky_provider);
  writer->put_id(requirements.stickiness.sticky_backend);
  writer->put_bool(requirements.allow_fallbacks);
  writer->put_u8(static_cast<std::uint8_t>(requirements.fallback_policy));
  writer->put_bool(requirements.retry_policy.allow_retry);
  writer->put_bool(requirements.retry_policy.allow_reroute);
  writer->put_bool(requirements.retry_policy.allow_model_switch);
  writer->put_bool(requirements.retry_policy.allow_provider_switch);
  writer->put_u32(requirements.retry_policy.max_attempts);
  writer->put_u32(requirements.retry_policy.max_fallbacks);
}

[[nodiscard]] bool get_requirements(PayloadReader* reader, const ResourceLimits& limits,
                                    RouteRequirements* requirements) {
  const std::uint32_t required = reader->get_u32();
  if (!reader->ok() || required > limits.max_requirement_entries) {
    return false;
  }
  for (std::uint32_t index = 0; index < required; ++index) {
    CapabilityRequirement requirement;
    requirement.key = CapabilityKey(reader->get_string());
    requirement.minimum_state = static_cast<CapabilityState>(reader->get_u8());
    if (!reader->ok() ||
        static_cast<std::uint8_t>(requirement.minimum_state) >=
            static_cast<std::uint8_t>(CapabilityState::kCount)) {
      return false;
    }
    requirements->required_capabilities.push_back(std::move(requirement));
  }
  const std::uint32_t preferred = reader->get_u32();
  if (!reader->ok() || preferred > limits.max_requirement_entries) {
    return false;
  }
  for (std::uint32_t index = 0; index < preferred; ++index) {
    requirements->preferred_capabilities.push_back(CapabilityKey(reader->get_string()));
  }
  requirements->minimum_capability_evidence = static_cast<CapabilityState>(reader->get_u8());
  requirements->min_context_tokens = reader->get_u32();
  requirements->max_output_tokens = reader->get_u32();
  requirements->required_input_modalities = ModalitySet(reader->get_u32());
  requirements->required_output_modalities = ModalitySet(reader->get_u32());
  requirements->require_structured_output = reader->get_bool();
  requirements->require_json_schema = reader->get_bool();
  requirements->require_tool_calling = reader->get_bool();
  requirements->require_streaming = reader->get_bool();
  requirements->require_logprobs = reader->get_bool();
  requirements->require_deterministic_seed = reader->get_bool();
  requirements->quality_floor = reader->get_u32();
  const auto read_ids = [reader, &limits](auto* list) {
    const std::uint32_t count = reader->get_u32();
    if (!reader->ok() || count > limits.max_allowlist_entries) {
      return false;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
      using IdType = typename std::remove_pointer_t<std::decay_t<decltype(list)>>::value_type;
      list->push_back(reader->get_id<IdType>());
    }
    return reader->ok();
  };
  if (!read_ids(&requirements->model_allowlist) || !read_ids(&requirements->model_denylist) ||
      !read_ids(&requirements->family_allowlist) || !read_ids(&requirements->family_denylist) ||
      !read_ids(&requirements->provider_allowlist) || !read_ids(&requirements->provider_denylist) ||
      !read_ids(&requirements->backend_allowlist) || !read_ids(&requirements->backend_denylist)) {
    return false;
  }
  requirements->local_only = reader->get_bool();
  requirements->offline_only = reader->get_bool();
  requirements->remote_allowed = reader->get_bool();
  const std::uint32_t required_localities = reader->get_u32();
  if (!reader->ok() || required_localities > limits.max_allowlist_entries) {
    return false;
  }
  for (std::uint32_t index = 0; index < required_localities; ++index) {
    requirements->required_localities.push_back(LocalityKey(reader->get_string()));
  }
  const std::uint32_t denied_localities = reader->get_u32();
  if (!reader->ok() || denied_localities > limits.max_allowlist_entries) {
    return false;
  }
  for (std::uint32_t index = 0; index < denied_localities; ++index) {
    requirements->denied_localities.push_back(LocalityKey(reader->get_string()));
  }
  requirements->minimum_trust = static_cast<TrustDomain>(reader->get_u8());
  requirements->require_known_cost = reader->get_bool();
  requirements->enforce_cost_ceiling = reader->get_bool();
  requirements->maximum_cost.unit.currency = reader->get_string();
  requirements->maximum_cost.unit.basis = reader->get_string();
  requirements->maximum_cost.micros = reader->get_i64();
  requirements->max_latency_micros = reader->get_u32();
  requirements->deadline_micros = reader->get_u32();
  requirements->require_slo = reader->get_bool();
  requirements->require_available = reader->get_bool();
  requirements->require_healthy = reader->get_bool();
  requirements->require_ready = reader->get_bool();
  requirements->require_resident = reader->get_bool();
  requirements->require_capacity = reader->get_bool();
  requirements->require_reservation = reader->get_bool();
  requirements->require_known_latency = reader->get_bool();
  requirements->require_known_compatibility = reader->get_bool();
  requirements->required_protocol = reader->get_string();
  requirements->required_protocol_version = reader->get_u32();
  requirements->affinity_model = reader->get_id<ModelId>();
  requirements->affinity_provider = reader->get_id<ProviderId>();
  requirements->affinity_backend = reader->get_id<BackendId>();
  requirements->stickiness.prefer_same_model_family = reader->get_bool();
  requirements->stickiness.prefer_same_provider = reader->get_bool();
  requirements->stickiness.prefer_same_backend = reader->get_bool();
  requirements->stickiness.prefer_context_locality = reader->get_bool();
  requirements->stickiness.sticky_family = reader->get_id<ModelFamilyId>();
  requirements->stickiness.sticky_provider = reader->get_id<ProviderId>();
  requirements->stickiness.sticky_backend = reader->get_id<BackendId>();
  requirements->allow_fallbacks = reader->get_bool();
  requirements->fallback_policy = static_cast<FallbackPolicy>(reader->get_u8());
  requirements->retry_policy.allow_retry = reader->get_bool();
  requirements->retry_policy.allow_reroute = reader->get_bool();
  requirements->retry_policy.allow_model_switch = reader->get_bool();
  requirements->retry_policy.allow_provider_switch = reader->get_bool();
  requirements->retry_policy.max_attempts = reader->get_u32();
  requirements->retry_policy.max_fallbacks = reader->get_u32();
  return reader->ok();
}

void put_decision(PayloadWriter* writer, const RouteDecision& decision) {
  writer->put_id(decision.decision_id);
  writer->put_generation(decision.decision_generation);
  writer->put_id(decision.request_id);
  writer->put_generation(decision.request_generation);
  writer->put_u8(static_cast<std::uint8_t>(decision.status));
  writer->put_u16(static_cast<std::uint16_t>(decision.code));
  put_authority(writer, decision.authority);
  put_candidate_key(writer, decision.explanation.winner);
  writer->put_u32(static_cast<std::uint32_t>(decision.fallbacks.size()));
  for (const CandidateKey& key : decision.fallbacks) {
    put_candidate_key(writer, key);
  }
  writer->put_u32(decision.explanation.eligible_candidate_count);
  writer->put_u32(decision.explanation.rejected_candidate_count);
  writer->put_string(decision.explanation.tie_break_reason);
  writer->put_string(decision.explanation.requirement_digest);
  writer->put_string(decision.explanation.semantic_digest);
  writer->put_u8(static_cast<std::uint8_t>(decision.explanation.currentness));
  writer->put_u16(static_cast<std::uint16_t>(decision.explanation.policy_result));
  put_cost(writer, decision.explanation.cost);
  writer->put_u32(static_cast<std::uint32_t>(decision.explanation.ranking.size()));
  for (const RankedCandidate& ranked : decision.explanation.ranking) {
    put_candidate_key(writer, ranked.key);
    writer->put_i64(ranked.score);
    writer->put_u32(ranked.rank);
    writer->put_u32(static_cast<std::uint32_t>(ranked.factors.size()));
    for (const FactorValue& factor : ranked.factors) {
      writer->put_u8(static_cast<std::uint8_t>(factor.factor));
      writer->put_bool(factor.known);
      writer->put_i64(factor.normalized);
      writer->put_i64(factor.raw);
      writer->put_i64(factor.weight_ppm);
      writer->put_i64(factor.contribution);
      writer->put_u64(factor.evidence_generation);
      writer->put_string(factor.source);
    }
  }
  writer->put_u32(static_cast<std::uint32_t>(decision.explanation.rejections.size()));
  for (const RouteRejection& rejection : decision.explanation.rejections) {
    writer->put_u64(rejection.model_id);
    writer->put_generation(rejection.model_generation);
    writer->put_u64(rejection.backend_id);
    writer->put_generation(rejection.backend_generation);
    writer->put_generation(rejection.backend_boot);
    writer->put_u64(rejection.endpoint_id);
    writer->put_u16(static_cast<std::uint16_t>(rejection.code));
    writer->put_string(rejection.detail);
  }
  writer->put_i64(decision.created_at_unix_millis);
}

[[nodiscard]] bool get_decision(PayloadReader* reader, const ResourceLimits& limits,
                                RouteDecision* decision) {
  decision->decision_id = reader->get_id<RouteDecisionId>();
  decision->decision_generation = reader->get_generation<RouteDecisionGeneration>();
  decision->request_id = reader->get_id<RouteRequestId>();
  decision->request_generation = reader->get_generation<RouteRequestGeneration>();
  decision->status = static_cast<RouteStatus>(reader->get_u8());
  decision->code = static_cast<OutcomeCode>(reader->get_u16());
  get_authority(reader, &decision->authority);
  get_candidate_key(reader, &decision->explanation.winner);
  const std::uint32_t fallbacks = reader->get_u32();
  if (!reader->ok() || fallbacks > limits.max_fallback_candidates) {
    return false;
  }
  for (std::uint32_t index = 0; index < fallbacks; ++index) {
    CandidateKey key;
    get_candidate_key(reader, &key);
    decision->fallbacks.push_back(key);
  }
  decision->explanation.eligible_candidate_count = reader->get_u32();
  decision->explanation.rejected_candidate_count = reader->get_u32();
  decision->explanation.tie_break_reason = reader->get_string();
  decision->explanation.requirement_digest = reader->get_string();
  decision->explanation.semantic_digest = reader->get_string();
  decision->explanation.currentness = static_cast<Currentness>(reader->get_u8());
  decision->explanation.policy_result = static_cast<OutcomeCode>(reader->get_u16());
  if (!get_cost(reader, &decision->explanation.cost)) {
    return false;
  }
  const std::uint32_t ranked = reader->get_u32();
  if (!reader->ok() || ranked > limits.max_ranked_candidates) {
    return false;
  }
  for (std::uint32_t index = 0; index < ranked; ++index) {
    RankedCandidate candidate;
    get_candidate_key(reader, &candidate.key);
    candidate.score = reader->get_i64();
    candidate.rank = reader->get_u32();
    const std::uint32_t factors = reader->get_u32();
    if (!reader->ok() || factors > limits.max_explanation_factors) {
      return false;
    }
    for (std::uint32_t factor_index = 0; factor_index < factors; ++factor_index) {
      FactorValue factor;
      factor.factor = static_cast<RankingFactor>(reader->get_u8());
      factor.known = reader->get_bool();
      factor.normalized = reader->get_i64();
      factor.raw = reader->get_i64();
      factor.weight_ppm = reader->get_i64();
      factor.contribution = reader->get_i64();
      factor.evidence_generation = reader->get_u64();
      factor.source = reader->get_string();
      if (!reader->ok() ||
          static_cast<std::uint8_t>(factor.factor) >=
              static_cast<std::uint8_t>(RankingFactor::kCount)) {
        return false;
      }
      candidate.factors.push_back(std::move(factor));
    }
    decision->explanation.ranking.push_back(std::move(candidate));
  }
  const std::uint32_t rejections = reader->get_u32();
  if (!reader->ok() || rejections > limits.max_rejection_records) {
    return false;
  }
  for (std::uint32_t index = 0; index < rejections; ++index) {
    RouteRejection rejection;
    rejection.model_id = reader->get_u64();
    rejection.model_generation = reader->get_generation<ModelGeneration>();
    rejection.backend_id = reader->get_u64();
    rejection.backend_generation = reader->get_generation<BackendGeneration>();
    rejection.backend_boot = reader->get_generation<BackendBootId>();
    rejection.endpoint_id = reader->get_u64();
    rejection.code = static_cast<OutcomeCode>(reader->get_u16());
    rejection.detail = reader->get_string();
    decision->explanation.rejections.push_back(std::move(rejection));
  }
  decision->created_at_unix_millis = reader->get_i64();
  decision->explanation.authority = decision->authority;
  decision->explanation.request_id = decision->request_id;
  decision->explanation.request_generation = decision->request_generation;
  decision->explanation.fallback_order = decision->fallbacks;
  return reader->ok();
}

}  // namespace

std::string_view to_string(MessageType type) noexcept {
  const auto index = static_cast<std::size_t>(type);
  if (index >= kTypeNames.size()) {
    return "UNKNOWN";
  }
  return kTypeNames[index];
}

void PayloadWriter::fail(std::string message) {
  if (ok_) {
    ok_ = false;
    error_ = std::move(message);
  }
}

void PayloadWriter::put_u8(std::uint8_t value) { bytes_.push_back(value); }

void PayloadWriter::put_u16(std::uint16_t value) {
  put_u8(static_cast<std::uint8_t>(value & 0xFFu));
  put_u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void PayloadWriter::put_u32(std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    put_u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void PayloadWriter::put_u64(std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    put_u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void PayloadWriter::put_i64(std::int64_t value) { put_u64(static_cast<std::uint64_t>(value)); }

void PayloadWriter::put_bool(bool value) { put_u8(value ? 1u : 0u); }

void PayloadWriter::put_string(std::string_view value) {
  if (value.size() > limits_.max_string_bytes) {
    fail("string exceeds the configured byte limit");
    return;
  }
  put_u32(static_cast<std::uint32_t>(value.size()));
  bytes_.insert(bytes_.end(), value.begin(), value.end());
}

void PayloadWriter::put_bytes(std::span<const std::uint8_t> value) {
  if (value.size() > limits_.max_payload_bytes) {
    fail("byte block exceeds the configured limit");
    return;
  }
  put_u32(static_cast<std::uint32_t>(value.size()));
  bytes_.insert(bytes_.end(), value.begin(), value.end());
}

void PayloadReader::fail(std::string message) {
  if (ok_) {
    ok_ = false;
    error_ = std::move(message);
  }
}

bool PayloadReader::require(std::size_t count) {
  if (!ok_ || remaining() < count) {
    fail("payload ended inside a declared field");
    return false;
  }
  return true;
}

std::uint8_t PayloadReader::get_u8() {
  if (!require(1)) {
    return 0;
  }
  return bytes_[offset_++];
}

std::uint16_t PayloadReader::get_u16() {
  std::uint16_t value = 0;
  for (int shift = 0; shift < 16; shift += 8) {
    value |= static_cast<std::uint16_t>(get_u8()) << shift;
  }
  return value;
}

std::uint32_t PayloadReader::get_u32() {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(get_u8()) << shift;
  }
  return value;
}

std::uint64_t PayloadReader::get_u64() {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(get_u8()) << shift;
  }
  return value;
}

std::int64_t PayloadReader::get_i64() { return static_cast<std::int64_t>(get_u64()); }

bool PayloadReader::get_bool() { return get_u8() != 0; }

std::string PayloadReader::get_string() {
  const std::uint32_t size = get_u32();
  if (!ok_) {
    return {};
  }
  if (size > limits_.max_string_bytes) {
    fail("declared string length exceeds the configured limit");
    return {};
  }
  if (!require(size)) {
    return {};
  }
  std::string out(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
  offset_ += size;
  return out;
}

std::vector<std::uint8_t> PayloadReader::get_bytes() {
  const std::uint32_t size = get_u32();
  if (!ok_) {
    return {};
  }
  if (size > limits_.max_payload_bytes) {
    fail("declared byte block length exceeds the configured limit");
    return {};
  }
  if (!require(size)) {
    return {};
  }
  std::vector<std::uint8_t> out(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                                bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
  offset_ += size;
  return out;
}

bool encode_frame(const Frame& frame, const ResourceLimits& limits, std::vector<std::uint8_t>* out,
                  std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  if (frame.header.magic != frame_magic) {
    if (error != nullptr) {
      *error = "frame magic is not the protocol magic";
    }
    return false;
  }
  if (frame.header.protocol_version != protocol_version) {
    if (error != nullptr) {
      *error = "frame protocol version is not supported";
    }
    return false;
  }
  const auto type_index = static_cast<std::size_t>(frame.header.message_type);
  if (type_index == 0 || type_index >= static_cast<std::size_t>(MessageType::kCount)) {
    if (error != nullptr) {
      *error = "frame message type is not a known message type";
    }
    return false;
  }
  if (frame.payload.size() > limits.max_frame_payload_bytes ||
      frame.payload.size() > limits.max_payload_bytes) {
    if (error != nullptr) {
      *error = "frame payload exceeds the configured limit";
    }
    return false;
  }

  FrameHeader header = frame.header;
  header.payload_bytes = static_cast<std::uint32_t>(frame.payload.size());
  header.payload_crc32c = detail::crc32c(frame.payload);

  std::vector<std::uint8_t> header_bytes;
  put_header(&header_bytes, header);
  header.header_crc32c =
      detail::crc32c(std::span<const std::uint8_t>(header_bytes.data(), 16));
  put_header(&header_bytes, header);

  out->clear();
  out->reserve(header_bytes.size() + frame.payload.size());
  out->insert(out->end(), header_bytes.begin(), header_bytes.end());
  out->insert(out->end(), frame.payload.begin(), frame.payload.end());
  if (out->size() > limits.max_frame_bytes) {
    out->clear();
    if (error != nullptr) {
      *error = "encoded frame exceeds the configured byte limit";
    }
    return false;
  }
  return true;
}

bool decode_header(std::span<const std::uint8_t> bytes, const ResourceLimits& limits,
                   FrameHeader* out, std::string* error) {
  if (bytes.size() < frame_header_bytes) {
    if (error != nullptr) {
      *error = "frame header is truncated";
    }
    return false;
  }
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  const std::uint32_t magic = get_u32(bytes, 0);
  if (magic != frame_magic) {
    if (error != nullptr) {
      *error = "frame magic does not match";
    }
    return false;
  }
  const std::uint16_t version = get_u16(bytes, 4);
  if (version != protocol_version) {
    if (error != nullptr) {
      *error = "frame protocol version is not supported";
    }
    return false;
  }
  const std::uint16_t type = get_u16(bytes, 6);
  if (type == 0 || static_cast<std::size_t>(type) >= static_cast<std::size_t>(MessageType::kCount)) {
    if (error != nullptr) {
      *error = "frame message type is not a known message type";
    }
    return false;
  }
  const std::uint32_t stored_crc = get_u32(bytes, 16);
  const std::uint32_t computed_crc =
      detail::crc32c(std::span<const std::uint8_t>(bytes.data(), 16));
  if (stored_crc != computed_crc) {
    if (error != nullptr) {
      *error = "frame header checksum mismatch";
    }
    return false;
  }
  const std::uint32_t payload_bytes = get_u32(bytes, 12);
  if (payload_bytes > limits.max_frame_payload_bytes ||
      payload_bytes > limits.max_payload_bytes) {
    if (error != nullptr) {
      *error = "declared frame payload length exceeds the configured limit";
    }
    return false;
  }
  out->magic = magic;
  out->protocol_version = version;
  out->message_type = type;
  out->flags = get_u32(bytes, 8);
  out->payload_bytes = payload_bytes;
  out->header_crc32c = stored_crc;
  out->payload_crc32c = get_u32(bytes, 20);
  return true;
}

bool decode_frame(std::span<const std::uint8_t> bytes, const ResourceLimits& limits, Frame* out,
                  std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  FrameHeader header;
  if (!decode_header(bytes, limits, &header, error)) {
    return false;
  }
  const std::size_t expected = frame_header_bytes + header.payload_bytes;
  if (bytes.size() != expected) {
    if (error != nullptr) {
      *error = bytes.size() < expected ? "frame is truncated" : "frame has trailing bytes";
    }
    return false;
  }
  const std::span<const std::uint8_t> payload(bytes.data() + frame_header_bytes,
                                              header.payload_bytes);
  if (detail::crc32c(payload) != header.payload_crc32c) {
    if (error != nullptr) {
      *error = "frame payload checksum mismatch";
    }
    return false;
  }
  out->header = header;
  out->payload.assign(payload.begin(), payload.end());
  return true;
}

// ---------------------------------------------------------------------------
// Message codecs
// ---------------------------------------------------------------------------

namespace {

template <class EncodeFn>
[[nodiscard]] bool encode_message(EncodeFn encode, const ResourceLimits& limits,
                                  std::vector<std::uint8_t>* out, std::string* error) {
  PayloadWriter writer(limits);
  encode(&writer);
  if (!writer.ok()) {
    if (error != nullptr) {
      *error = writer.error();
    }
    return false;
  }
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  *out = writer.bytes();
  return true;
}

template <class DecodeFn>
[[nodiscard]] bool decode_message(std::span<const std::uint8_t> payload,
                                  const ResourceLimits& limits, DecodeFn decode,
                                  std::string* error) {
  if (payload.size() > limits.max_payload_bytes) {
    if (error != nullptr) {
      *error = "message payload exceeds the configured limit";
    }
    return false;
  }
  PayloadReader reader(payload, limits);
  if (!decode(&reader)) {
    if (error != nullptr) {
      *error = reader.ok() ? "message payload is malformed" : reader.error();
    }
    return false;
  }
  if (!reader.exhausted()) {
    if (error != nullptr) {
      *error = "message payload has trailing bytes";
    }
    return false;
  }
  return true;
}

}  // namespace

bool encode_hello(const HelloMessage& message, const ResourceLimits& limits,
                  std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message(
      [&message](PayloadWriter* writer) {
        writer->put_id(message.router_id);
        writer->put_generation(message.router_epoch);
        writer->put_generation(message.coordinator_epoch);
        writer->put_id(message.backend_id);
        writer->put_generation(message.backend_generation);
        writer->put_generation(message.backend_boot);
        writer->put_string(message.product);
        writer->put_u32(message.protocol_version);
      },
      limits, out, error);
}

bool decode_hello(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                  HelloMessage* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out](PayloadReader* reader) {
        out->router_id = reader->get_id<RouterId>();
        out->router_epoch = reader->get_generation<RouterEpoch>();
        out->coordinator_epoch = reader->get_generation<CoordinatorEpoch>();
        out->backend_id = reader->get_id<BackendId>();
        out->backend_generation = reader->get_generation<BackendGeneration>();
        out->backend_boot = reader->get_generation<BackendBootId>();
        out->product = reader->get_string();
        out->protocol_version = reader->get_u32();
        return reader->ok();
      },
      error);
}

bool encode_hello_ack(const HelloAckMessage& message, const ResourceLimits& limits,
                      std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message(
      [&message](PayloadWriter* writer) {
        writer->put_id(message.router_id);
        writer->put_generation(message.router_epoch);
        writer->put_generation(message.coordinator_epoch);
        writer->put_u16(static_cast<std::uint16_t>(message.code));
        writer->put_string(message.detail);
      },
      limits, out, error);
}

bool decode_hello_ack(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                      HelloAckMessage* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out](PayloadReader* reader) {
        out->router_id = reader->get_id<RouterId>();
        out->router_epoch = reader->get_generation<RouterEpoch>();
        out->coordinator_epoch = reader->get_generation<CoordinatorEpoch>();
        out->code = static_cast<OutcomeCode>(reader->get_u16());
        out->detail = reader->get_string();
        return reader->ok();
      },
      error);
}

bool encode_backend_register(const BackendRegisterMessage& message, const ResourceLimits& limits,
                             std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message(
      [&message](PayloadWriter* writer) { put_backend(writer, message.backend); }, limits, out,
      error);
}

bool decode_backend_register(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                             BackendRegisterMessage* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [&limits, out](PayloadReader* reader) { return get_backend(reader, limits, &out->backend); },
      error);
}

bool encode_backend_register_ack(const BackendRegisterAckMessage& message,
                                 const ResourceLimits& limits, std::vector<std::uint8_t>* out,
                                 std::string* error) {
  return encode_message(
      [&message](PayloadWriter* writer) {
        writer->put_id(message.backend_id);
        writer->put_generation(message.backend_boot);
        writer->put_u16(static_cast<std::uint16_t>(message.code));
        writer->put_string(message.detail);
      },
      limits, out, error);
}

bool decode_backend_register_ack(std::span<const std::uint8_t> payload,
                                 const ResourceLimits& limits, BackendRegisterAckMessage* out,
                                 std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out](PayloadReader* reader) {
        out->backend_id = reader->get_id<BackendId>();
        out->backend_boot = reader->get_generation<BackendBootId>();
        out->code = static_cast<OutcomeCode>(reader->get_u16());
        out->detail = reader->get_string();
        return reader->ok();
      },
      error);
}

bool encode_capability_publish(const CapabilityPublishMessage& message,
                               const ResourceLimits& limits, std::vector<std::uint8_t>* out,
                               std::string* error) {
  return encode_message(
      [&message](PayloadWriter* writer) {
        const BackendCapabilityPublication& publication = message.publication;
        writer->put_id(publication.backend_id);
        writer->put_generation(publication.backend_generation);
        writer->put_generation(publication.backend_boot);
        writer->put_id(publication.profile_id);
        writer->put_generation(publication.generation);
        writer->put_u32(static_cast<std::uint32_t>(publication.claims.size()));
        for (const CapabilityEvidence& claim : publication.claims) {
          writer->put_string(claim.key.value());
          writer->put_u8(static_cast<std::uint8_t>(claim.state));
          writer->put_generation(claim.generation);
          writer->put_id(claim.profile_id);
          writer->put_generation(claim.model_generation);
          writer->put_generation(claim.backend_generation);
          writer->put_generation(claim.backend_boot);
          writer->put_i64(claim.observed_at_unix_millis);
          writer->put_i64(claim.expires_at_unix_millis);
          writer->put_string(claim.source);
          writer->put_string(claim.detail);
        }
      },
      limits, out, error);
}

bool decode_capability_publish(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                               CapabilityPublishMessage* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [&limits, out](PayloadReader* reader) {
        BackendCapabilityPublication& publication = out->publication;
        publication.backend_id = reader->get_id<BackendId>();
        publication.backend_generation = reader->get_generation<BackendGeneration>();
        publication.backend_boot = reader->get_generation<BackendBootId>();
        publication.profile_id = reader->get_id<CapabilityProfileId>();
        publication.generation = reader->get_generation<CapabilityGeneration>();
        const std::uint32_t count = reader->get_u32();
        if (!reader->ok() || count > limits.max_capability_entries_per_backend) {
          return false;
        }
        for (std::uint32_t index = 0; index < count; ++index) {
          CapabilityEvidence claim;
          claim.key = CapabilityKey(reader->get_string());
          claim.state = static_cast<CapabilityState>(reader->get_u8());
          claim.generation = reader->get_generation<CapabilityGeneration>();
          claim.profile_id = reader->get_id<CapabilityProfileId>();
          claim.model_generation = reader->get_generation<ModelGeneration>();
          claim.backend_generation = reader->get_generation<BackendGeneration>();
          claim.backend_boot = reader->get_generation<BackendBootId>();
          claim.observed_at_unix_millis = reader->get_i64();
          claim.expires_at_unix_millis = reader->get_i64();
          claim.source = reader->get_string();
          claim.detail = reader->get_string();
          if (!reader->ok() ||
              static_cast<std::uint8_t>(claim.state) >=
                  static_cast<std::uint8_t>(CapabilityState::kCount)) {
            return false;
          }
          publication.claims.push_back(std::move(claim));
        }
        return reader->ok();
      },
      error);
}

bool encode_route_request(const RouteRequestMessage& message, const ResourceLimits& limits,
                          std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message(
      [&message](PayloadWriter* writer) {
        const RouteRequest& request = message.request;
        writer->put_id(request.request_id);
        writer->put_generation(request.request_generation);
        writer->put_id(request.tenant);
        writer->put_id(request.name_space);
        writer->put_id(request.policy_id);
        writer->put_generation(request.policy_generation);
        writer->put_id(request.budget_id);
        writer->put_generation(request.budget_generation);
        writer->put_id(request.slo_id);
        writer->put_generation(request.slo_generation);
        writer->put_id(request.correlation_id);
        writer->put_u64(request.caller.agent_runtime_id);
        writer->put_u64(request.caller.agent_run_id);
        writer->put_u64(request.caller.action_id);
        writer->put_u64(request.caller.action_generation);
        writer->put_u64(request.caller.attempt_generation);
        writer->put_u64(request.caller.work_id);
        writer->put_u64(request.caller.work_generation);
        writer->put_u32(request.estimated_input_tokens);
        writer->put_u32(request.estimated_output_tokens);
        writer->put_i64(request.created_at_unix_millis);
        writer->put_i64(request.expires_at_unix_millis);
        put_requirements(writer, request.requirements);
      },
      limits, out, error);
}

bool decode_route_request(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                          RouteRequestMessage* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [&limits, out](PayloadReader* reader) {
        RouteRequest& request = out->request;
        request.request_id = reader->get_id<RouteRequestId>();
        request.request_generation = reader->get_generation<RouteRequestGeneration>();
        request.tenant = reader->get_id<TenantId>();
        request.name_space = reader->get_id<NamespaceId>();
        request.policy_id = reader->get_id<PolicyId>();
        request.policy_generation = reader->get_generation<PolicyGeneration>();
        request.budget_id = reader->get_id<BudgetId>();
        request.budget_generation = reader->get_generation<BudgetGeneration>();
        request.slo_id = reader->get_id<SLOId>();
        request.slo_generation = reader->get_generation<SLOGeneration>();
        request.correlation_id = reader->get_id<CorrelationId>();
        request.caller.agent_runtime_id = reader->get_u64();
        request.caller.agent_run_id = reader->get_u64();
        request.caller.action_id = reader->get_u64();
        request.caller.action_generation = reader->get_u64();
        request.caller.attempt_generation = reader->get_u64();
        request.caller.work_id = reader->get_u64();
        request.caller.work_generation = reader->get_u64();
        request.estimated_input_tokens = reader->get_u32();
        request.estimated_output_tokens = reader->get_u32();
        request.created_at_unix_millis = reader->get_i64();
        request.expires_at_unix_millis = reader->get_i64();
        return get_requirements(reader, limits, &request.requirements);
      },
      error);
}

bool encode_route_response(const RouteResponseMessage& message, const ResourceLimits& limits,
                           std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message(
      [&message](PayloadWriter* writer) {
        writer->put_u16(static_cast<std::uint16_t>(message.code));
        writer->put_bool(message.has_decision);
        put_decision(writer, message.decision);
        writer->put_string(message.detail);
      },
      limits, out, error);
}

bool decode_route_response(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                           RouteResponseMessage* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [&limits, out](PayloadReader* reader) {
        out->code = static_cast<OutcomeCode>(reader->get_u16());
        out->has_decision = reader->get_bool();
        if (!get_decision(reader, limits, &out->decision)) {
          return false;
        }
        out->detail = reader->get_string();
        return reader->ok();
      },
      error);
}

bool encode_dispatch(const DispatchMessage& message, const ResourceLimits& limits,
                     std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message(
      [&message](PayloadWriter* writer) {
        writer->put_id(message.dispatch_id);
        writer->put_generation(message.dispatch_generation);
        writer->put_id(message.decision_id);
        writer->put_generation(message.decision_generation);
        put_candidate_key(writer, message.target);
        writer->put_u32(message.attempt);
      },
      limits, out, error);
}

bool decode_dispatch(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                     DispatchMessage* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out](PayloadReader* reader) {
        out->dispatch_id = reader->get_id<DispatchId>();
        out->dispatch_generation = reader->get_generation<DispatchGeneration>();
        out->decision_id = reader->get_id<RouteDecisionId>();
        out->decision_generation = reader->get_generation<RouteDecisionGeneration>();
        get_candidate_key(reader, &out->target);
        out->attempt = reader->get_u32();
        return reader->ok();
      },
      error);
}

bool encode_dispatch_result(const DispatchResultMessage& message, const ResourceLimits& limits,
                            std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message(
      [&message](PayloadWriter* writer) {
        writer->put_id(message.dispatch_id);
        writer->put_id(message.decision_id);
        writer->put_u16(static_cast<std::uint16_t>(message.code));
        writer->put_u8(static_cast<std::uint8_t>(message.failure));
        writer->put_u32(message.latency_micros);
        writer->put_string(message.detail);
      },
      limits, out, error);
}

bool decode_dispatch_result(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                            DispatchResultMessage* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out](PayloadReader* reader) {
        out->dispatch_id = reader->get_id<DispatchId>();
        out->decision_id = reader->get_id<RouteDecisionId>();
        out->code = static_cast<OutcomeCode>(reader->get_u16());
        out->failure = static_cast<FailureClass>(reader->get_u8());
        out->latency_micros = reader->get_u32();
        out->detail = reader->get_string();
        return reader->ok() &&
               static_cast<std::uint8_t>(out->failure) <
                   static_cast<std::uint8_t>(FailureClass::kCount);
      },
      error);
}

bool encode_error(const ErrorMessage& message, const ResourceLimits& limits,
                  std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message(
      [&message](PayloadWriter* writer) {
        writer->put_u16(static_cast<std::uint16_t>(message.code));
        writer->put_string(message.detail);
      },
      limits, out, error);
}

bool decode_error(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                  ErrorMessage* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out](PayloadReader* reader) {
        out->code = static_cast<OutcomeCode>(reader->get_u16());
        out->detail = reader->get_string();
        return reader->ok();
      },
      error);
}

namespace {

template <class Evidence>
void put_backend_evidence(PayloadWriter* writer, const Evidence& evidence) {
  writer->put_u8(static_cast<std::uint8_t>(evidence.state));
  writer->put_generation(evidence.generation);
  writer->put_id(evidence.backend_id);
  writer->put_generation(evidence.backend_generation);
  writer->put_generation(evidence.backend_boot);
  writer->put_generation(evidence.endpoint_generation);
  writer->put_i64(evidence.observed_at_unix_millis);
  writer->put_i64(evidence.expires_at_unix_millis);
  writer->put_string(evidence.source);
  writer->put_string(evidence.detail);
}

template <class Evidence, class StateEnum, class GenerationType>
[[nodiscard]] bool get_backend_evidence(PayloadReader* reader, Evidence* evidence) {
  evidence->state = static_cast<StateEnum>(reader->get_u8());
  evidence->generation = reader->get_generation<GenerationType>();
  evidence->backend_id = reader->get_id<BackendId>();
  evidence->backend_generation = reader->get_generation<BackendGeneration>();
  evidence->backend_boot = reader->get_generation<BackendBootId>();
  evidence->endpoint_generation = reader->get_generation<EndpointGeneration>();
  evidence->observed_at_unix_millis = reader->get_i64();
  evidence->expires_at_unix_millis = reader->get_i64();
  evidence->source = reader->get_string();
  evidence->detail = reader->get_string();
  return reader->ok() &&
         static_cast<std::uint8_t>(evidence->state) < static_cast<std::uint8_t>(StateEnum::kCount);
}

}  // namespace

bool encode_health(const HealthEvidence& evidence, const ResourceLimits& limits,
                   std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message([&evidence](PayloadWriter* writer) { put_backend_evidence(writer, evidence); },
                        limits, out, error);
}

bool decode_health(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                   HealthEvidence* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out](PayloadReader* reader) {
        return get_backend_evidence<HealthEvidence, HealthState, HealthGeneration>(reader, out);
      },
      error);
}

bool encode_availability(const AvailabilityEvidence& evidence, const ResourceLimits& limits,
                         std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message([&evidence](PayloadWriter* writer) { put_backend_evidence(writer, evidence); },
                        limits, out, error);
}

bool decode_availability(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                         AvailabilityEvidence* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out](PayloadReader* reader) {
        return get_backend_evidence<AvailabilityEvidence, AvailabilityState, AvailabilityGeneration>(
            reader, out);
      },
      error);
}

bool encode_readiness(const ReadinessEvidence& evidence, const ResourceLimits& limits,
                      std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message([&evidence](PayloadWriter* writer) { put_backend_evidence(writer, evidence); },
                        limits, out, error);
}

bool decode_readiness(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                      ReadinessEvidence* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out](PayloadReader* reader) {
        return get_backend_evidence<ReadinessEvidence, ReadinessState, ReadinessGeneration>(reader,
                                                                                            out);
      },
      error);
}

bool encode_residency(const ResidencyEvidence& evidence, const ResourceLimits& limits,
                      std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message([&evidence](PayloadWriter* writer) { put_backend_evidence(writer, evidence); },
                        limits, out, error);
}

bool decode_residency(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                      ResidencyEvidence* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out](PayloadReader* reader) {
        return get_backend_evidence<ResidencyEvidence, ResidencyState, ResidencyGeneration>(reader,
                                                                                            out);
      },
      error);
}

bool encode_capacity(const CapacityEvidence& evidence, const CapacityDetail& detail,
                     const ResourceLimits& limits, std::vector<std::uint8_t>* out,
                     std::string* error) {
  return encode_message(
      [&evidence, &detail](PayloadWriter* writer) {
        put_backend_evidence(writer, evidence);
        writer->put_generation(detail.generation);
        writer->put_id(detail.backend_id);
        writer->put_generation(detail.backend_boot);
        writer->put_u32(detail.available_slots);
        writer->put_u32(detail.total_slots);
        writer->put_u32(detail.queue_depth);
        writer->put_u32(detail.max_queue_depth);
        writer->put_i64(detail.observed_at_unix_millis);
        writer->put_i64(detail.expires_at_unix_millis);
      },
      limits, out, error);
}

bool decode_capacity(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                     CapacityEvidence* out, CapacityDetail* detail, std::string* error) {
  if (out == nullptr || detail == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out, detail](PayloadReader* reader) {
        if (!get_backend_evidence<CapacityEvidence, CapacityState, CapacityGeneration>(reader,
                                                                                       out)) {
          return false;
        }
        detail->generation = reader->get_generation<CapacityGeneration>();
        detail->backend_id = reader->get_id<BackendId>();
        detail->backend_boot = reader->get_generation<BackendBootId>();
        detail->available_slots = reader->get_u32();
        detail->total_slots = reader->get_u32();
        detail->queue_depth = reader->get_u32();
        detail->max_queue_depth = reader->get_u32();
        detail->observed_at_unix_millis = reader->get_i64();
        detail->expires_at_unix_millis = reader->get_i64();
        return reader->ok();
      },
      error);
}

bool encode_cost(const CostEvidence& evidence, const ResourceLimits& limits,
                  std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message([&evidence](PayloadWriter* writer) { put_cost(writer, evidence); }, limits,
                        out, error);
}

bool decode_cost(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                 CostEvidence* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits, [out](PayloadReader* reader) { return get_cost(reader, out); }, error);
}

std::string_view to_string(AdminOp op) noexcept {
  switch (op) {
    case AdminOp::NONE:
      return "NONE";
    case AdminOp::SNAPSHOT:
      return "SNAPSHOT";
    case AdminOp::INVARIANTS:
      return "INVARIANTS";
    case AdminOp::SUMMARY:
      return "SUMMARY";
    case AdminOp::REVALIDATE:
      return "REVALIDATE";
    case AdminOp::DISPATCH:
      return "DISPATCH";
    case AdminOp::REROUTE:
      return "REROUTE";
    case AdminOp::SET_POLICY_OPEN:
      return "SET_POLICY_OPEN";
    case AdminOp::SET_POLICY_DENY_BACKEND:
      return "SET_POLICY_DENY_BACKEND";
    case AdminOp::SET_POLICY_DENY_MODEL:
      return "SET_POLICY_DENY_MODEL";
    case AdminOp::SET_BUDGET_DENIED:
      return "SET_BUDGET_DENIED";
    case AdminOp::SET_BUDGET_ALLOWED:
      return "SET_BUDGET_ALLOWED";
    case AdminOp::ADVANCE_PRICE:
      return "ADVANCE_PRICE";
    case AdminOp::FENCE_BACKEND:
      return "FENCE_BACKEND";
    case AdminOp::RETIRE_MODEL:
      return "RETIRE_MODEL";
    case AdminOp::REVOKE_CAPABILITY:
      return "REVOKE_CAPABILITY";
    case AdminOp::SET_READINESS:
      return "SET_READINESS";
    case AdminOp::SET_AVAILABILITY:
      return "SET_AVAILABILITY";
    case AdminOp::SAVE:
      return "SAVE";
    case AdminOp::LOAD:
      return "LOAD";
    case AdminOp::SHUTDOWN:
      return "SHUTDOWN";
    case AdminOp::BACKEND_COUNT:
      return "BACKEND_COUNT";
    default:
      return "INVALID";
  }
}

bool encode_admin_request(const AdminRequest& message, const ResourceLimits& limits,
                          std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message(
      [&message](PayloadWriter* writer) {
        writer->put_u16(static_cast<std::uint16_t>(message.op));
        writer->put_id(message.backend_id);
        writer->put_id(message.model_id);
        writer->put_id(message.decision_id);
        writer->put_u64(message.generation);
        writer->put_u64(message.value);
        writer->put_string(message.detail);
      },
      limits, out, error);
}

bool decode_admin_request(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                          AdminRequest* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out](PayloadReader* reader) {
        out->op = static_cast<AdminOp>(reader->get_u16());
        out->backend_id = reader->get_id<BackendId>();
        out->model_id = reader->get_id<ModelId>();
        out->decision_id = reader->get_id<RouteDecisionId>();
        out->generation = reader->get_u64();
        out->value = reader->get_u64();
        out->detail = reader->get_string();
        return reader->ok() &&
               static_cast<std::uint16_t>(out->op) < static_cast<std::uint16_t>(AdminOp::kCount);
      },
      error);
}

bool encode_admin_response(const AdminResponse& message, const ResourceLimits& limits,
                           std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message(
      [&message](PayloadWriter* writer) {
        writer->put_u16(static_cast<std::uint16_t>(message.code));
        writer->put_string(message.detail);
        writer->put_string(message.json);
      },
      limits, out, error);
}

bool decode_admin_response(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                           AdminResponse* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out](PayloadReader* reader) {
        out->code = static_cast<OutcomeCode>(reader->get_u16());
        out->detail = reader->get_string();
        out->json = reader->get_string();
        return reader->ok();
      },
      error);
}

bool encode_latency(const LatencyEvidence& evidence, const ResourceLimits& limits,
                    std::vector<std::uint8_t>* out, std::string* error) {
  return encode_message(
      [&evidence](PayloadWriter* writer) {
        writer->put_generation(evidence.health_generation);
        writer->put_id(evidence.backend_id);
        writer->put_generation(evidence.backend_boot);
        writer->put_u32(evidence.dispatch_micros);
        writer->put_u32(evidence.time_to_first_token_micros);
        writer->put_u32(evidence.completed_micros);
        writer->put_u32(evidence.tail_micros);
        writer->put_u32(evidence.queue_micros);
        writer->put_u32(evidence.warm_start_penalty_micros);
        writer->put_u32(evidence.cold_start_penalty_micros);
        writer->put_u32(evidence.sample_count);
        writer->put_i64(evidence.observed_at_unix_millis);
        writer->put_i64(evidence.expires_at_unix_millis);
        writer->put_string(evidence.source);
      },
      limits, out, error);
}

bool decode_latency(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                    LatencyEvidence* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output pointer is null";
    }
    return false;
  }
  return decode_message(
      payload, limits,
      [out](PayloadReader* reader) {
        out->health_generation = reader->get_generation<HealthGeneration>();
        out->backend_id = reader->get_id<BackendId>();
        out->backend_boot = reader->get_generation<BackendBootId>();
        out->dispatch_micros = reader->get_u32();
        out->time_to_first_token_micros = reader->get_u32();
        out->completed_micros = reader->get_u32();
        out->tail_micros = reader->get_u32();
        out->queue_micros = reader->get_u32();
        out->warm_start_penalty_micros = reader->get_u32();
        out->cold_start_penalty_micros = reader->get_u32();
        out->sample_count = reader->get_u32();
        out->observed_at_unix_millis = reader->get_i64();
        out->expires_at_unix_millis = reader->get_i64();
        out->source = reader->get_string();
        return reader->ok();
      },
      error);
}

}  // namespace model_router::distributed
