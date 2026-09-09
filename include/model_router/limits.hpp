// Model Router - bounded resource limits.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_LIMITS_HPP
#define MODEL_ROUTER_LIMITS_HPP

#include <cstdint>
#include <string>

namespace model_router {

/// Every externally influenced collection is bounded. Limits are checked before
/// allocation with checked arithmetic; exceeding a limit is an explicit
/// REJECT_LIMIT outcome, never a silent truncation or unbounded growth.
struct ResourceLimits {
  // Canonical catalog
  std::uint32_t max_models = 8192;
  std::uint32_t max_providers = 1024;
  std::uint32_t max_backends = 16384;
  std::uint32_t max_backend_incarnations = 65536;
  std::uint32_t max_endpoints = 16384;
  std::uint32_t max_model_bindings_per_backend = 256;
  std::uint32_t max_capability_entries_per_model = 256;
  std::uint32_t max_capability_entries_per_backend = 256;
  std::uint32_t max_capability_entries = 1u << 20;

  // Routing
  std::uint32_t max_candidates_per_request = 100000;
  std::uint32_t max_ranked_candidates = 4096;
  std::uint32_t max_fallback_candidates = 64;
  std::uint32_t max_explanation_factors = 512;
  std::uint32_t max_rejection_records = 100000;
  std::uint32_t max_route_requests = 1000000;
  std::uint32_t max_route_decisions = 1000000;
  std::uint32_t max_route_history = 1000000;
  std::uint32_t max_dispatch_records = 1000000;
  std::uint32_t max_requirement_entries = 256;
  std::uint32_t max_allowlist_entries = 4096;

  // Evidence
  std::uint32_t max_policy_records = 1024;
  std::uint32_t max_budget_records = 1024;
  std::uint32_t max_slo_records = 1024;
  std::uint32_t max_cost_records = 65536;
  std::uint32_t max_compatibility_records = 4096;
  std::uint32_t max_trust_records = 4096;

  // Tenancy
  std::uint32_t max_tenants = 4096;
  std::uint32_t max_namespaces = 8192;
  std::uint32_t max_tenant_policy_bindings = 4096;

  // Transport
  std::uint32_t max_connections = 256;
  std::uint32_t max_session_threads = 512;
  std::uint32_t max_send_queue_frames = 4096;
  std::uint32_t max_outstanding_correlations = 4096;
  std::uint32_t max_frame_payload_bytes = 4u * 1024u * 1024u;
  std::uint32_t max_payload_bytes = 4u * 1024u * 1024u;
  std::uint32_t max_frame_bytes = 8u * 1024u * 1024u;

  // Serialization
  std::uint32_t max_string_bytes = 4096;
  std::uint32_t max_collection_entries = 100000;
  std::uint32_t max_metadata_entries = 256;
  std::uint32_t max_metadata_key_bytes = 128;
  std::uint32_t max_metadata_value_bytes = 2048;

  // Persistence
  std::uint64_t max_persistence_bytes = 1024ull * 1024ull * 1024ull;
  std::uint32_t max_persistence_records = 4000000;
  std::uint32_t max_temporary_files = 256;

  /// Returns an empty string when the limits are coherent, otherwise a
  /// deterministic description of the first violated relation.
  [[nodiscard]] std::string validate() const;
};

/// Returns the default limits.
[[nodiscard]] ResourceLimits default_resource_limits();

}  // namespace model_router

#endif  // MODEL_ROUTER_LIMITS_HPP
