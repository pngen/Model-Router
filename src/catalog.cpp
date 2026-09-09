// Model Router - model, backend, endpoint, and provider descriptors.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/catalog.hpp"

#include <algorithm>

namespace model_router {

std::string_view to_string(Provenance provenance) noexcept {
  switch (provenance) {
    case Provenance::REAL:
      return "REAL";
    case Provenance::SYNTHETIC:
      return "SYNTHETIC";
    case Provenance::UNSUPPORTED:
      return "UNSUPPORTED";
    default:
      return "INVALID";
  }
}

std::string_view to_string(Modality modality) noexcept {
  switch (modality) {
    case Modality::NONE:
      return "NONE";
    case Modality::TEXT:
      return "TEXT";
    case Modality::IMAGE:
      return "IMAGE";
    case Modality::AUDIO:
      return "AUDIO";
    case Modality::VIDEO:
      return "VIDEO";
    case Modality::EMBEDDING:
      return "EMBEDDING";
    default:
      return "INVALID";
  }
}

std::string_view to_string(ModelLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case ModelLifecycle::UNKNOWN:
      return "UNKNOWN";
    case ModelLifecycle::CURRENT:
      return "CURRENT";
    case ModelLifecycle::DEPRECATED:
      return "DEPRECATED";
    case ModelLifecycle::RETIRED:
      return "RETIRED";
    default:
      return "INVALID";
  }
}

std::string ModelDescriptor::validate() const {
  if (!model_id.valid()) {
    return "model identity is not valid";
  }
  if (!model_generation.valid()) {
    return "model generation is not valid";
  }
  if (!artifact_generation.valid()) {
    return "artifact generation is not valid";
  }
  if (context_limit_tokens == 0) {
    return "model context limit is zero";
  }
  if (max_output_tokens > context_limit_tokens) {
    return "model max output tokens exceed the context limit";
  }
  if (input_modalities.empty()) {
    return "model declares no input modality";
  }
  if (output_modalities.empty()) {
    return "model declares no output modality";
  }
  if (capability_profile_id.valid() != capability_generation.valid()) {
    return "model capability profile identity and generation disagree";
  }
  return {};
}

std::string EndpointDescriptor::validate() const {
  if (!endpoint_id.valid()) {
    return "endpoint identity is not valid";
  }
  if (!endpoint_generation.valid()) {
    return "endpoint generation is not valid";
  }
  if (!backend_id.valid()) {
    return "endpoint backend identity is not valid";
  }
  if (!backend_boot.valid()) {
    return "endpoint backend boot identity is not valid";
  }
  if (reference.empty()) {
    return "endpoint reference is empty";
  }
  return {};
}

const ModelBinding* BackendDescriptor::find_binding(ModelId model_id) const noexcept {
  const auto iter = std::lower_bound(
      model_bindings.begin(), model_bindings.end(), model_id,
      [](const ModelBinding& binding, ModelId value) { return binding.model_id < value; });
  if (iter == model_bindings.end() || !(iter->model_id == model_id)) {
    return nullptr;
  }
  return &*iter;
}

void BackendDescriptor::canonicalize() {
  std::stable_sort(model_bindings.begin(), model_bindings.end(),
                   [](const ModelBinding& lhs, const ModelBinding& rhs) {
                     return lhs.model_id < rhs.model_id;
                   });
  model_bindings.erase(
      std::unique(model_bindings.begin(), model_bindings.end(),
                  [](const ModelBinding& lhs, const ModelBinding& rhs) {
                    return lhs.model_id == rhs.model_id;
                  }),
      model_bindings.end());
}

std::string BackendDescriptor::validate() const {
  if (!backend_id.valid()) {
    return "backend identity is not valid";
  }
  if (!backend_generation.valid()) {
    return "backend generation is not valid";
  }
  if (!backend_boot.valid()) {
    return "backend boot identity is not valid";
  }
  if (!backend_registration_generation.valid()) {
    return "backend registration generation is not valid";
  }
  if (!provider_id.valid()) {
    return "backend provider identity is not valid";
  }
  if (!provider_generation.valid()) {
    return "backend provider generation is not valid";
  }
  const std::string endpoint_error = endpoint.validate();
  if (!endpoint_error.empty()) {
    return endpoint_error;
  }
  if (!(endpoint.backend_id == backend_id)) {
    return "backend endpoint belongs to a different backend identity";
  }
  if (!(endpoint.backend_boot == backend_boot)) {
    return "backend endpoint belongs to a different backend incarnation";
  }
  // A profile identity may exist before any evidence for it is published, but a
  // generation without an identity is incoherent.
  if (capability_generation.valid() && !capability_profile_id.valid()) {
    return "backend capability generation exists without a profile identity";
  }
  if (compatibility_generation.valid() && !compatibility_profile_id.valid()) {
    return "backend compatibility generation exists without a profile identity";
  }
  if (model_bindings.empty()) {
    return "backend declares no model binding";
  }
  ModelId previous{};
  bool first = true;
  for (const ModelBinding& binding : model_bindings) {
    if (!binding.model_id.valid()) {
      return "backend model binding has an invalid model identity";
    }
    if (!binding.model_generation.valid()) {
      return "backend model binding has an invalid model generation";
    }
    if (!binding.artifact_generation.valid()) {
      return "backend model binding has an invalid artifact generation";
    }
    if (!first && !(previous < binding.model_id)) {
      return "backend model bindings are not in canonical order or contain duplicates";
    }
    previous = binding.model_id;
    first = false;
  }
  return {};
}

}  // namespace model_router
