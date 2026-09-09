// Model Router - model, backend, endpoint, and provider descriptors.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_CATALOG_HPP
#define MODEL_ROUTER_CATALOG_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "model_router/capability.hpp"
#include "model_router/clock.hpp"
#include "model_router/evidence.hpp"
#include "model_router/ids.hpp"

namespace model_router {

/// How a descriptor came to exist. This label is carried into explanations so a
/// synthetic reference profile can never be mistaken for a real deployment.
enum class Provenance : std::uint8_t {
  REAL = 0,
  SYNTHETIC = 1,
  UNSUPPORTED = 2,
  kCount
};

[[nodiscard]] std::string_view to_string(Provenance provenance) noexcept;

/// Input/output modality bit set.
enum class Modality : std::uint32_t {
  NONE = 0,
  TEXT = 1u << 0,
  IMAGE = 1u << 1,
  AUDIO = 1u << 2,
  VIDEO = 1u << 3,
  EMBEDDING = 1u << 4,
  kCount = 5
};

[[nodiscard]] std::string_view to_string(Modality modality) noexcept;

class ModalitySet {
 public:
  constexpr ModalitySet() noexcept = default;
  constexpr explicit ModalitySet(std::uint32_t bits) noexcept : bits_(bits) {}

  [[nodiscard]] constexpr std::uint32_t bits() const noexcept { return bits_; }
  [[nodiscard]] constexpr bool contains(Modality modality) const noexcept {
    return (bits_ & static_cast<std::uint32_t>(modality)) != 0;
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
  constexpr void add(Modality modality) noexcept { bits_ |= static_cast<std::uint32_t>(modality); }
  constexpr void remove(Modality modality) noexcept {
    bits_ &= ~static_cast<std::uint32_t>(modality);
  }
  /// True when every modality in p other is present here.
  [[nodiscard]] constexpr bool covers(ModalitySet other) const noexcept {
    return (bits_ & other.bits_) == other.bits_;
  }

  friend constexpr bool operator==(ModalitySet, ModalitySet) noexcept = default;

 private:
  std::uint32_t bits_{0};
};

/// Lifecycle state of a model generation as published by the model registry
/// boundary. Model Router consumes this; it never invents lifecycle truth.
enum class ModelLifecycle : std::uint8_t {
  UNKNOWN = 0,
  CURRENT = 1,
  DEPRECATED = 2,
  RETIRED = 3,
  kCount
};

[[nodiscard]] std::string_view to_string(ModelLifecycle lifecycle) noexcept;

/// Semantic model identity and its declared shape. Model identity is separate
/// from serving backend identity: one model may be served by many backends.
struct ModelDescriptor {
  ModelId model_id{};
  ModelGeneration model_generation{};
  ModelVersionId model_version_id{};
  ArtifactGeneration artifact_generation{};
  ModelFamilyId family_id{};

  std::uint32_t context_limit_tokens{0};
  std::uint32_t max_output_tokens{0};

  ModalitySet input_modalities{};
  ModalitySet output_modalities{};

  QualityClass quality_class{kUnclassifiedQuality};
  ModelLifecycle lifecycle{ModelLifecycle::UNKNOWN};

  CapabilityProfileId capability_profile_id{};
  CapabilityGeneration capability_generation{};
  CapabilityProfile capabilities{};

  Provenance provenance{Provenance::SYNTHETIC};
  UnixMillis registered_at_unix_millis{0};
  UnixMillis expires_at_unix_millis{kNoExpiry};
  std::string display_name;

  [[nodiscard]] bool retired() const noexcept {
    return lifecycle == ModelLifecycle::RETIRED;
  }
  [[nodiscard]] bool current(UnixMillis now) const noexcept {
    return model_generation.valid() && artifact_generation.valid() &&
           lifecycle == ModelLifecycle::CURRENT &&
           !(expires_at_unix_millis != kNoExpiry && now >= expires_at_unix_millis);
  }
  /// Returns an empty string when the descriptor is internally coherent.
  [[nodiscard]] std::string validate() const;
};

/// One model served by one backend, with the price and residency that apply to
/// that specific pairing.
struct ModelBinding {
  ModelId model_id{};
  ModelGeneration model_generation{};
  ArtifactGeneration artifact_generation{};

