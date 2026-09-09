// Model Router - deterministic SYNTHETIC reference model profiles.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// These profiles exist to prove routing behaviour. They are SYNTHETIC: no
// benchmark quality is claimed for them and they do not perform inference.

#ifndef MODEL_ROUTER_REFERENCE_PROFILES_HPP
#define MODEL_ROUTER_REFERENCE_PROFILES_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "model_router/catalog.hpp"
#include "model_router/evidence.hpp"

namespace model_router::reference {

/// Deterministic identity set for the reference catalog.
struct Catalog {
  ProviderId provider{};
  ProviderGeneration provider_generation{};

  ModelId small_model{};
  ModelGeneration small_generation{};
  ArtifactGeneration small_artifact{};

  ModelId general_model{};
  ModelGeneration general_generation{};
  ArtifactGeneration general_artifact{};

  ModelId specialist_model{};
  ModelGeneration specialist_generation{};
  ArtifactGeneration specialist_artifact{};

  LocalityKey locality{};
};

/// Returns the fixed reference identities. Calling it twice returns the same
/// values, so reference proofs are reproducible.
[[nodiscard]] Catalog make_catalog();

/// Builds the three reference model descriptors. All are labelled SYNTHETIC.
[[nodiscard]] std::vector<ModelDescriptor> make_models(const Catalog& catalog,
                                                       UnixMillis now);

/// Builds one reference backend descriptor bound to a specific incarnation.
/// Dynamic evidence is left UNKNOWN: a backend must publish its own current
/// evidence after every boot.
[[nodiscard]] BackendDescriptor make_backend(const Catalog& catalog, BackendId backend_id,
                                             BackendGeneration backend_generation,
                                             BackendBootId backend_boot,
                                             BackendRegistrationGeneration registration,
                                             std::string endpoint_reference, Provenance provenance,
                                             UnixMillis now);

/// Price evidence for one (model, backend) pairing, in a single cost unit.
/// Different backends carry different prices so cost-aware routing is provable.
[[nodiscard]] CostEvidence make_cost_evidence(BackendId backend_id, ModelId model_id,
                                              PriceGeneration price_generation,
                                              std::int64_t input_micros_per_unit,
                                              std::int64_t output_micros_per_unit,
                                              std::int64_t estimated_total_micros, UnixMillis now);

/// The single cost unit used by every reference price.
[[nodiscard]] CostUnit reference_cost_unit();

}  // namespace model_router::reference

#endif  // MODEL_ROUTER_REFERENCE_PROFILES_HPP
