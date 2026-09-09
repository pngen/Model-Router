// Model Router - durable state persistence: round-trips, determinism, integrity,
// atomic replacement, and bounded sizes.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "model_router/detail/crc32c.hpp"
#include "router_fixture.hpp"

using model_router::BackendBootId;
using model_router::BackendGeneration;
using model_router::BackendId;
using model_router::BackendRegistrationGeneration;
using model_router::CoordinatorEpoch;
using model_router::OutcomeCode;
using model_router::PersistenceLoad;
using model_router::PersistenceResult;
using model_router::PolicyGeneration;
using model_router::PolicyId;
using model_router::PriceGeneration;
using model_router::Provenance;
using model_router::ResourceLimits;
using model_router::RouteOutcome;
using model_router::RouterPersistentState;
using model_router::RouterStateStore;
using model_router::TenantId;
using model_router::UnixMillis;

namespace {

constexpr TenantId kTenant(1);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);

// Header field offsets, exactly as persistence.hpp documents the layout.
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 8;
constexpr std::size_t kHeaderBytesOffset = 12;
constexpr std::size_t kFlagsOffset = 16;
constexpr std::size_t kRecordCountOffset = 20;
constexpr std::size_t kPayloadBytesOffset = 24;
constexpr std::size_t kPayloadCrcOffset = 32;
constexpr std::size_t kHeaderCrcOffset = 36;
constexpr std::size_t kDigestOffset = 40;
constexpr std::size_t kHeaderCrcCoverage = 36;

[[nodiscard]] std::string unique_name(const char* tag) {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t value = counter.fetch_add(1) + 1;
  return std::string("mr_persistence_") + tag + "_" + std::to_string(value) + ".state";
}

/// A unique path in the current working directory. The file, its .tmp sibling,
/// and any directory that was created at either name are removed on every exit
/// path, including a failing check.
class TempPath {
 public:
  explicit TempPath(std::string path) : path_(std::move(path)) {}
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  ~TempPath() { cleanup(); }

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string temp_sibling() const { return path_ + ".tmp"; }

  void cleanup() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    std::filesystem::remove_all(path_ + ".tmp", error);
  }

 private:
  std::string path_;
};

[[nodiscard]] bool exists(const std::string& path) {
  std::error_code error;
  return std::filesystem::exists(path, error);
}

void create_directory(const std::string& path) {
  std::error_code error;
  std::filesystem::create_directory(path, error);
  MR_CHECK(!error);
}

void remove_path(const std::string& path) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
  MR_CHECK(!error);
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::string& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  MR_CHECK(!error);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  std::ifstream stream(path, std::ios::binary);
  MR_CHECK(stream.good());
  if (!bytes.empty()) {
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    MR_CHECK(stream.gcount() == static_cast<std::streamsize>(bytes.size()));
  }
  return bytes;
}

void write_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  MR_CHECK(stream.good());
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  stream.flush();
  MR_CHECK(stream.good());
}

