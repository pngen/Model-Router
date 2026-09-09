// Model Router - durable state, integrity-checked persistence, conservative recovery.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_PERSISTENCE_HPP
#define MODEL_ROUTER_PERSISTENCE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "model_router/candidate.hpp"
#include "model_router/decision.hpp"
#include "model_router/evidence.hpp"
#include "model_router/limits.hpp"

namespace model_router {

/// On-disk header layout. The layout is fixed for a given format version: it is
/// never changed while the version number stays the same.
struct PersistenceHeader {
  /// "MRSTATE\0"
  char magic[8];
  std::uint32_t format_version;
  /// Size of this header in bytes.
  std::uint32_t header_bytes;
  std::uint32_t flags;
  std::uint32_t record_count;
  std::uint64_t payload_bytes;
  std::uint32_t payload_crc32c;
  /// CRC-32C over every header byte before this field.
  std::uint32_t header_crc32c;
  /// SHA-256 over the canonical semantic rendering of the payload.
  std::uint8_t semantic_sha256[32];
};

inline constexpr std::size_t persistence_header_bytes = 72;
inline constexpr char persistence_magic[8] = {'M', 'R', 'S', 'T', 'A', 'T', 'E', '\0'};

/// One persisted model record. Semantic model identity and operator-supplied
/// capability claims survive a restart; nothing here is dynamic evidence.
struct PersistedModelRecord {
  ModelDescriptor model{};

  friend bool operator==(const PersistedModelRecord&, const PersistedModelRecord&) = default;
};

/// One persisted backend registration record. Only registration identity and
/// operator-supplied capability claims survive: health, availability, readiness,
/// residency, capacity, latency, and live endpoint sessions do not.
struct PersistedBackendRecord {
  BackendId backend_id{};
  BackendGeneration backend_generation{};
  /// The most recent incarnation that was ever registered.
  BackendBootId last_boot{};
  BackendRegistrationGeneration last_registration_generation{};

  ProviderId provider_id{};
  ProviderGeneration provider_generation{};

  EndpointId endpoint_id{};
  EndpointGeneration last_endpoint_generation{};

  TrustProfileId trust_profile_id{};
  TrustGeneration trust_generation{};
  TrustDomain trust_domain{TrustDomain::UNKNOWN};

  LocalityKey locality{};
  std::uint32_t network_distance{0};

  CapabilityProfileId capability_profile_id{};
  CapabilityGeneration capability_generation{};
  CapabilityProfile capabilities{};

  std::vector<ModelBinding> model_bindings;

  bool offline_capable{false};
  Provenance provenance{Provenance::SYNTHETIC};
  UnixMillis registered_at_unix_millis{0};

  friend bool operator==(const PersistedBackendRecord&, const PersistedBackendRecord&) = default;
};

/// A fenced backend incarnation. Fenced boots are durable history and may never
/// become authoritative again, in any process, at any epoch.
struct FencedBootRecord {
  BackendId backend_id{};
  BackendGeneration backend_generation{};
  BackendBootId backend_boot{};
  OutcomeCode reason{OutcomeCode::FENCED};
  UnixMillis fenced_at_unix_millis{0};

  [[nodiscard]] bool canonical_less(const FencedBootRecord& other) const noexcept;
  friend bool operator==(const FencedBootRecord&, const FencedBootRecord&) = default;
};

/// A retained historical route decision. History is inspectable and is never
/// dispatchable merely because it was once valid.
struct PersistedRouteRecord {
  RouteDecisionId decision_id{};
  RouteDecisionGeneration decision_generation{};
  RouteRequestId request_id{};
  RouteRequestGeneration request_generation{};
  CandidateKey winner{};
  std::vector<CandidateKey> fallbacks{};
  RouteAuthority authority{};
  RouteStatus status{RouteStatus::CURRENT};
  OutcomeCode code{OutcomeCode::ROUTED};
  CostEvidence cost{};
  PolicyGeneration policy_generation{};
  PriceGeneration price_generation{};
  BudgetGeneration budget_generation{};
  SLOGeneration slo_generation{};
  std::string semantic_digest;
  std::string requirement_digest;
  UnixMillis created_at_unix_millis{0};

