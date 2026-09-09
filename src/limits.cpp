// Model Router - bounded resource limits.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/limits.hpp"

namespace model_router {
namespace {

bool add_failure(std::string* message, const char* name, std::uint64_t value,
                 const char* relation, std::uint64_t bound) {
  if (message->empty()) {
    *message = std::string(name) + "=" + std::to_string(value) + " violates " + relation + "=" +
               std::to_string(bound);
  }
  return false;
}

}  // namespace

std::string ResourceLimits::validate() const {
  std::string message;

  if (max_models == 0) {
    add_failure(&message, "max_models", max_models, "minimum", 1);
  }
  if (max_backends == 0) {
    add_failure(&message, "max_backends", max_backends, "minimum", 1);
  }
  if (max_candidates_per_request == 0) {
    add_failure(&message, "max_candidates_per_request", max_candidates_per_request, "minimum", 1);
  }
  if (max_ranked_candidates == 0) {
    add_failure(&message, "max_ranked_candidates", max_ranked_candidates, "minimum", 1);
  }
  if (max_ranked_candidates > max_candidates_per_request) {
    add_failure(&message, "max_ranked_candidates", max_ranked_candidates,
                "max_candidates_per_request", max_candidates_per_request);
  }
  if (max_fallback_candidates > max_ranked_candidates) {
    add_failure(&message, "max_fallback_candidates", max_fallback_candidates,
                "max_ranked_candidates", max_ranked_candidates);
  }
  if (max_explanation_factors == 0) {
    add_failure(&message, "max_explanation_factors", max_explanation_factors, "minimum", 1);
  }
  if (max_rejection_records < max_candidates_per_request) {
    add_failure(&message, "max_rejection_records", max_rejection_records,
                "max_candidates_per_request", max_candidates_per_request);
  }
  if (max_frame_payload_bytes == 0) {
    add_failure(&message, "max_frame_payload_bytes", max_frame_payload_bytes, "minimum", 1);
  }
  if (max_frame_bytes < max_frame_payload_bytes) {
    add_failure(&message, "max_frame_bytes", max_frame_bytes, "max_frame_payload_bytes",
                max_frame_payload_bytes);
  }
  if (max_payload_bytes == 0) {
    add_failure(&message, "max_payload_bytes", max_payload_bytes, "minimum", 1);
  }
  if (max_string_bytes == 0) {
    add_failure(&message, "max_string_bytes", max_string_bytes, "minimum", 1);
  }
  if (max_collection_entries == 0) {
    add_failure(&message, "max_collection_entries", max_collection_entries, "minimum", 1);
  }
  if (max_metadata_entries > max_collection_entries) {
    add_failure(&message, "max_metadata_entries", max_metadata_entries, "max_collection_entries",
                max_collection_entries);
  }
  if (max_persistence_bytes == 0) {
    add_failure(&message, "max_persistence_bytes", max_persistence_bytes, "minimum", 1);
  }
  if (max_persistence_records == 0) {
    add_failure(&message, "max_persistence_records", max_persistence_records, "minimum", 1);
  }
  if (max_send_queue_frames == 0) {
    add_failure(&message, "max_send_queue_frames", max_send_queue_frames, "minimum", 1);
  }
  if (max_session_threads == 0) {
    add_failure(&message, "max_session_threads", max_session_threads, "minimum", 1);
  }
  if (max_connections == 0) {
    add_failure(&message, "max_connections", max_connections, "minimum", 1);
  }
  if (max_route_history == 0) {
    add_failure(&message, "max_route_history", max_route_history, "minimum", 1);
  }
  if (max_requirement_entries == 0) {
    add_failure(&message, "max_requirement_entries", max_requirement_entries, "minimum", 1);
  }
  if (max_model_bindings_per_backend == 0) {
    add_failure(&message, "max_model_bindings_per_backend", max_model_bindings_per_backend,
                "minimum", 1);
  }
  if (max_backend_incarnations < max_backends) {
    add_failure(&message, "max_backend_incarnations", max_backend_incarnations, "max_backends",
                max_backends);
  }
  return message;
}

ResourceLimits default_resource_limits() { return ResourceLimits{}; }

}  // namespace model_router
