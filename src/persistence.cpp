// Model Router - versioned, integrity-checked durable state.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// File layout: a fixed 72-byte header followed by a deterministic record
// payload. The file must be exactly header_bytes + payload_bytes long, so both
// truncation and trailing garbage are rejected. The format version is checked
// before any ambiguous parsing. Saves are atomic: the image is written to a
// sibling temporary file, flushed to the device, and moved over the target.

#include "model_router/persistence.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "model_router/detail/crc32c.hpp"
#include "model_router/detail/sha256.hpp"
#include "model_router/version.hpp"

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace model_router {
namespace {

constexpr std::uint16_t kRecordMeta = 1;
constexpr std::uint16_t kRecordModel = 2;
constexpr std::uint16_t kRecordBackend = 3;
constexpr std::uint16_t kRecordFenced = 4;
constexpr std::uint16_t kRecordRoute = 5;

class ByteWriter {
 public:
  explicit ByteWriter(const ResourceLimits& limits) : limits_(limits) {}

  void put_u8(std::uint8_t value) { bytes_.push_back(value); }
  void put_u16(std::uint16_t value) {
    put_u8(static_cast<std::uint8_t>(value & 0xFFu));
    put_u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  }
  void put_u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      put_u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }
  void put_u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      put_u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }
  void put_i64(std::int64_t value) { put_u64(static_cast<std::uint64_t>(value)); }
  void put_bool(bool value) { put_u8(value ? 1u : 0u); }
  void put_string(std::string_view value) {
    if (!ok_ || value.size() > limits_.max_string_bytes) {
      fail();
      return;
    }
    put_u32(static_cast<std::uint32_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }
  template <class Id>
  void put_id(Id id) {
    put_u64(id.value());
  }
  template <class Gen>
  void put_generation(Gen generation) {
    put_u64(generation.value());
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

 private:
  void fail() { ok_ = false; }

  ResourceLimits limits_;
  std::vector<std::uint8_t> bytes_;
  bool ok_{true};
};

class ByteReader {
 public:
  ByteReader(std::span<const std::uint8_t> bytes, const ResourceLimits& limits)
      : bytes_(bytes), limits_(limits) {}

  [[nodiscard]] std::uint8_t get_u8() {
    if (!require(1)) {
      return 0;
    }
    return bytes_[offset_++];
  }
  [[nodiscard]] std::uint16_t get_u16() {
    std::uint16_t value = 0;
    for (int shift = 0; shift < 16; shift += 8) {
      value |= static_cast<std::uint16_t>(get_u8()) << shift;
    }
    return value;
  }
  [[nodiscard]] std::uint32_t get_u32() {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      value |= static_cast<std::uint32_t>(get_u8()) << shift;
    }
    return value;
  }
  [[nodiscard]] std::uint64_t get_u64() {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      value |= static_cast<std::uint64_t>(get_u8()) << shift;
    }
    return value;
  }
  [[nodiscard]] std::int64_t get_i64() { return static_cast<std::int64_t>(get_u64()); }
  [[nodiscard]] bool get_bool() { return get_u8() != 0; }
  [[nodiscard]] std::string get_string() {
    const std::uint32_t size = get_u32();
    if (!ok_ || size > limits_.max_string_bytes) {
      fail();
      return {};
    }
    if (!require(size)) {
      return {};
    }
    std::string out(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return out;
  }
  template <class Id>
  [[nodiscard]] Id get_id() {
    return Id(get_u64());
  }
  template <class Gen>
  [[nodiscard]] Gen get_generation() {
    return Gen(get_u64());
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
  [[nodiscard]] bool exhausted() const noexcept { return offset_ == bytes_.size(); }

 private:
  [[nodiscard]] bool require(std::size_t count) {
    if (!ok_ || remaining() < count) {
      fail();
      return false;
    }
    return true;
  }
  void fail() { ok_ = false; }

  std::span<const std::uint8_t> bytes_;
  ResourceLimits limits_;
  std::size_t offset_{0};
  bool ok_{true};
};

void write_capability(ByteWriter& writer, const CapabilityEvidence& evidence) {
  writer.put_string(evidence.key.value());
  writer.put_u8(static_cast<std::uint8_t>(evidence.state));
  writer.put_generation(evidence.generation);
  writer.put_id(evidence.profile_id);
  writer.put_generation(evidence.model_generation);
  writer.put_generation(evidence.backend_generation);
  writer.put_generation(evidence.backend_boot);
  writer.put_i64(evidence.observed_at_unix_millis);
  writer.put_i64(evidence.expires_at_unix_millis);
  writer.put_string(evidence.source);
  writer.put_string(evidence.detail);
}

[[nodiscard]] bool read_capability(ByteReader& reader, CapabilityEvidence* evidence) {
  evidence->key = CapabilityKey(reader.get_string());
  evidence->state = static_cast<CapabilityState>(reader.get_u8());
  evidence->generation = reader.get_generation<CapabilityGeneration>();
  evidence->profile_id = reader.get_id<CapabilityProfileId>();
  evidence->model_generation = reader.get_generation<ModelGeneration>();
  evidence->backend_generation = reader.get_generation<BackendGeneration>();
  evidence->backend_boot = reader.get_generation<BackendBootId>();
  evidence->observed_at_unix_millis = reader.get_i64();
  evidence->expires_at_unix_millis = reader.get_i64();
  evidence->source = reader.get_string();
  evidence->detail = reader.get_string();
  return reader.ok() && static_cast<std::uint8_t>(evidence->state) <
                             static_cast<std::uint8_t>(CapabilityState::kCount);
}

void write_profile(ByteWriter& writer, const CapabilityProfile& profile) {
  writer.put_id(profile.id());
  writer.put_generation(profile.generation());
  writer.put_u32(static_cast<std::uint32_t>(profile.entries().size()));
  for (const CapabilityEvidence& entry : profile.entries()) {
    write_capability(writer, entry);
  }
}

[[nodiscard]] bool read_profile(ByteReader& reader, const ResourceLimits& limits,
                                CapabilityProfile* profile) {
  const CapabilityProfileId id = reader.get_id<CapabilityProfileId>();
  const CapabilityGeneration generation = reader.get_generation<CapabilityGeneration>();
  const std::uint32_t count = reader.get_u32();
  if (!reader.ok() || count > limits.max_capability_entries) {
    return false;
  }
  CapabilityProfile parsed(id, generation);
  for (std::uint32_t index = 0; index < count; ++index) {
    CapabilityEvidence evidence;
    if (!read_capability(reader, &evidence)) {
      return false;
    }
    if (!parsed.set(std::move(evidence), limits.max_capability_entries)) {
      return false;
    }
  }
  *profile = std::move(parsed);
  return true;
}

void write_cost(ByteWriter& writer, const CostEvidence& cost) {
  writer.put_id(cost.evidence_id);
  writer.put_generation(cost.price_generation);
  writer.put_id(cost.backend_id);
  writer.put_id(cost.model_id);
  writer.put_string(cost.unit.currency);
  writer.put_string(cost.unit.basis);
  writer.put_i64(cost.input_micros_per_unit);
  writer.put_i64(cost.output_micros_per_unit);
  writer.put_i64(cost.request_minimum_micros);
  writer.put_i64(cost.cache_read_micros_per_unit);
  writer.put_i64(cost.cache_write_micros_per_unit);
  writer.put_i64(cost.estimated_total_micros);
  writer.put_u8(static_cast<std::uint8_t>(cost.knownness));
  writer.put_i64(cost.effective_at_unix_millis);
  writer.put_i64(cost.expires_at_unix_millis);
  writer.put_string(cost.source);
}

[[nodiscard]] bool read_cost(ByteReader& reader, CostEvidence* cost) {
  cost->evidence_id = reader.get_id<CostEvidenceId>();
  cost->price_generation = reader.get_generation<PriceGeneration>();
  cost->backend_id = reader.get_id<BackendId>();
  cost->model_id = reader.get_id<ModelId>();
  cost->unit.currency = reader.get_string();
  cost->unit.basis = reader.get_string();
  cost->input_micros_per_unit = reader.get_i64();
  cost->output_micros_per_unit = reader.get_i64();
  cost->request_minimum_micros = reader.get_i64();
  cost->cache_read_micros_per_unit = reader.get_i64();
  cost->cache_write_micros_per_unit = reader.get_i64();
  cost->estimated_total_micros = reader.get_i64();
  cost->knownness = static_cast<CostKnownness>(reader.get_u8());
  cost->effective_at_unix_millis = reader.get_i64();
  cost->expires_at_unix_millis = reader.get_i64();
  cost->source = reader.get_string();
  return reader.ok() &&
         static_cast<std::uint8_t>(cost->knownness) <
             static_cast<std::uint8_t>(CostKnownness::kCount);
}

void write_authority(ByteWriter& writer, const RouteAuthority& authority) {
  writer.put_id(authority.router_id);
  writer.put_generation(authority.router_epoch);
  writer.put_generation(authority.coordinator_epoch);
  writer.put_id(authority.request_id);
  writer.put_generation(authority.request_generation);
  writer.put_id(authority.decision_id);
  writer.put_generation(authority.decision_generation);
  writer.put_id(authority.model_id);
  writer.put_generation(authority.model_generation);
  writer.put_generation(authority.artifact_generation);
  writer.put_id(authority.provider_id);
  writer.put_generation(authority.provider_generation);
  writer.put_id(authority.backend_id);
  writer.put_generation(authority.backend_generation);
  writer.put_generation(authority.backend_boot);
  writer.put_generation(authority.backend_registration_generation);
  writer.put_id(authority.endpoint_id);
  writer.put_generation(authority.endpoint_generation);
  writer.put_id(authority.capability_profile_id);
  writer.put_generation(authority.capability_generation);
  writer.put_id(authority.policy_id);
  writer.put_generation(authority.policy_generation);
  writer.put_id(authority.budget_id);
  writer.put_generation(authority.budget_generation);
  writer.put_id(authority.cost_evidence_id);
  writer.put_generation(authority.price_generation);
  writer.put_id(authority.slo_id);
  writer.put_generation(authority.slo_generation);
  writer.put_id(authority.compatibility_profile_id);
  writer.put_generation(authority.compatibility_generation);
  writer.put_id(authority.trust_profile_id);
  writer.put_generation(authority.trust_generation);
  writer.put_generation(authority.health_generation);
  writer.put_generation(authority.availability_generation);
  writer.put_generation(authority.readiness_generation);
  writer.put_generation(authority.residency_generation);
  writer.put_generation(authority.capacity_generation);
  writer.put_id(authority.reservation_id);
  writer.put_generation(authority.reservation_generation);
  writer.put_id(authority.tenant);
  writer.put_id(authority.name_space);
  writer.put_generation(authority.dispatch_generation);
  writer.put_i64(authority.issued_at_unix_millis);
  writer.put_i64(authority.expires_at_unix_millis);
}

void read_authority(ByteReader& reader, RouteAuthority* authority) {
  authority->router_id = reader.get_id<RouterId>();
  authority->router_epoch = reader.get_generation<RouterEpoch>();
  authority->coordinator_epoch = reader.get_generation<CoordinatorEpoch>();
  authority->request_id = reader.get_id<RouteRequestId>();
  authority->request_generation = reader.get_generation<RouteRequestGeneration>();
  authority->decision_id = reader.get_id<RouteDecisionId>();
  authority->decision_generation = reader.get_generation<RouteDecisionGeneration>();
  authority->model_id = reader.get_id<ModelId>();
  authority->model_generation = reader.get_generation<ModelGeneration>();
  authority->artifact_generation = reader.get_generation<ArtifactGeneration>();
  authority->provider_id = reader.get_id<ProviderId>();
  authority->provider_generation = reader.get_generation<ProviderGeneration>();
  authority->backend_id = reader.get_id<BackendId>();
  authority->backend_generation = reader.get_generation<BackendGeneration>();
  authority->backend_boot = reader.get_generation<BackendBootId>();
  authority->backend_registration_generation =
      reader.get_generation<BackendRegistrationGeneration>();
  authority->endpoint_id = reader.get_id<EndpointId>();
  authority->endpoint_generation = reader.get_generation<EndpointGeneration>();
  authority->capability_profile_id = reader.get_id<CapabilityProfileId>();
  authority->capability_generation = reader.get_generation<CapabilityGeneration>();
  authority->policy_id = reader.get_id<PolicyId>();
  authority->policy_generation = reader.get_generation<PolicyGeneration>();
  authority->budget_id = reader.get_id<BudgetId>();
  authority->budget_generation = reader.get_generation<BudgetGeneration>();
  authority->cost_evidence_id = reader.get_id<CostEvidenceId>();
  authority->price_generation = reader.get_generation<PriceGeneration>();
  authority->slo_id = reader.get_id<SLOId>();
  authority->slo_generation = reader.get_generation<SLOGeneration>();
  authority->compatibility_profile_id = reader.get_id<CompatibilityProfileId>();
  authority->compatibility_generation = reader.get_generation<CompatibilityGeneration>();
  authority->trust_profile_id = reader.get_id<TrustProfileId>();
  authority->trust_generation = reader.get_generation<TrustGeneration>();
  authority->health_generation = reader.get_generation<HealthGeneration>();
  authority->availability_generation = reader.get_generation<AvailabilityGeneration>();
  authority->readiness_generation = reader.get_generation<ReadinessGeneration>();
  authority->residency_generation = reader.get_generation<ResidencyGeneration>();
  authority->capacity_generation = reader.get_generation<CapacityGeneration>();
  authority->reservation_id = reader.get_id<ReservationId>();
  authority->reservation_generation = reader.get_generation<ReservationGeneration>();
  authority->tenant = reader.get_id<TenantId>();
  authority->name_space = reader.get_id<NamespaceId>();
  authority->dispatch_generation = reader.get_generation<DispatchGeneration>();
  authority->issued_at_unix_millis = reader.get_i64();
  authority->expires_at_unix_millis = reader.get_i64();
}

void write_candidate_key(ByteWriter& writer, const CandidateKey& key) {
  writer.put_id(key.model_id);
  writer.put_generation(key.model_generation);
  writer.put_generation(key.artifact_generation);
  writer.put_id(key.backend_id);
  writer.put_generation(key.backend_generation);
  writer.put_generation(key.backend_boot);
  writer.put_id(key.endpoint_id);
  writer.put_generation(key.endpoint_generation);
}

void read_candidate_key(ByteReader& reader, CandidateKey* key) {
  key->model_id = reader.get_id<ModelId>();
  key->model_generation = reader.get_generation<ModelGeneration>();
  key->artifact_generation = reader.get_generation<ArtifactGeneration>();
  key->backend_id = reader.get_id<BackendId>();
  key->backend_generation = reader.get_generation<BackendGeneration>();
  key->backend_boot = reader.get_generation<BackendBootId>();
  key->endpoint_id = reader.get_id<EndpointId>();
  key->endpoint_generation = reader.get_generation<EndpointGeneration>();
}

[[nodiscard]] std::uint32_t checked_record_length(std::size_t length,
                                                  const ResourceLimits& limits) {
  if (length > limits.max_payload_bytes) {
    return 0;
  }
  return static_cast<std::uint32_t>(length);
}

void write_record(std::vector<std::uint8_t>* payload, std::uint16_t kind,
                  const std::vector<std::uint8_t>& body) {
  payload->push_back(static_cast<std::uint8_t>(kind & 0xFFu));
  payload->push_back(static_cast<std::uint8_t>((kind >> 8) & 0xFFu));
  const std::uint32_t length = static_cast<std::uint32_t>(body.size());
  for (int shift = 0; shift < 32; shift += 8) {
    payload->push_back(static_cast<std::uint8_t>((length >> shift) & 0xFFu));
  }
  payload->insert(payload->end(), body.begin(), body.end());
}

[[nodiscard]] std::vector<std::uint8_t> canonical_text_bytes(const RouterPersistentState& state) {
  std::string text;
  text += "router=";
  text += std::to_string(state.router_id.value());
  text += ";gen=";
  text += std::to_string(state.router_generation.value());
  text += ";router_epoch=";
  text += std::to_string(state.last_router_epoch.value());
  text += ";coordinator_epoch=";
  text += std::to_string(state.last_coordinator_epoch.value());
  text += ";boot=";
  text += std::to_string(state.last_boot.value());
  text += ";saved_at=";
  text += std::to_string(state.saved_at_unix_millis);
  text += ";counters=";
  text += std::to_string(state.counters.highest_identity_issued);
  text += ",";
  text += std::to_string(state.counters.route_requests);
  text += ",";
  text += std::to_string(state.counters.route_decisions);
  text += ",";
  text += std::to_string(state.counters.dispatches);
  text += ",";
  text += std::to_string(state.counters.route_rejections);
  text += ",";
  text += std::to_string(state.counters.reroutes);
  text += ",";
  text += std::to_string(state.counters.fallbacks_taken);
  for (const PersistedModelRecord& record : state.models) {
    text += "|model=";
    text += std::to_string(record.model.model_id.value());
    text += "@";
    text += std::to_string(record.model.model_generation.value());
    text += ".";
    text += std::to_string(record.model.artifact_generation.value());
    text += ":";
    text += std::string(to_string(record.model.lifecycle));
  }
  for (const PersistedBackendRecord& record : state.backends) {
    text += "|backend=";
    text += std::to_string(record.backend_id.value());
    text += "@";
    text += std::to_string(record.backend_generation.value());
    text += ".boot=";
    text += std::to_string(record.last_boot.value());
    text += ":";
    text += std::to_string(record.model_bindings.size());
  }
  for (const FencedBootRecord& record : state.fenced_boots) {
    text += "|fenced=";
    text += std::to_string(record.backend_id.value());
    text += ".boot=";
    text += std::to_string(record.backend_boot.value());
  }
  for (const PersistedRouteRecord& record : state.routes) {
    text += "|route=";
    text += std::to_string(record.decision_id.value());
    text += ":";
    text += record.semantic_digest;
  }
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

}  // namespace

// ---------------------------------------------------------------------------
// RouterPersistentState
// ---------------------------------------------------------------------------

bool FencedBootRecord::canonical_less(const FencedBootRecord& other) const noexcept {
  if (!(backend_id == other.backend_id)) {
    return backend_id < other.backend_id;
  }
  if (!(backend_boot == other.backend_boot)) {
    return backend_boot < other.backend_boot;
  }
  if (!(backend_generation == other.backend_generation)) {
    return backend_generation < other.backend_generation;
  }
  return static_cast<std::uint16_t>(reason) < static_cast<std::uint16_t>(other.reason);
}

void RouterPersistentState::canonicalize() {
  std::sort(models.begin(), models.end(),
            [](const PersistedModelRecord& lhs, const PersistedModelRecord& rhs) {
              if (!(lhs.model.model_id == rhs.model.model_id)) {
                return lhs.model.model_id < rhs.model.model_id;
              }
              return lhs.model.model_generation < rhs.model.model_generation;
            });
  std::sort(backends.begin(), backends.end(),
            [](const PersistedBackendRecord& lhs, const PersistedBackendRecord& rhs) {
              return lhs.backend_id < rhs.backend_id;
            });
  for (PersistedBackendRecord& backend : backends) {
    std::sort(backend.model_bindings.begin(), backend.model_bindings.end(),
              [](const ModelBinding& lhs, const ModelBinding& rhs) {
                return lhs.model_id < rhs.model_id;
              });
  }
  std::sort(fenced_boots.begin(), fenced_boots.end(),
            [](const FencedBootRecord& lhs, const FencedBootRecord& rhs) {
              return lhs.canonical_less(rhs);
            });
  std::sort(routes.begin(), routes.end(),
            [](const PersistedRouteRecord& lhs, const PersistedRouteRecord& rhs) {
              return lhs.decision_id < rhs.decision_id;
            });
}

std::string RouterPersistentState::validate(const ResourceLimits& effective_limits) const {
  if (!router_id.valid()) {
    return "durable state has no router identity";
  }
  if (!router_generation.valid()) {
    return "durable state has no router generation";
  }
  if (const std::string error = effective_limits.validate(); !error.empty()) {
    return "durable limits are incoherent: " + error;
  }
  if (const std::string error = weights.validate(); !error.empty()) {
    return "durable ranking weights are incoherent: " + error;
  }
  if (models.size() > effective_limits.max_models) {
    return "durable model record count exceeds the configured limit";
  }
  if (backends.size() > effective_limits.max_backends) {
    return "durable backend record count exceeds the configured limit";
  }
  if (fenced_boots.size() > effective_limits.max_backend_incarnations) {
    return "durable fenced incarnation count exceeds the configured limit";
  }
  if (routes.size() > effective_limits.max_route_history) {
    return "durable route record count exceeds the configured limit";
  }
  if (static_cast<std::uint64_t>(models.size()) + backends.size() + fenced_boots.size() +
          routes.size() >
      effective_limits.max_persistence_records) {
    return "durable record count exceeds the configured limit";
  }
  for (const PersistedModelRecord& record : models) {
    if (const std::string error = record.model.validate(); !error.empty()) {
      return "durable model record is invalid: " + error;
    }
  }
  for (const PersistedBackendRecord& record : backends) {
    if (!record.backend_id.valid() || !record.backend_generation.valid() ||
        !record.last_boot.valid()) {
      return "durable backend record has an invalid identity";
    }
    if (record.model_bindings.size() > effective_limits.max_model_bindings_per_backend) {
      return "durable backend record exceeds the model binding limit";
    }
    if (record.capabilities.entries().size() > effective_limits.max_capability_entries_per_backend) {
      return "durable backend record exceeds the capability limit";
    }
  }
  for (const FencedBootRecord& record : fenced_boots) {
    if (!record.backend_id.valid() || !record.backend_boot.valid()) {
      return "durable fenced record has an invalid identity";
    }
  }
  for (const PersistedRouteRecord& record : routes) {
    if (!record.decision_id.valid() || !record.decision_generation.valid()) {
      return "durable route record has an invalid identity";
    }
  }
  return {};
}

std::string RouterPersistentState::semantic_digest() const {
  return detail::to_hex(detail::sha256(canonical_text_bytes(*this)));
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

PersistenceResult RouterStateStore::encode(const RouterPersistentState& state,
                                           const ResourceLimits& limits,
                                           std::vector<std::uint8_t>* out_bytes,
                                           std::uint32_t* out_records) {
  PersistenceResult result;
  if (out_bytes == nullptr) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.detail = "encode output pointer is null";
    return result;
  }
  RouterPersistentState canonical = state;
  canonical.canonicalize();
  if (const std::string error = canonical.validate(limits); !error.empty()) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.detail = error;
    return result;
  }

  std::vector<std::uint8_t> payload;
  std::uint32_t records = 0;

  // Meta record.
  {
    ByteWriter writer(limits);
    writer.put_id(canonical.router_id);
    writer.put_generation(canonical.router_generation);
    writer.put_generation(canonical.last_router_epoch);
    writer.put_generation(canonical.last_coordinator_epoch);
    writer.put_generation(canonical.last_boot);
    writer.put_generation(canonical.last_policy_generation);
    writer.put_generation(canonical.last_price_generation);
    writer.put_generation(canonical.last_budget_generation);
    writer.put_generation(canonical.last_slo_generation);
    writer.put_generation(canonical.last_capability_generation);
    writer.put_generation(canonical.last_model_generation);
    writer.put_generation(canonical.last_backend_generation);
    writer.put_i64(canonical.saved_at_unix_millis);
    writer.put_i64(canonical.last_started_at_unix_millis);
    writer.put_u64(canonical.counters.highest_identity_issued);
    writer.put_u64(canonical.counters.route_requests);
    writer.put_u64(canonical.counters.route_decisions);
    writer.put_u64(canonical.counters.dispatches);
    writer.put_u64(canonical.counters.route_rejections);
    writer.put_u64(canonical.counters.reroutes);
    writer.put_u64(canonical.counters.fallbacks_taken);
    writer.put_u32(canonical.limits.max_models);
    writer.put_u32(canonical.limits.max_backends);
    writer.put_u32(canonical.limits.max_route_history);
    writer.put_u32(canonical.limits.max_fallback_candidates);
    writer.put_u32(canonical.limits.max_explanation_factors);
    writer.put_u32(canonical.limits.max_string_bytes);
    writer.put_u32(canonical.limits.max_capability_entries_per_backend);
    writer.put_u32(canonical.limits.max_model_bindings_per_backend);
    if (!writer.ok()) {
      result.code = OutcomeCode::REJECT_LIMIT;
      result.detail = "meta record exceeds the configured bounds";
      return result;
    }
    write_record(&payload, kRecordMeta, writer.bytes());
    ++records;
  }

  for (const PersistedModelRecord& record : canonical.models) {
    ByteWriter writer(limits);
    const ModelDescriptor& model = record.model;
    writer.put_id(model.model_id);
    writer.put_generation(model.model_generation);
    writer.put_id(model.model_version_id);
    writer.put_generation(model.artifact_generation);
    writer.put_id(model.family_id);
    writer.put_u32(model.context_limit_tokens);
    writer.put_u32(model.max_output_tokens);
    writer.put_u32(model.input_modalities.bits());
    writer.put_u32(model.output_modalities.bits());
    writer.put_u32(model.quality_class);
    writer.put_u8(static_cast<std::uint8_t>(model.lifecycle));
    writer.put_id(model.capability_profile_id);
    writer.put_generation(model.capability_generation);
    write_profile(writer, model.capabilities);
    writer.put_u8(static_cast<std::uint8_t>(model.provenance));
    writer.put_i64(model.registered_at_unix_millis);
    writer.put_i64(model.expires_at_unix_millis);
    writer.put_string(model.display_name);
    if (!writer.ok()) {
      result.code = OutcomeCode::REJECT_LIMIT;
      result.detail = "model record exceeds the configured bounds";
      return result;
    }
    write_record(&payload, kRecordModel, writer.bytes());
    ++records;
  }

  for (const PersistedBackendRecord& record : canonical.backends) {
    ByteWriter writer(limits);
    writer.put_id(record.backend_id);
    writer.put_generation(record.backend_generation);
    writer.put_generation(record.last_boot);
    writer.put_generation(record.last_registration_generation);
    writer.put_id(record.provider_id);
    writer.put_generation(record.provider_generation);
    writer.put_id(record.endpoint_id);
    writer.put_generation(record.last_endpoint_generation);
    writer.put_id(record.trust_profile_id);
    writer.put_generation(record.trust_generation);
    writer.put_u8(static_cast<std::uint8_t>(record.trust_domain));
    writer.put_string(record.locality.value());
    writer.put_u32(record.network_distance);
    writer.put_id(record.capability_profile_id);
    writer.put_generation(record.capability_generation);
    write_profile(writer, record.capabilities);
    writer.put_u32(static_cast<std::uint32_t>(record.model_bindings.size()));
    for (const ModelBinding& binding : record.model_bindings) {
      writer.put_id(binding.model_id);
      writer.put_generation(binding.model_generation);
      writer.put_generation(binding.artifact_generation);
      writer.put_u32(binding.context_limit_tokens);
      writer.put_u32(binding.quality_class);
      write_cost(writer, binding.cost);
      writer.put_i64(binding.bound_at_unix_millis);
    }
    writer.put_bool(record.offline_capable);
    writer.put_u8(static_cast<std::uint8_t>(record.provenance));
    writer.put_i64(record.registered_at_unix_millis);
    if (!writer.ok()) {
      result.code = OutcomeCode::REJECT_LIMIT;
      result.detail = "backend record exceeds the configured bounds";
      return result;
    }
    write_record(&payload, kRecordBackend, writer.bytes());
    ++records;
  }

  for (const FencedBootRecord& record : canonical.fenced_boots) {
    ByteWriter writer(limits);
    writer.put_id(record.backend_id);
    writer.put_generation(record.backend_generation);
    writer.put_generation(record.backend_boot);
    writer.put_u16(static_cast<std::uint16_t>(record.reason));
    writer.put_i64(record.fenced_at_unix_millis);
    if (!writer.ok()) {
      result.code = OutcomeCode::REJECT_LIMIT;
      result.detail = "fenced record exceeds the configured bounds";
      return result;
    }
    write_record(&payload, kRecordFenced, writer.bytes());
    ++records;
  }

  for (const PersistedRouteRecord& record : canonical.routes) {
    ByteWriter writer(limits);
    writer.put_id(record.decision_id);
    writer.put_generation(record.decision_generation);
    writer.put_id(record.request_id);
    writer.put_generation(record.request_generation);
    write_candidate_key(writer, record.winner);
    writer.put_u32(static_cast<std::uint32_t>(record.fallbacks.size()));
    for (const CandidateKey& key : record.fallbacks) {
      write_candidate_key(writer, key);
    }
    write_authority(writer, record.authority);
    writer.put_u8(static_cast<std::uint8_t>(record.status));
    writer.put_u16(static_cast<std::uint16_t>(record.code));
    write_cost(writer, record.cost);
    writer.put_generation(record.policy_generation);
    writer.put_generation(record.price_generation);
    writer.put_generation(record.budget_generation);
    writer.put_generation(record.slo_generation);
    writer.put_string(record.semantic_digest);
    writer.put_string(record.requirement_digest);
    writer.put_i64(record.created_at_unix_millis);
    if (!writer.ok()) {
      result.code = OutcomeCode::REJECT_LIMIT;
      result.detail = "route record exceeds the configured bounds";
      return result;
    }
    write_record(&payload, kRecordRoute, writer.bytes());
    ++records;
  }

  if (records > limits.max_persistence_records) {
    result.code = OutcomeCode::REJECT_LIMIT;
    result.detail = "record count exceeds the configured persistence limit";
    return result;
  }

  const std::vector<std::uint8_t> text = canonical_text_bytes(canonical);
  const auto digest = detail::sha256(text);

  if (persistence_header_bytes + payload.size() > limits.max_persistence_bytes) {
    result.code = OutcomeCode::REJECT_LIMIT;
    result.detail = "persistence image exceeds the configured byte limit";
    return result;
  }

  std::vector<std::uint8_t> image(persistence_header_bytes + payload.size(), 0);
  std::size_t offset = 0;
  const auto put_u32_at = [&image](std::size_t at, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      image[at + static_cast<std::size_t>(shift / 8)] =
          static_cast<std::uint8_t>((value >> shift) & 0xFFu);
    }
  };
  const auto put_u64_at = [&image](std::size_t at, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      image[at + static_cast<std::size_t>(shift / 8)] =
          static_cast<std::uint8_t>((value >> shift) & 0xFFu);
    }
  };

  std::memcpy(image.data(), persistence_magic, sizeof(persistence_magic));
  offset = sizeof(persistence_magic);
  put_u32_at(offset, persistence_format_version);
  offset += 4;
  put_u32_at(offset, static_cast<std::uint32_t>(persistence_header_bytes));
  offset += 4;
  put_u32_at(offset, 0u);  // flags
  offset += 4;
  put_u32_at(offset, records);
  offset += 4;
  put_u64_at(offset, payload.size());
  offset += 8;
  put_u32_at(offset, detail::crc32c(payload));
  offset += 4;
  put_u32_at(offset, 0u);  // header_crc32c placeholder
  offset += 4;
  std::memcpy(image.data() + offset, digest.data(), digest.size());
  offset += digest.size();

  // The header checksum covers every header byte before the checksum field
  // itself (bytes [0, 36)), exactly as persistence.hpp documents.
  const std::uint32_t header_crc =
      detail::crc32c(std::span<const std::uint8_t>(image.data(), 36));
  put_u32_at(36, header_crc);
  std::memcpy(image.data() + persistence_header_bytes, payload.data(), payload.size());

  *out_bytes = std::move(image);
  if (out_records != nullptr) {
    *out_records = records;
  }
  result.code = OutcomeCode::ACCEPTED;
  return result;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