[[nodiscard]] std::uint32_t get_u32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(bytes[at + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(bytes[at + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return value;
}

void put_u32(std::vector<std::uint8_t>* bytes, std::size_t at, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    (*bytes)[at + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((value >> shift) & 0xFFu);
  }
}

/// Recomputes the header checksum exactly as the format specifies: CRC-32C over
/// every header byte before the checksum field itself.
void refresh_header_crc(std::vector<std::uint8_t>* bytes) {
  const std::uint32_t crc =
      model_router::detail::crc32c(std::span<const std::uint8_t>(bytes->data(), kHeaderCrcCoverage));
  put_u32(bytes, kHeaderCrcOffset, crc);
}

/// ModelDescriptor declares no operator==, so identity is compared field by field.
void check_same_model(const model_router::ModelDescriptor& lhs,
                      const model_router::ModelDescriptor& rhs) {
  MR_CHECK_EQ(lhs.model_id, rhs.model_id);
  MR_CHECK_EQ(lhs.model_generation, rhs.model_generation);
  MR_CHECK_EQ(lhs.model_version_id, rhs.model_version_id);
  MR_CHECK_EQ(lhs.artifact_generation, rhs.artifact_generation);
  MR_CHECK_EQ(lhs.family_id, rhs.family_id);
  MR_CHECK_EQ(lhs.context_limit_tokens, rhs.context_limit_tokens);
  MR_CHECK_EQ(lhs.max_output_tokens, rhs.max_output_tokens);
  MR_CHECK_EQ(lhs.input_modalities.bits(), rhs.input_modalities.bits());
  MR_CHECK_EQ(lhs.output_modalities.bits(), rhs.output_modalities.bits());
  MR_CHECK_EQ(lhs.quality_class, rhs.quality_class);
  MR_CHECK_EQ(lhs.lifecycle, rhs.lifecycle);
  MR_CHECK_EQ(lhs.capability_profile_id, rhs.capability_profile_id);
  MR_CHECK_EQ(lhs.capability_generation, rhs.capability_generation);
  MR_CHECK(lhs.capabilities.entries() == rhs.capabilities.entries());
  MR_CHECK_EQ(lhs.provenance, rhs.provenance);
  MR_CHECK_EQ(lhs.registered_at_unix_millis, rhs.registered_at_unix_millis);
  MR_CHECK_EQ(lhs.expires_at_unix_millis, rhs.expires_at_unix_millis);
  MR_CHECK_EQ(lhs.display_name, rhs.display_name);
}

/// BackendDescriptor declares no operator==, so the durable record is compared
/// field by field as well.
void check_same_backend_record(const model_router::PersistedBackendRecord& lhs,
                               const model_router::PersistedBackendRecord& rhs) {
  MR_CHECK_EQ(lhs.backend_id, rhs.backend_id);
  MR_CHECK_EQ(lhs.backend_generation, rhs.backend_generation);
  MR_CHECK_EQ(lhs.last_boot, rhs.last_boot);
  MR_CHECK_EQ(lhs.last_registration_generation, rhs.last_registration_generation);
  MR_CHECK_EQ(lhs.provider_id, rhs.provider_id);
  MR_CHECK_EQ(lhs.provider_generation, rhs.provider_generation);
  MR_CHECK_EQ(lhs.endpoint_id, rhs.endpoint_id);
  MR_CHECK_EQ(lhs.last_endpoint_generation, rhs.last_endpoint_generation);
  MR_CHECK_EQ(lhs.trust_profile_id, rhs.trust_profile_id);
  MR_CHECK_EQ(lhs.trust_generation, rhs.trust_generation);
  MR_CHECK_EQ(lhs.trust_domain, rhs.trust_domain);
  MR_CHECK_EQ(lhs.locality, rhs.locality);
  MR_CHECK_EQ(lhs.network_distance, rhs.network_distance);
  MR_CHECK_EQ(lhs.capability_profile_id, rhs.capability_profile_id);
  MR_CHECK_EQ(lhs.capability_generation, rhs.capability_generation);
  MR_CHECK(lhs.capabilities.entries() == rhs.capabilities.entries());
  MR_CHECK(lhs.model_bindings == rhs.model_bindings);
  MR_CHECK_EQ(lhs.offline_capable, rhs.offline_capable);
  MR_CHECK_EQ(lhs.provenance, rhs.provenance);
  MR_CHECK_EQ(lhs.registered_at_unix_millis, rhs.registered_at_unix_millis);
}

struct DurableContent {
  model_router::RouterId router_id{};
  model_router::RouterGeneration router_generation{};
  model_router::RouterEpoch router_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  model_router::RouterBootId router_boot{};
  model_router::RouteDecisionId decision_id{};
  model_router::RouteRequestId request_id{};
  model_router::CandidateKey winner{};
  model_router::PersistenceCounters counters{};
  UnixMillis saved_at{0};
  UnixMillis started_at{0};
};

/// Builds a router with non-trivial durable content: three models, two backend
/// registrations, one fenced incarnation with a real generation and fence time,
/// real price evidence, and one retained route.
DurableContent build_rich_content(mrtest::Fixture* fixture, bool with_route) {
  fixture->start();
  MR_CHECK(mrtest::install_open_policy(*fixture->router, kPolicy, kPolicyGeneration).accepted());
  fixture->add_backend(BackendId(1), 1);
  fixture->add_backend(BackendId(2), 1);
  // A newer incarnation of backend 2, so the previous boot can be fenced while
  // the backend itself stays registered.
  fixture->add_backend(BackendId(2), 2, BackendGeneration(1), 2);
  fixture->publish_costs(BackendId(1), PriceGeneration(1), 100, 100, 100);
  fixture->publish_costs(BackendId(2), PriceGeneration(1), 900, 900, 900);
  MR_CHECK(fixture->router
               ->fence_backend_boot(BackendId(2), BackendGeneration(1), BackendBootId(1),
                                    OutcomeCode::FENCED)
               .accepted());

  DurableContent content;
  content.router_id = fixture->router->router_id();
  content.router_generation = fixture->router->summary().router_generation;
  content.router_epoch = fixture->router->router_epoch();
  content.coordinator_epoch = fixture->router->coordinator_epoch();
  content.router_boot = fixture->router->router_boot();
  content.saved_at = fixture->now();
  content.started_at = fixture->router->summary().started_at_unix_millis;

  if (with_route) {
    model_router::RouteRequest request =
        mrtest::make_request(*fixture, kTenant, kPolicy, kPolicyGeneration);
    request.requirements.require_known_cost = true;
    const RouteOutcome outcome = fixture->router->route(request);
    MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
    MR_CHECK(outcome.has_decision);
    content.decision_id = outcome.decision.decision_id;
    content.request_id = outcome.decision.request_id;
    content.winner = outcome.decision.explanation.winner;
  }
  content.counters = fixture->router->summary().counters;
  return content;
}

/// Starts a router with an explicit, caller-supplied identity so two independent
/// routers can hold byte-identical canonical content.
void start_identified_router(mrtest::Fixture* fixture, model_router::RouterId router_id,
                             model_router::RouterGeneration generation) {
  model_router::ModelRouterOptions options;
  options.clock = fixture->clock;
  options.router_id = router_id;
  options.router_generation = generation;
  options.providers.dispatcher = fixture->dispatcher;
  fixture->router = std::make_unique<model_router::ModelRouter>(std::move(options));
  MR_CHECK(fixture->router->start().accepted());
  fixture->register_catalog();
  fixture->add_backend(BackendId(1), 1);
  fixture->add_backend(BackendId(2), 1);
  fixture->add_backend(BackendId(2), 2, BackendGeneration(1), 2);
  MR_CHECK(fixture->router
               ->fence_backend_boot(BackendId(2), BackendGeneration(1), BackendBootId(1),
                                    OutcomeCode::FENCED)
               .accepted());
}

void expect_rejected(const PersistenceLoad& load, const char* fragment) {
  MR_CHECK_EQ(load.code, OutcomeCode::REJECT_INVALID);
  MR_CHECK(!load.has_state);
  MR_CHECK(!load.ok());
  MR_CHECK(load.detail.find(fragment) != std::string::npos);
}

}  // namespace

MR_TEST(persistence, save_then_load_round_trips_every_durable_field) {
  mrtest::Fixture fixture;
  const DurableContent content = build_rich_content(&fixture, true);
  TempPath file(unique_name("round_trip"));
  const PersistenceResult saved = fixture.router->save(file.path());
  MR_CHECK_EQ(saved.code, OutcomeCode::ACCEPTED);
  MR_CHECK(!exists(file.temp_sibling()));

  const ResourceLimits limits;
  const PersistenceLoad loaded = RouterStateStore::load(file.path(), limits);
  MR_CHECK(loaded.ok());
  MR_CHECK_EQ(loaded.format_version, model_router::persistence_format_version);
  MR_CHECK_EQ(loaded.record_count, std::uint32_t{8});  // meta + 3 models + 2 backends + fence + route

  const RouterPersistentState& state = loaded.state;
  // Identity and epochs.
  MR_CHECK_EQ(state.router_id, content.router_id);
  MR_CHECK_EQ(state.router_generation, content.router_generation);
  MR_CHECK_EQ(state.last_router_epoch, content.router_epoch);
  MR_CHECK_EQ(state.last_coordinator_epoch, content.coordinator_epoch);
  MR_CHECK_EQ(state.last_boot, content.router_boot);
  MR_CHECK_EQ(state.saved_at_unix_millis, content.saved_at);
  MR_CHECK_EQ(state.last_started_at_unix_millis, content.started_at);

  // Models, byte-for-byte identical to the registered reference descriptors.
  const std::vector<model_router::ModelDescriptor> expected_models =
      model_router::reference::make_models(fixture.catalog, fixture.now());
  MR_CHECK_EQ(state.models.size(), expected_models.size());
  for (std::size_t index = 0; index < expected_models.size(); ++index) {
    check_same_model(state.models[index].model, expected_models[index]);
  }

  // Backend registration identity survives; dynamic evidence deliberately does not.
  MR_CHECK_EQ(state.backends.size(), std::size_t{2});
  const model_router::PersistedBackendRecord* first = nullptr;
  const model_router::PersistedBackendRecord* second = nullptr;
  for (const model_router::PersistedBackendRecord& record : state.backends) {
    if (record.backend_id == BackendId(1)) {
      first = &record;
    }
    if (record.backend_id == BackendId(2)) {
      second = &record;
    }
  }
  MR_CHECK(first != nullptr);
  MR_CHECK(second != nullptr);
  MR_CHECK_EQ(first->backend_generation, BackendGeneration(1));
  MR_CHECK_EQ(first->last_boot, BackendBootId(1));
  MR_CHECK_EQ(first->last_registration_generation, BackendRegistrationGeneration(1));
  MR_CHECK_EQ(first->provider_id, fixture.catalog.provider);
  MR_CHECK_EQ(first->provider_generation, fixture.catalog.provider_generation);
  MR_CHECK_EQ(first->endpoint_id, model_router::EndpointId(101));
  MR_CHECK_EQ(first->last_endpoint_generation, model_router::EndpointGeneration(1));
  MR_CHECK_EQ(first->trust_profile_id, model_router::TrustProfileId(1));
  MR_CHECK_EQ(first->trust_generation, model_router::TrustGeneration(1));
  MR_CHECK_EQ(first->trust_domain, model_router::TrustDomain::LOCAL);
  MR_CHECK_EQ(first->locality, fixture.catalog.locality);
  MR_CHECK_EQ(first->network_distance, std::uint32_t{1});
  MR_CHECK_EQ(first->capability_profile_id, model_router::CapabilityProfileId(102));
  MR_CHECK_EQ(first->offline_capable, true);
  MR_CHECK_EQ(first->provenance, Provenance::SYNTHETIC);
  MR_CHECK_EQ(first->registered_at_unix_millis, fixture.now());
  MR_CHECK_EQ(first->model_bindings.size(), std::size_t{3});
  for (const model_router::ModelBinding& binding : first->model_bindings) {
    MR_CHECK_EQ(binding.cost.price_generation, PriceGeneration(1));
    MR_CHECK_EQ(binding.cost.estimated_total_micros, std::int64_t{100});
    MR_CHECK(binding.cost.knownness == model_router::CostKnownness::QUOTED);
    MR_CHECK_EQ(binding.cost.unit, model_router::reference::reference_cost_unit());
  }
  // Backend 2 is the newer incarnation; its bindings carry their own prices.
  MR_CHECK_EQ(second->backend_generation, BackendGeneration(1));
  MR_CHECK_EQ(second->last_boot, BackendBootId(2));
  MR_CHECK_EQ(second->last_registration_generation, BackendRegistrationGeneration(2));
  for (const model_router::ModelBinding& binding : second->model_bindings) {
    MR_CHECK_EQ(binding.cost.estimated_total_micros, std::int64_t{900});
  }

  // The fenced incarnation keeps its real generation and fence time.
  MR_CHECK_EQ(state.fenced_boots.size(), std::size_t{1});
  MR_CHECK_EQ(state.fenced_boots[0].backend_id, BackendId(2));
  MR_CHECK_EQ(state.fenced_boots[0].backend_generation, BackendGeneration(1));
  MR_CHECK_EQ(state.fenced_boots[0].backend_boot, BackendBootId(1));
  MR_CHECK_EQ(state.fenced_boots[0].reason, OutcomeCode::FENCED);
  MR_CHECK_EQ(state.fenced_boots[0].fenced_at_unix_millis, fixture.now());
  MR_CHECK(state.fenced_boots[0].fenced_at_unix_millis != 0);

  // The retained route keeps its identity, winner, and bound authority.
  MR_CHECK_EQ(state.routes.size(), std::size_t{1});
  const model_router::PersistedRouteRecord& route = state.routes[0];
  MR_CHECK_EQ(route.decision_id, content.decision_id);
  MR_CHECK_EQ(route.request_id, content.request_id);
  MR_CHECK_EQ(route.winner, content.winner);
  MR_CHECK_EQ(route.status, model_router::RouteStatus::CURRENT);
  MR_CHECK_EQ(route.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(route.authority.router_id, content.router_id);
  MR_CHECK_EQ(route.authority.router_epoch, content.router_epoch);
  MR_CHECK_EQ(route.authority.coordinator_epoch, content.coordinator_epoch);
  MR_CHECK_EQ(route.authority.request_id, content.request_id);
  MR_CHECK_EQ(route.authority.model_id, content.winner.model_id);
  MR_CHECK_EQ(route.authority.model_generation, content.winner.model_generation);
  MR_CHECK_EQ(route.authority.backend_id, content.winner.backend_id);
  MR_CHECK_EQ(route.authority.backend_boot, content.winner.backend_boot);
  MR_CHECK_EQ(route.authority.tenant, kTenant);
  MR_CHECK_EQ(route.policy_generation, kPolicyGeneration);
  MR_CHECK_EQ(route.price_generation, PriceGeneration(1));
  MR_CHECK(!route.semantic_digest.empty());
  MR_CHECK_EQ(route.created_at_unix_millis, fixture.now());

  // Counters survive.
  MR_CHECK_EQ(state.counters.route_requests, content.counters.route_requests);
  MR_CHECK_EQ(state.counters.route_decisions, content.counters.route_decisions);
  MR_CHECK_EQ(state.counters.dispatches, content.counters.dispatches);
  MR_CHECK_EQ(state.counters.route_rejections, content.counters.route_rejections);
  MR_CHECK_EQ(state.counters.reroutes, content.counters.reroutes);
  MR_CHECK_EQ(state.counters.fallbacks_taken, content.counters.fallbacks_taken);
  MR_CHECK_EQ(state.counters.route_requests, std::uint64_t{1});
  MR_CHECK(state.counters.highest_identity_issued != 0);

  // A file that decodes also validates, and the decoded state re-encodes to the
  // exact bytes on disk.
  const PersistenceResult validated = RouterStateStore::validate_file(file.path(), limits);
  MR_CHECK_EQ(validated.code, OutcomeCode::ACCEPTED);
  std::vector<std::uint8_t> image;
  std::uint32_t records = 0;
  MR_CHECK_EQ(RouterStateStore::encode(state, limits, &image, &records).code, OutcomeCode::ACCEPTED);
  MR_CHECK_EQ(records, loaded.record_count);
  MR_CHECK(image == read_bytes(file.path()));
}

MR_TEST(persistence, encoded_image_is_deterministic_for_identical_content) {
  // The same router saved twice produces byte-identical images.
  mrtest::Fixture fixture;
  const DurableContent content = build_rich_content(&fixture, true);
  TempPath first(unique_name("deterministic_a"));
  TempPath second(unique_name("deterministic_b"));
  MR_CHECK_EQ(fixture.router->save(first.path()).code, OutcomeCode::ACCEPTED);
  MR_CHECK_EQ(fixture.router->save(second.path()).code, OutcomeCode::ACCEPTED);
  const std::vector<std::uint8_t> first_bytes = read_bytes(first.path());
  MR_CHECK(first_bytes == read_bytes(second.path()));
  MR_CHECK(!first_bytes.empty());

  // Canonicalization makes the image independent of insertion order: reversing
  // every list yields the same semantic digest and the same bytes.
  const ResourceLimits limits;
  const PersistenceLoad loaded = RouterStateStore::load(first.path(), limits);
  MR_CHECK(loaded.ok());
  RouterPersistentState shuffled = loaded.state;
  std::reverse(shuffled.models.begin(), shuffled.models.end());
  std::reverse(shuffled.backends.begin(), shuffled.backends.end());
  std::reverse(shuffled.fenced_boots.begin(), shuffled.fenced_boots.end());
  std::reverse(shuffled.routes.begin(), shuffled.routes.end());
  shuffled.canonicalize();
  MR_CHECK_EQ(shuffled.semantic_digest(), loaded.state.semantic_digest());
  std::vector<std::uint8_t> shuffled_image;
  std::uint32_t shuffled_records = 0;
  MR_CHECK_EQ(RouterStateStore::encode(shuffled, limits, &shuffled_image, &shuffled_records).code,
              OutcomeCode::ACCEPTED);
  MR_CHECK(shuffled_image == first_bytes);
  MR_CHECK_EQ(shuffled_records, loaded.record_count);

  // Two independent routers holding the same canonical content produce the same
  // semantic digest and the same image.
  mrtest::Fixture left;
  mrtest::Fixture right;
  start_identified_router(&left, model_router::RouterId(4242), model_router::RouterGeneration(7));
  start_identified_router(&right, model_router::RouterId(4242), model_router::RouterGeneration(7));
  TempPath left_file(unique_name("identical_left"));
  TempPath right_file(unique_name("identical_right"));
  MR_CHECK_EQ(left.router->save(left_file.path()).code, OutcomeCode::ACCEPTED);
  MR_CHECK_EQ(right.router->save(right_file.path()).code, OutcomeCode::ACCEPTED);
  const PersistenceLoad left_state = RouterStateStore::load(left_file.path(), limits);
  const PersistenceLoad right_state = RouterStateStore::load(right_file.path(), limits);
  MR_CHECK(left_state.ok());
  MR_CHECK(right_state.ok());
  MR_CHECK_EQ(left_state.state.router_id, right_state.state.router_id);
  MR_CHECK_EQ(left_state.state.semantic_digest(), right_state.state.semantic_digest());
  MR_CHECK(read_bytes(left_file.path()) == read_bytes(right_file.path()));
  (void)content;
}

MR_TEST(persistence, corrupted_header_bytes_are_rejected) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, true);
  TempPath file(unique_name("corrupt_header"));
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  const std::vector<std::uint8_t> pristine = read_bytes(file.path());
  const ResourceLimits limits;

  // Magic.
  {
    std::vector<std::uint8_t> bytes = pristine;
    bytes[kMagicOffset] ^= 0x01u;
    write_bytes(file.path(), bytes);
    expect_rejected(RouterStateStore::load(file.path(), limits), "magic");
  }
  // Header size.
  {
    std::vector<std::uint8_t> bytes = pristine;
    bytes[kHeaderBytesOffset] ^= 0x01u;
    write_bytes(file.path(), bytes);
    expect_rejected(RouterStateStore::load(file.path(), limits), "header size");
  }
  // Unknown flags.
  {
    std::vector<std::uint8_t> bytes = pristine;
    bytes[kFlagsOffset] = 0x02u;
    write_bytes(file.path(), bytes);
    expect_rejected(RouterStateStore::load(file.path(), limits), "unknown flags");
  }
  // A header byte covered by the header checksum.
  {
    std::vector<std::uint8_t> bytes = pristine;
    bytes[kRecordCountOffset] ^= 0x01u;
    write_bytes(file.path(), bytes);
    expect_rejected(RouterStateStore::load(file.path(), limits), "header checksum mismatch");
  }
}

MR_TEST(persistence, corrupted_payload_byte_is_rejected) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, true);
  TempPath file(unique_name("corrupt_payload"));
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  std::vector<std::uint8_t> bytes = read_bytes(file.path());
  MR_CHECK(bytes.size() > model_router::persistence_header_bytes);
  bytes[model_router::persistence_header_bytes + 5] ^= 0xFFu;
  write_bytes(file.path(), bytes);
  expect_rejected(RouterStateStore::load(file.path(), model_router::ResourceLimits{}),
                  "payload checksum mismatch");
}