  friend bool operator==(const PersistedRouteRecord&, const PersistedRouteRecord&) = default;
};

/// Deterministic, bounded counters that must survive a restart.
struct PersistenceCounters {
  std::uint64_t highest_identity_issued{0};
  std::uint64_t route_requests{0};
  std::uint64_t route_decisions{0};
  std::uint64_t dispatches{0};
  std::uint64_t route_rejections{0};
  std::uint64_t reroutes{0};
  std::uint64_t fallbacks_taken{0};

  friend bool operator==(const PersistenceCounters&, const PersistenceCounters&) = default;
};

/// The complete durable state of a router. Dynamic evidence is deliberately
/// absent: a restart must re-observe it rather than resurrect it.
struct RouterPersistentState {
  RouterId router_id{};
  RouterGeneration router_generation{};
  /// Highest epoch ever published by a previous process.
  RouterEpoch last_router_epoch{};
  CoordinatorEpoch last_coordinator_epoch{};
  RouterBootId last_boot{};

  ResourceLimits limits{};
  RankingWeights weights{};

  PolicyGeneration last_policy_generation{};
  PriceGeneration last_price_generation{};
  BudgetGeneration last_budget_generation{};
  SLOGeneration last_slo_generation{};
  CapabilityGeneration last_capability_generation{};
  ModelGeneration last_model_generation{};
  BackendGeneration last_backend_generation{};

  std::vector<PersistedModelRecord> models;
  std::vector<PersistedBackendRecord> backends;
  std::vector<FencedBootRecord> fenced_boots;
  std::vector<PersistedRouteRecord> routes;
  PersistenceCounters counters{};

  UnixMillis saved_at_unix_millis{0};
  UnixMillis last_started_at_unix_millis{0};

  /// Canonicalizes every list so the file bytes are independent of insertion
  /// order.
  void canonicalize();
  /// Returns an empty string when the state is coherent. \p effective_limits is
  /// the limit set to validate against; the member limits are the ones that
  /// travel with the file.
  [[nodiscard]] std::string validate(const ResourceLimits& effective_limits) const;
  /// Canonical SHA-256 digest of the semantic content.
  [[nodiscard]] std::string semantic_digest() const;
};

/// Outcome of a persistence operation.
struct PersistenceResult {
  OutcomeCode code{OutcomeCode::INTERNAL_ERROR};
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return code == OutcomeCode::ACCEPTED; }
  friend bool operator==(const PersistenceResult&, const PersistenceResult&) = default;
};

/// Result of loading durable state.
struct PersistenceLoad {
  OutcomeCode code{OutcomeCode::INTERNAL_ERROR};
  std::string detail;
  RouterPersistentState state{};
  bool has_state{false};
  std::uint32_t format_version{0};
  std::uint32_t record_count{0};

  [[nodiscard]] bool ok() const noexcept { return has_state && code == OutcomeCode::ACCEPTED; }
};

/// Durable state store.
///
/// File layout: fixed header, deterministic record payload, no trailer. The
/// file must be exactly header_bytes + payload_bytes long, so truncation and
/// trailing garbage are both rejected. An unsupported format version is
/// rejected before any ambiguous parsing occurs. Saves are atomic: the payload
/// is written to a sibling temporary file, flushed to the device, and renamed
/// over the target.
class RouterStateStore {
 public:
  /// Serializes p state to p path atomically. Existing content is replaced
  /// only after the new bytes are durable.
  [[nodiscard]] static PersistenceResult save(const RouterPersistentState& state,
                                              const std::string& path,
                                              const ResourceLimits& limits);
  /// Loads and fully validates p path.
  [[nodiscard]] static PersistenceLoad load(const std::string& path,
                                            const ResourceLimits& limits);
  /// Validates a file without retaining its decoded state.
  [[nodiscard]] static PersistenceResult validate_file(const std::string& path,
                                                       const ResourceLimits& limits);
  /// Encodes p state into a byte image. Exposed for tests and tooling.
  [[nodiscard]] static PersistenceResult encode(const RouterPersistentState& state,
                                                const ResourceLimits& limits,
                                                std::vector<std::uint8_t>* out_bytes,
                                                std::uint32_t* out_records);
  /// Decodes a byte image produced by encode().
  [[nodiscard]] static PersistenceLoad decode(const std::vector<std::uint8_t>& bytes,
                                              const ResourceLimits& limits);
};

}  // namespace model_router

#endif  // MODEL_ROUTER_PERSISTENCE_HPP