PersistenceLoad RouterStateStore::decode(const std::vector<std::uint8_t>& bytes,
                                         const ResourceLimits& limits) {
  PersistenceLoad load;
  if (bytes.size() < persistence_header_bytes) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = "persistence image is shorter than its header";
    return load;
  }
  if (bytes.size() > limits.max_persistence_bytes) {
    load.code = OutcomeCode::REJECT_LIMIT;
    load.detail = "persistence image exceeds the configured byte limit";
    return load;
  }
  const auto get_u32_at = [&bytes](std::size_t at) {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      value |= static_cast<std::uint32_t>(bytes[at + static_cast<std::size_t>(shift / 8)]) << shift;
    }
    return value;
  };
  const auto get_u64_at = [&bytes](std::size_t at) {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      value |= static_cast<std::uint64_t>(bytes[at + static_cast<std::size_t>(shift / 8)]) << shift;
    }
    return value;
  };

  if (std::memcmp(bytes.data(), persistence_magic, sizeof(persistence_magic)) != 0) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = "persistence magic does not match";
    return load;
  }
  const std::uint32_t version = get_u32_at(8);
  if (version != persistence_format_version) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = "unsupported persistence format version " + std::to_string(version);
    load.format_version = version;
    return load;
  }
  load.format_version = version;
  const std::uint32_t header_bytes = get_u32_at(12);
  if (header_bytes != persistence_header_bytes) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = "persistence header size is not the format's header size";
    return load;
  }
  const std::uint32_t flags = get_u32_at(16);
  if (flags != 0) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = "persistence header carries unknown flags";
    return load;
  }
  const std::uint32_t records = get_u32_at(20);
  const std::uint64_t payload_bytes = get_u64_at(24);
  const std::uint32_t payload_crc = get_u32_at(32);
  const std::uint32_t stored_header_crc = get_u32_at(36);
  const std::uint32_t computed_header_crc =
      detail::crc32c(std::span<const std::uint8_t>(bytes.data(), 36));
  if (stored_header_crc != computed_header_crc) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = "persistence header checksum mismatch";
    return load;
  }
  if (payload_bytes > limits.max_payload_bytes) {
    load.code = OutcomeCode::REJECT_LIMIT;
    load.detail = "persistence payload length exceeds the configured limit";
    return load;
  }
  const std::uint64_t expected = static_cast<std::uint64_t>(persistence_header_bytes) + payload_bytes;
  if (expected != bytes.size()) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = bytes.size() < expected ? "persistence image is truncated"
                                          : "persistence image has trailing garbage";
    return load;
  }
  const std::span<const std::uint8_t> payload(bytes.data() + persistence_header_bytes,
                                              static_cast<std::size_t>(payload_bytes));
  if (detail::crc32c(payload) != payload_crc) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = "persistence payload checksum mismatch";
    return load;
  }

  RouterPersistentState state;
  std::uint32_t seen_records = 0;
  std::size_t offset = 0;
  bool have_meta = false;

  while (offset < payload.size()) {
    if (payload.size() - offset < 6) {
      load.code = OutcomeCode::REJECT_INVALID;
      load.detail = "persistence payload ends inside a record header";
      return load;
    }
    const std::uint16_t kind = static_cast<std::uint16_t>(
        payload[offset] | (static_cast<std::uint16_t>(payload[offset + 1]) << 8));
    const std::uint32_t length = static_cast<std::uint32_t>(
        payload[offset + 2] | (static_cast<std::uint32_t>(payload[offset + 3]) << 8) |
        (static_cast<std::uint32_t>(payload[offset + 4]) << 16) |
        (static_cast<std::uint32_t>(payload[offset + 5]) << 24));
    offset += 6;
    if (length > payload.size() - offset) {
      load.code = OutcomeCode::REJECT_INVALID;
      load.detail = "persistence record length exceeds the remaining payload";
      return load;
    }
    const std::span<const std::uint8_t> body(payload.data() + offset, length);
    offset += length;
    ++seen_records;
    if (seen_records > limits.max_persistence_records) {
      load.code = OutcomeCode::REJECT_LIMIT;
      load.detail = "persistence record count exceeds the configured limit";
      return load;
    }

    ByteReader reader(body, limits);
    switch (kind) {
      case kRecordMeta: {
        if (have_meta) {
          load.code = OutcomeCode::REJECT_INVALID;
          load.detail = "persistence image contains more than one meta record";
          return load;
        }
        state.router_id = reader.get_id<RouterId>();
        state.router_generation = reader.get_generation<RouterGeneration>();
        state.last_router_epoch = reader.get_generation<RouterEpoch>();
        state.last_coordinator_epoch = reader.get_generation<CoordinatorEpoch>();
        state.last_boot = reader.get_generation<RouterBootId>();
        state.last_policy_generation = reader.get_generation<PolicyGeneration>();
        state.last_price_generation = reader.get_generation<PriceGeneration>();
        state.last_budget_generation = reader.get_generation<BudgetGeneration>();
        state.last_slo_generation = reader.get_generation<SLOGeneration>();
        state.last_capability_generation = reader.get_generation<CapabilityGeneration>();
        state.last_model_generation = reader.get_generation<ModelGeneration>();
        state.last_backend_generation = reader.get_generation<BackendGeneration>();
        state.saved_at_unix_millis = reader.get_i64();
        state.last_started_at_unix_millis = reader.get_i64();
        state.counters.highest_identity_issued = reader.get_u64();
        state.counters.route_requests = reader.get_u64();
        state.counters.route_decisions = reader.get_u64();
        state.counters.dispatches = reader.get_u64();
        state.counters.route_rejections = reader.get_u64();
        state.counters.reroutes = reader.get_u64();
        state.counters.fallbacks_taken = reader.get_u64();
        state.limits.max_models = reader.get_u32();
        state.limits.max_backends = reader.get_u32();
        state.limits.max_route_history = reader.get_u32();
        state.limits.max_fallback_candidates = reader.get_u32();
        state.limits.max_explanation_factors = reader.get_u32();
        state.limits.max_string_bytes = reader.get_u32();
        state.limits.max_capability_entries_per_backend = reader.get_u32();
        state.limits.max_model_bindings_per_backend = reader.get_u32();
        have_meta = true;
        break;
      }
      case kRecordModel: {
        PersistedModelRecord record;
        ModelDescriptor& model = record.model;
        model.model_id = reader.get_id<ModelId>();
        model.model_generation = reader.get_generation<ModelGeneration>();
        model.model_version_id = reader.get_id<ModelVersionId>();
        model.artifact_generation = reader.get_generation<ArtifactGeneration>();
        model.family_id = reader.get_id<ModelFamilyId>();
        model.context_limit_tokens = reader.get_u32();
        model.max_output_tokens = reader.get_u32();
        model.input_modalities = ModalitySet(reader.get_u32());
        model.output_modalities = ModalitySet(reader.get_u32());
        model.quality_class = reader.get_u32();
        model.lifecycle = static_cast<ModelLifecycle>(reader.get_u8());
        model.capability_profile_id = reader.get_id<CapabilityProfileId>();
        model.capability_generation = reader.get_generation<CapabilityGeneration>();
        if (!read_profile(reader, limits, &model.capabilities)) {
          load.code = OutcomeCode::REJECT_INVALID;
          load.detail = "persistence model record has an invalid capability profile";
          return load;
        }
        model.provenance = static_cast<Provenance>(reader.get_u8());
        model.registered_at_unix_millis = reader.get_i64();
        model.expires_at_unix_millis = reader.get_i64();
        model.display_name = reader.get_string();
        if (!reader.ok() || !reader.exhausted()) {
          load.code = OutcomeCode::REJECT_INVALID;
          load.detail = "persistence model record is malformed";
          return load;
        }
        state.models.push_back(std::move(record));
        break;
      }
      case kRecordBackend: {
        PersistedBackendRecord record;
        record.backend_id = reader.get_id<BackendId>();
        record.backend_generation = reader.get_generation<BackendGeneration>();
        record.last_boot = reader.get_generation<BackendBootId>();
        record.last_registration_generation =
            reader.get_generation<BackendRegistrationGeneration>();
        record.provider_id = reader.get_id<ProviderId>();
        record.provider_generation = reader.get_generation<ProviderGeneration>();
        record.endpoint_id = reader.get_id<EndpointId>();
        record.last_endpoint_generation = reader.get_generation<EndpointGeneration>();
        record.trust_profile_id = reader.get_id<TrustProfileId>();
        record.trust_generation = reader.get_generation<TrustGeneration>();
        record.trust_domain = static_cast<TrustDomain>(reader.get_u8());
        record.locality = LocalityKey(reader.get_string());
        record.network_distance = reader.get_u32();
        record.capability_profile_id = reader.get_id<CapabilityProfileId>();
        record.capability_generation = reader.get_generation<CapabilityGeneration>();
        if (!read_profile(reader, limits, &record.capabilities)) {
          load.code = OutcomeCode::REJECT_INVALID;
          load.detail = "persistence backend record has an invalid capability profile";
          return load;
        }
        const std::uint32_t bindings = reader.get_u32();
        if (bindings > limits.max_model_bindings_per_backend) {
          load.code = OutcomeCode::REJECT_LIMIT;
          load.detail = "persistence backend record exceeds the model binding limit";
          return load;
        }
        for (std::uint32_t index = 0; index < bindings; ++index) {
          ModelBinding binding;
          binding.model_id = reader.get_id<ModelId>();
          binding.model_generation = reader.get_generation<ModelGeneration>();
          binding.artifact_generation = reader.get_generation<ArtifactGeneration>();
          binding.context_limit_tokens = reader.get_u32();
          binding.quality_class = reader.get_u32();
          if (!read_cost(reader, &binding.cost)) {
            load.code = OutcomeCode::REJECT_INVALID;
            load.detail = "persistence backend record has invalid cost evidence";
            return load;
          }
          binding.bound_at_unix_millis = reader.get_i64();
          record.model_bindings.push_back(std::move(binding));
        }
        record.offline_capable = reader.get_bool();
        record.provenance = static_cast<Provenance>(reader.get_u8());
        record.registered_at_unix_millis = reader.get_i64();
        if (!reader.ok() || !reader.exhausted()) {
          load.code = OutcomeCode::REJECT_INVALID;
          load.detail = "persistence backend record is malformed";
          return load;
        }
        state.backends.push_back(std::move(record));
        break;
      }
      case kRecordFenced: {
        FencedBootRecord record;
        record.backend_id = reader.get_id<BackendId>();
        record.backend_generation = reader.get_generation<BackendGeneration>();
        record.backend_boot = reader.get_generation<BackendBootId>();
        record.reason = static_cast<OutcomeCode>(reader.get_u16());
        record.fenced_at_unix_millis = reader.get_i64();
        if (!reader.ok() || !reader.exhausted()) {
          load.code = OutcomeCode::REJECT_INVALID;
          load.detail = "persistence fenced record is malformed";
          return load;
        }
        state.fenced_boots.push_back(std::move(record));
        break;
      }
      case kRecordRoute: {
        PersistedRouteRecord record;
        record.decision_id = reader.get_id<RouteDecisionId>();
        record.decision_generation = reader.get_generation<RouteDecisionGeneration>();
        record.request_id = reader.get_id<RouteRequestId>();
        record.request_generation = reader.get_generation<RouteRequestGeneration>();
        read_candidate_key(reader, &record.winner);
        const std::uint32_t fallbacks = reader.get_u32();
        if (fallbacks > limits.max_fallback_candidates) {
          load.code = OutcomeCode::REJECT_LIMIT;
          load.detail = "persistence route record exceeds the fallback limit";
          return load;
        }
        for (std::uint32_t index = 0; index < fallbacks; ++index) {
          CandidateKey key;
          read_candidate_key(reader, &key);
          record.fallbacks.push_back(key);
        }
        read_authority(reader, &record.authority);
        record.status = static_cast<RouteStatus>(reader.get_u8());
        record.code = static_cast<OutcomeCode>(reader.get_u16());
        if (!read_cost(reader, &record.cost)) {
          load.code = OutcomeCode::REJECT_INVALID;
          load.detail = "persistence route record has invalid cost evidence";
          return load;
        }
        record.policy_generation = reader.get_generation<PolicyGeneration>();
        record.price_generation = reader.get_generation<PriceGeneration>();
        record.budget_generation = reader.get_generation<BudgetGeneration>();
        record.slo_generation = reader.get_generation<SLOGeneration>();
        record.semantic_digest = reader.get_string();
        record.requirement_digest = reader.get_string();
        record.created_at_unix_millis = reader.get_i64();
        if (!reader.ok() || !reader.exhausted()) {
          load.code = OutcomeCode::REJECT_INVALID;
          load.detail = "persistence route record is malformed";
          return load;
        }
        state.routes.push_back(std::move(record));
        break;
      }
      default:
        load.code = OutcomeCode::REJECT_INVALID;
        load.detail = "persistence record has an unknown record kind";
        return load;
    }
  }

  if (!have_meta) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = "persistence image has no meta record";
    return load;
  }
  if (seen_records != records) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = "persistence record count does not match the header";
    return load;
  }

  state.canonicalize();
  if (const std::string error = state.validate(limits); !error.empty()) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = error;
    return load;
  }
  const std::vector<std::uint8_t> text = canonical_text_bytes(state);
  const auto digest = detail::sha256(text);
  if (std::memcmp(digest.data(), bytes.data() + 40, digest.size()) != 0) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = "persistence semantic digest does not match the decoded state";
    return load;
  }

  load.state = std::move(state);
  load.has_state = true;
  load.record_count = records;
  load.code = OutcomeCode::ACCEPTED;
  return load;
}