MR_TEST(persistence, corrupted_payload_crc_is_rejected_with_a_valid_header_crc) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, true);
  TempPath file(unique_name("corrupt_payload_crc"));
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  std::vector<std::uint8_t> bytes = read_bytes(file.path());
  const std::uint32_t stored = get_u32(bytes, kPayloadCrcOffset);
  put_u32(&bytes, kPayloadCrcOffset, stored ^ 0x00000001u);
  // The payload checksum field is inside the header checksum's coverage, so the
  // header checksum must be refreshed to reach the payload check.
  refresh_header_crc(&bytes);
  write_bytes(file.path(), bytes);
  expect_rejected(RouterStateStore::load(file.path(), model_router::ResourceLimits{}),
                  "payload checksum mismatch");
}

MR_TEST(persistence, corrupted_header_crc_is_rejected) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, true);
  TempPath file(unique_name("corrupt_header_crc"));
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  std::vector<std::uint8_t> bytes = read_bytes(file.path());
  bytes[kHeaderCrcOffset] ^= 0x01u;
  write_bytes(file.path(), bytes);
  expect_rejected(RouterStateStore::load(file.path(), model_router::ResourceLimits{}),
                  "header checksum mismatch");
}

MR_TEST(persistence, corrupted_semantic_digest_is_rejected) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, true);
  TempPath file(unique_name("corrupt_digest"));
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  std::vector<std::uint8_t> bytes = read_bytes(file.path());
  bytes[kDigestOffset] ^= 0x01u;
  write_bytes(file.path(), bytes);
  // The digest lies outside the header checksum's coverage, so this reaches the
  // semantic comparison of the decoded state.
  expect_rejected(RouterStateStore::load(file.path(), model_router::ResourceLimits{}),
                  "semantic digest does not match");
}