  /// 0 means "use the model descriptor value".
  std::uint32_t context_limit_tokens{0};
  /// kUnclassifiedQuality means "use the model descriptor value".
  QualityClass quality_class{kUnclassifiedQuality};

  CostEvidence cost{};
  UnixMillis bound_at_unix_millis{0};

  friend bool operator==(const ModelBinding&, const ModelBinding&) = default;
};

/// A concrete dispatch destination. The endpoint generation changes whenever the
/// address, protocol, or connection identity is re-established.
struct EndpointDescriptor {
  EndpointId endpoint_id{};
  EndpointGeneration endpoint_generation{};
  BackendId backend_id{};
  BackendBootId backend_boot{};
  /// Opaque, bounded destination reference (for example "127.0.0.1:7411").
  std::string reference;
  std::string protocol;
  std::uint32_t protocol_version{0};

  [[nodiscard]] std::string validate() const;
  friend bool operator==(const EndpointDescriptor&, const EndpointDescriptor&) = default;
};

/// Everything Model Router knows about one serving backend incarnation. Dynamic
/// facts (health, availability, readiness, residency, capacity, latency) are
/// generation-bound evidence; a restarted process must republish all of them.
struct BackendDescriptor {
  BackendId backend_id{};
  BackendGeneration backend_generation{};
  BackendBootId backend_boot{};
  BackendRegistrationGeneration backend_registration_generation{};

  ProviderId provider_id{};
  ProviderGeneration provider_generation{};

  EndpointDescriptor endpoint{};

  TrustProfileId trust_profile_id{};
  TrustGeneration trust_generation{};
  TrustDomain trust_domain{TrustDomain::UNKNOWN};

  LocalityKey locality{};
  /// Relative network distance in micro-units of cost; 0 means unknown.
  std::uint32_t network_distance{0};

  CapabilityProfileId capability_profile_id{};
  CapabilityGeneration capability_generation{};
  CapabilityProfile capabilities{};

  CompatibilityProfileId compatibility_profile_id{};
  CompatibilityGeneration compatibility_generation{};

  std::vector<ModelBinding> model_bindings;

  HealthEvidence health{};
  AvailabilityEvidence availability{};
  ReadinessEvidence readiness{};
  ResidencyEvidence residency{};
  CapacityEvidence capacity{};
  CapacityDetail capacity_detail{};
  LatencyEvidence latency{};

  /// Historical reliability in parts per 10000 (0 = unknown). Only ever set
  /// from authoritative evidence, never guessed.
  std::uint32_t reliability_ppm{0};

  bool offline_capable{false};
  Provenance provenance{Provenance::SYNTHETIC};

  UnixMillis registered_at_unix_millis{0};
  UnixMillis expires_at_unix_millis{kNoExpiry};

  [[nodiscard]] bool retired() const noexcept { return health.state == HealthState::RETIRED; }
  [[nodiscard]] bool draining() const noexcept { return health.state == HealthState::DRAINING; }

  [[nodiscard]] const ModelBinding* find_binding(ModelId model_id) const noexcept;
  /// Returns an empty string when the descriptor is internally coherent.
  [[nodiscard]] std::string validate() const;
  /// Canonicalizes model_bindings ordering so iteration never depends on input
  /// order.
  void canonicalize();
};

/// A provider identity. Providers are a grouping of backends, not a dispatch
/// destination.
struct ProviderDescriptor {
  ProviderId provider_id{};
  ProviderGeneration generation{};
  std::string display_name;
  TrustDomain default_trust_domain{TrustDomain::UNKNOWN};
  bool third_party{true};

  friend bool operator==(const ProviderDescriptor&, const ProviderDescriptor&) = default;
};

}  // namespace model_router

#endif  // MODEL_ROUTER_CATALOG_HPP