// ---------------------------------------------------------------------------
// Atomic file operations
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] bool write_image_atomic(const std::string& path,
                                      const std::vector<std::uint8_t>& image,
                                      std::string* error) {
  const std::string temporary = path + ".tmp";
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, temporary.c_str(), "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(temporary.c_str(), "wb");
#endif
  if (file == nullptr) {
    *error = "cannot open the temporary state file for writing";
    return false;
  }
  const std::size_t written = std::fwrite(image.data(), 1, image.size(), file);
  if (written != image.size()) {
    std::fclose(file);
    std::remove(temporary.c_str());
    *error = "short write while writing the state image";
    return false;
  }
  if (std::fflush(file) != 0) {
    std::fclose(file);
    std::remove(temporary.c_str());
    *error = "flush failed while writing the state image";
    return false;
  }
#if defined(_WIN32)
  if (_commit(_fileno(file)) != 0) {
    std::fclose(file);
    std::remove(temporary.c_str());
    *error = "durable flush failed while writing the state image";
    return false;
  }
#else
  if (::fsync(fileno(file)) != 0) {
    std::fclose(file);
    std::remove(temporary.c_str());
    *error = "durable flush failed while writing the state image";
    return false;
  }
#endif
  if (std::fclose(file) != 0) {
    std::remove(temporary.c_str());
    *error = "close failed while writing the state image";
    return false;
  }
#if defined(_WIN32)
  if (MoveFileExA(temporary.c_str(), path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    std::remove(temporary.c_str());
    *error = "atomic replacement of the state file failed";
    return false;
  }
#else
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    *error = "atomic replacement of the state file failed";
    return false;
  }
#endif
  return true;
}