MR_TEST(persistence, truncation_is_rejected_at_every_boundary) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, true);
  TempPath file(unique_name("truncated"));
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  const std::vector<std::uint8_t> bytes = read_bytes(file.path());
  const std::size_t header = model_router::persistence_header_bytes;
  MR_CHECK(bytes.size() > header + 1);
  const ResourceLimits limits;
  const std::size_t sizes[] = {0, 1, header - 1, header, header + 1, bytes.size() - 1};
  for (const std::size_t size : sizes) {
    std::vector<std::uint8_t> truncated(bytes.begin(),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(size));
    const PersistenceLoad load = RouterStateStore::decode(truncated, limits);
    MR_CHECK_EQ(load.code, OutcomeCode::REJECT_INVALID);
    MR_CHECK(!load.has_state);
    if (size < header) {
      MR_CHECK(load.detail.find("shorter than its header") != std::string::npos);
    } else {
      MR_CHECK(load.detail.find("truncated") != std::string::npos);
    }
  }
  // The complete image still decodes, proving the truncations above are the
  // only difference.
  MR_CHECK(RouterStateStore::decode(bytes, limits).ok());
}

MR_TEST(persistence, trailing_garbage_is_rejected) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, true);
  TempPath file(unique_name("trailing"));
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  const std::vector<std::uint8_t> bytes = read_bytes(file.path());
  const ResourceLimits limits;
  for (const std::size_t extra : {std::size_t{1}, std::size_t{4096}}) {
    std::vector<std::uint8_t> padded = bytes;
    padded.insert(padded.end(), extra, 0xA5u);
    const PersistenceLoad load = RouterStateStore::decode(padded, limits);
    MR_CHECK_EQ(load.code, OutcomeCode::REJECT_INVALID);
    MR_CHECK(!load.has_state);
    MR_CHECK(load.detail.find("trailing garbage") != std::string::npos);
  }
}

MR_TEST(persistence, unsupported_format_version_is_rejected_before_parsing) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, true);
  TempPath file(unique_name("version"));
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  const std::vector<std::uint8_t> bytes = read_bytes(file.path());
  const ResourceLimits limits;
  for (const std::uint32_t version : {std::uint32_t{2}, std::uint32_t{0xFFFFFFFFu}}) {
    std::vector<std::uint8_t> patched = bytes;
    put_u32(&patched, kVersionOffset, version);
    const PersistenceLoad load = RouterStateStore::decode(patched, limits);
    MR_CHECK_EQ(load.code, OutcomeCode::REJECT_INVALID);
    MR_CHECK(!load.has_state);
    MR_CHECK_EQ(load.format_version, version);
    MR_CHECK(load.detail.find("unsupported persistence format version") != std::string::npos);
    MR_CHECK(load.detail.find(std::to_string(version)) != std::string::npos);
  }
}

MR_TEST(persistence, record_count_mismatch_is_rejected_with_a_valid_header_crc) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, true);
  TempPath file(unique_name("record_count"));
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  const std::vector<std::uint8_t> bytes = read_bytes(file.path());
  const std::uint32_t records = get_u32(bytes, kRecordCountOffset);
  MR_CHECK_EQ(records, std::uint32_t{8});
  const ResourceLimits limits;
  for (const std::uint32_t patched : {records + 1u, records - 1u}) {
    std::vector<std::uint8_t> image = bytes;
    put_u32(&image, kRecordCountOffset, patched);
    refresh_header_crc(&image);
    // The header checksum is valid, so only the count check can reject.
    const PersistenceLoad load = RouterStateStore::decode(image, limits);
    MR_CHECK_EQ(load.code, OutcomeCode::REJECT_INVALID);
    MR_CHECK(!load.has_state);
    MR_CHECK(load.detail.find("record count does not match") != std::string::npos);
  }
}