[[nodiscard]] bool read_image(const std::string& path, const ResourceLimits& limits,
                              std::vector<std::uint8_t>* out, std::string* error) {
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, path.c_str(), "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "rb");
#endif
  if (file == nullptr) {
    *error = "cannot open the state file for reading";
    return false;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    *error = "cannot determine the state file size";
    return false;
  }
  const long size = std::ftell(file);
  if (size < 0) {
    std::fclose(file);
    *error = "cannot determine the state file size";
    return false;
  }
  if (static_cast<std::uint64_t>(size) > limits.max_persistence_bytes) {
    std::fclose(file);
    *error = "state file exceeds the configured byte limit";
    return false;
  }
  std::rewind(file);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size > 0) {
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    if (read != bytes.size()) {
      std::fclose(file);
      *error = "short read while reading the state image";
      return false;
    }
  }
  std::fclose(file);
  *out = std::move(bytes);
  return true;
}

}  // namespace

PersistenceResult RouterStateStore::save(const RouterPersistentState& state, const std::string& path,
                                         const ResourceLimits& limits) {
  PersistenceResult result;
  if (path.empty()) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.detail = "persistence path is empty";
    return result;
  }
  std::vector<std::uint8_t> image;
  std::uint32_t records = 0;
  RouterPersistentState canonical = state;
  canonical.canonicalize();
  result = encode(canonical, limits, &image, &records);
  if (!result.ok()) {
    return result;
  }
  std::string error;
  if (!write_image_atomic(path, image, &error)) {
    result.code = OutcomeCode::INTERNAL_ERROR;
    result.detail = error;
    return result;
  }
  result.code = OutcomeCode::ACCEPTED;
  result.detail = "records=" + std::to_string(records) + " bytes=" + std::to_string(image.size());
  return result;
}

PersistenceLoad RouterStateStore::load(const std::string& path, const ResourceLimits& limits) {
  PersistenceLoad load;
  if (path.empty()) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = "persistence path is empty";
    return load;
  }
  std::vector<std::uint8_t> image;
  std::string error;
  if (!read_image(path, limits, &image, &error)) {
    load.code = OutcomeCode::REJECT_INVALID;
    load.detail = error;
    return load;
  }
  return decode(image, limits);
}

PersistenceResult RouterStateStore::validate_file(const std::string& path,
                                                  const ResourceLimits& limits) {
  const PersistenceLoad loaded = load(path, limits);
  PersistenceResult result;
  result.code = loaded.code;
  result.detail = loaded.detail;
  return result;
}

}  // namespace model_router