MR_TEST(persistence, saving_replaces_the_previous_file_atomically) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, false);
  TempPath file(unique_name("atomic"));
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  const std::vector<std::uint8_t> before = read_bytes(file.path());
  const ResourceLimits limits;
  const PersistenceLoad first = RouterStateStore::load(file.path(), limits);
  MR_CHECK(first.ok());
  MR_CHECK_EQ(first.state.backends.size(), std::size_t{2});

  // Mutating the router and saving again fully replaces the file.
  fixture.add_backend(BackendId(3), 1);
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  const std::vector<std::uint8_t> after = read_bytes(file.path());
  MR_CHECK(before != after);
  MR_CHECK(!exists(file.temp_sibling()));
  const PersistenceLoad second = RouterStateStore::load(file.path(), limits);
  MR_CHECK(second.ok());
  MR_CHECK_EQ(second.state.backends.size(), std::size_t{3});
  MR_CHECK(second.state.backends.size() > first.state.backends.size());

  // A save that cannot replace the target leaves the previous file intact.
  create_directory(file.temp_sibling());
  const PersistenceResult failed = fixture.router->save(file.path());
  MR_CHECK(!failed.ok());
  MR_CHECK_EQ(failed.code, OutcomeCode::INTERNAL_ERROR);
  MR_CHECK(failed.detail.find("temporary") != std::string::npos);
  MR_CHECK(read_bytes(file.path()) == after);
  MR_CHECK(RouterStateStore::load(file.path(), limits).ok());
  remove_path(file.temp_sibling());
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  MR_CHECK(RouterStateStore::load(file.path(), limits).ok());

  // A path that is a directory cannot be replaced either, and no .tmp is left.
  TempPath directory(unique_name("directory_target"));
  create_directory(directory.path());
  const PersistenceResult to_directory = fixture.router->save(directory.path());
  MR_CHECK(!to_directory.ok());
  MR_CHECK_EQ(to_directory.code, OutcomeCode::INTERNAL_ERROR);
  MR_CHECK(!exists(directory.temp_sibling()));
  MR_CHECK(exists(directory.path()));
}

MR_TEST(persistence, bounded_record_and_payload_sizes_are_enforced) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, true);
  TempPath file(unique_name("bounded"));
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  const ResourceLimits limits;
  const PersistenceLoad loaded = RouterStateStore::load(file.path(), limits);
  MR_CHECK(loaded.ok());

  // 3 models + 2 backends + 1 fenced boot + 1 route == 7 records; the encoded
  // image adds the meta record, so a limit of 7 must reject it.
  ResourceLimits record_limits = limits;
  record_limits.max_persistence_records = 7;
  std::vector<std::uint8_t> image;
  std::uint32_t records = 0;
  const PersistenceResult too_many =
      RouterStateStore::encode(loaded.state, record_limits, &image, &records);
  MR_CHECK_EQ(too_many.code, OutcomeCode::REJECT_LIMIT);
  MR_CHECK(too_many.detail.find("record count") != std::string::npos);

  // The same state encodes under the unmodified limit.
  MR_CHECK_EQ(RouterStateStore::encode(loaded.state, limits, &image, &records).code,
              OutcomeCode::ACCEPTED);
  MR_CHECK_EQ(records, std::uint32_t{8});

  // A payload larger than the configured payload bound is rejected.
  ResourceLimits payload_limits = limits;
  payload_limits.max_payload_bytes = get_u32(image, kPayloadBytesOffset) - 1u;
  const PersistenceLoad too_large = RouterStateStore::decode(image, payload_limits);
  MR_CHECK_EQ(too_large.code, OutcomeCode::REJECT_LIMIT);
  MR_CHECK(!too_large.has_state);
  MR_CHECK(too_large.detail.find("payload length exceeds the configured limit") != std::string::npos);

  // An image larger than the configured byte bound is rejected as well.
  ResourceLimits byte_limits = limits;
  byte_limits.max_persistence_bytes = image.size() - 1;
  const PersistenceLoad too_big = RouterStateStore::decode(image, byte_limits);
  MR_CHECK_EQ(too_big.code, OutcomeCode::REJECT_LIMIT);
  MR_CHECK(too_big.detail.find("byte limit") != std::string::npos);
}

MR_TEST(persistence, store_encode_decode_round_trips_in_memory) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, true);
  TempPath file(unique_name("memory"));
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  const ResourceLimits limits;
  const PersistenceLoad on_disk = RouterStateStore::load(file.path(), limits);
  MR_CHECK(on_disk.ok());

  std::vector<std::uint8_t> image;
  std::uint32_t records = 0;
  MR_CHECK_EQ(RouterStateStore::encode(on_disk.state, limits, &image, &records).code,
              OutcomeCode::ACCEPTED);
  MR_CHECK_EQ(records, on_disk.record_count);
  const PersistenceLoad in_memory = RouterStateStore::decode(image, limits);
  MR_CHECK(in_memory.ok());
  MR_CHECK_EQ(in_memory.record_count, records);
  MR_CHECK_EQ(in_memory.format_version, model_router::persistence_format_version);
  MR_CHECK_EQ(in_memory.state.router_id, on_disk.state.router_id);
  MR_CHECK_EQ(in_memory.state.router_generation, on_disk.state.router_generation);
  MR_CHECK_EQ(in_memory.state.last_router_epoch, on_disk.state.last_router_epoch);
  MR_CHECK_EQ(in_memory.state.last_coordinator_epoch, on_disk.state.last_coordinator_epoch);
  MR_CHECK_EQ(in_memory.state.last_boot, on_disk.state.last_boot);
  MR_CHECK_EQ(in_memory.state.saved_at_unix_millis, on_disk.state.saved_at_unix_millis);
  MR_CHECK_EQ(in_memory.state.last_started_at_unix_millis,
              on_disk.state.last_started_at_unix_millis);
  MR_CHECK_EQ(in_memory.state.models.size(), on_disk.state.models.size());
  for (std::size_t index = 0; index < on_disk.state.models.size(); ++index) {
    check_same_model(in_memory.state.models[index].model, on_disk.state.models[index].model);
  }
  MR_CHECK_EQ(in_memory.state.backends.size(), on_disk.state.backends.size());
  for (std::size_t index = 0; index < on_disk.state.backends.size(); ++index) {
    check_same_backend_record(in_memory.state.backends[index], on_disk.state.backends[index]);
  }
  MR_CHECK(in_memory.state.fenced_boots == on_disk.state.fenced_boots);
  MR_CHECK(in_memory.state.routes == on_disk.state.routes);
  MR_CHECK(in_memory.state.counters == on_disk.state.counters);
  MR_CHECK_EQ(in_memory.state.semantic_digest(), on_disk.state.semantic_digest());

  // validate_file agrees with load, for a good file and for a corrupted one.
  MR_CHECK_EQ(RouterStateStore::validate_file(file.path(), limits).code, on_disk.code);
  std::vector<std::uint8_t> corrupt = image;
  corrupt[kDigestOffset] ^= 0xFFu;
  write_bytes(file.path(), corrupt);
  const PersistenceLoad bad_load = RouterStateStore::load(file.path(), limits);
  const PersistenceResult bad_validate = RouterStateStore::validate_file(file.path(), limits);
  MR_CHECK(!bad_load.ok());
  MR_CHECK_EQ(bad_validate.code, bad_load.code);
  MR_CHECK_EQ(bad_validate.detail, bad_load.detail);
  MR_CHECK(!bad_validate.ok());
}

MR_TEST(persistence, a_path_containing_spaces_works) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, true);
  TempPath file("mr persistence spaced " + std::to_string(fixture.now()) + " state.bin");
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  MR_CHECK(exists(file.path()));
  MR_CHECK(!exists(file.temp_sibling()));
  const ResourceLimits limits;
  const PersistenceLoad loaded = RouterStateStore::load(file.path(), limits);
  MR_CHECK(loaded.ok());
  MR_CHECK_EQ(loaded.state.router_id, fixture.router->router_id());
  MR_CHECK_EQ(RouterStateStore::validate_file(file.path(), limits).code, OutcomeCode::ACCEPTED);
  MR_CHECK(loaded.state.routes.size() == std::size_t{1});
}

MR_TEST(persistence, missing_and_empty_paths_are_rejected) {
  mrtest::Fixture fixture;
  build_rich_content(&fixture, true);
  const ResourceLimits limits;
  TempPath absent(unique_name("absent"));
  const PersistenceLoad missing = RouterStateStore::load(absent.path(), limits);
  MR_CHECK_EQ(missing.code, OutcomeCode::REJECT_INVALID);
  MR_CHECK(!missing.has_state);
  MR_CHECK(missing.detail.find("cannot open the state file") != std::string::npos);

  TempPath file(unique_name("empty_path"));
  MR_CHECK_EQ(fixture.router->save(file.path()).code, OutcomeCode::ACCEPTED);
  const PersistenceLoad loaded = RouterStateStore::load(file.path(), limits);
  MR_CHECK(loaded.ok());
  const PersistenceResult empty_save = RouterStateStore::save(loaded.state, "", limits);
  MR_CHECK_EQ(empty_save.code, OutcomeCode::REJECT_INVALID);
  MR_CHECK(empty_save.detail.find("path is empty") != std::string::npos);
  const PersistenceLoad empty_load = RouterStateStore::load("", limits);
  MR_CHECK_EQ(empty_load.code, OutcomeCode::REJECT_INVALID);
  MR_CHECK(empty_load.detail.find("path is empty") != std::string::npos);
  const PersistenceResult null_output = RouterStateStore::encode(loaded.state, limits, nullptr, nullptr);
  MR_CHECK_EQ(null_output.code, OutcomeCode::REJECT_INVALID);
  MR_CHECK(null_output.detail.find("null") != std::string::npos);
}
