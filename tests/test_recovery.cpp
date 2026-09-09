// Model Router - conservative recovery from durable state.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "router_fixture.hpp"

using model_router::BackendBootId;
using model_router::BackendGeneration;
using model_router::BackendId;
using model_router::BackendRegistrationGeneration;
using model_router::CoordinatorEpoch;
using model_router::MutationResult;
using model_router::OutcomeCode;
using model_router::PersistenceLoad;
using model_router::PersistenceResult;
using model_router::PolicyGeneration;
using model_router::PolicyId;
using model_router::PriceGeneration;
using model_router::Provenance;
using model_router::ResourceLimits;
using model_router::RouteOutcome;
using model_router::RouterStateStore;
using model_router::TenantId;
using model_router::UnixMillis;

namespace {

constexpr TenantId kTenant(1);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);

[[nodiscard]] std::string unique_name(const char* tag) {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t value = counter.fetch_add(1) + 1;
  return std::string("mr_recovery_") + tag + "_" + std::to_string(value) + ".state";
}

/// Removes the state file and its .tmp sibling on every exit path.
class TempPath {
 public:
  explicit TempPath(std::string path) : path_(std::move(path)) {}
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  ~TempPath() { cleanup(); }

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  void cleanup() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    std::filesystem::remove_all(path_ + ".tmp", error);
  }

 private:
  std::string path_;
};

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::string& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  MR_CHECK(!error);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  std::ifstream stream(path, std::ios::binary);
  MR_CHECK(stream.good());
  if (!bytes.empty()) {
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
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

/// Everything a recovery test must observe about the state that was saved.
struct Prepared {
  model_router::RouterId router_id{};
  model_router::RouterGeneration router_generation{};
  model_router::RouterEpoch router_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  model_router::RouterBootId router_boot{};
  model_router::RouteDecisionId decision_id{};
  model_router::RouteRequestId request_id{};
  model_router::CandidateKey winner{};
  model_router::RouteAuthority authority{};
  model_router::PersistenceCounters counters{};
  UnixMillis fenced_at{0};
  UnixMillis saved_at{0};
  std::uint32_t model_count{0};
  std::uint32_t backend_count{0};
};

/// Builds non-trivial durable state (three models, two backends, a fenced
/// incarnation with a real generation and fence time, real prices, one retained
/// route), saves it, and destroys the router that produced it.
Prepared save_then_destroy(mrtest::Fixture* fixture, const std::string& path) {
  fixture->start();
  MR_CHECK(mrtest::install_open_policy(*fixture->router, kPolicy, kPolicyGeneration).accepted());
  fixture->add_backend(BackendId(1), 1);
  fixture->add_backend(BackendId(2), 1);
  fixture->add_backend(BackendId(2), 2, BackendGeneration(1), 2);
  fixture->publish_costs(BackendId(1), PriceGeneration(1), 100, 100, 100);
  fixture->publish_costs(BackendId(2), PriceGeneration(1), 900, 900, 900);

  // Time moves before the fence, so the recorded fence time can only be the
  // real observation and never the registration timestamp.
  fixture->clock->advance(5000);
  const UnixMillis fence_time = fixture->now();
  MR_CHECK(fixture->router
               ->fence_backend_boot(BackendId(2), BackendGeneration(1), BackendBootId(1),
                                    OutcomeCode::FENCED)
               .accepted());

  model_router::RouteRequest request =
      mrtest::make_request(*fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.require_known_cost = true;
  const RouteOutcome outcome = fixture->router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK(outcome.has_decision);
  MR_CHECK_EQ(outcome.decision.status, model_router::RouteStatus::CURRENT);

  Prepared prepared;
  prepared.router_id = fixture->router->router_id();
  prepared.router_generation = fixture->router->summary().router_generation;
  prepared.router_epoch = fixture->router->router_epoch();
  prepared.coordinator_epoch = fixture->router->coordinator_epoch();
  prepared.router_boot = fixture->router->router_boot();
  prepared.decision_id = outcome.decision.decision_id;
  prepared.request_id = outcome.decision.request_id;
  prepared.winner = outcome.decision.explanation.winner;
  prepared.authority = outcome.decision.authority;
  prepared.counters = fixture->router->summary().counters;
  prepared.model_count = fixture->router->summary().model_count;
  prepared.backend_count = fixture->router->summary().backend_count;
  prepared.fenced_at = fence_time;
  prepared.saved_at = fixture->now();
  MR_CHECK_EQ(fixture->router->save(path).code, OutcomeCode::ACCEPTED);

  // The producing process is gone: only the file remains.
  fixture->router.reset();
  return prepared;
}

/// Starts a fresh process-equivalent router that loads durable state at start.
void start_recovered(mrtest::Fixture* fixture, const std::string& path) {
  model_router::ModelRouterOptions options;
  options.clock = fixture->clock;
  options.persistence_path = path;
  options.load_on_start = true;
  options.providers.dispatcher = fixture->dispatcher;
  fixture->router = std::make_unique<model_router::ModelRouter>(std::move(options));
  const MutationResult started = fixture->router->start();
  MR_CHECK(started.accepted());
}

/// A fresh fixture whose clock is later than the saved state, as a restarted
/// process would be.
[[nodiscard]] std::unique_ptr<mrtest::Fixture> recovered_fixture(const std::string& path) {
  auto fixture = std::make_unique<mrtest::Fixture>();
  fixture->clock = std::make_shared<model_router::LogicalClock>(1700000100000LL);
  start_recovered(fixture.get(), path);
  return fixture;
}

}  // namespace

MR_TEST(recovery, identity_is_restored_and_epochs_are_advanced) {
  TempPath file(unique_name("identity"));
  mrtest::Fixture author;
  const Prepared prepared = save_then_destroy(&author, file.path());

  const std::unique_ptr<mrtest::Fixture> fixture = recovered_fixture(file.path());
  const model_router::RouterSummary summary = fixture->router->summary();
  MR_CHECK(summary.running);
  MR_CHECK(summary.recovered);
  MR_CHECK_EQ(summary.router_id, prepared.router_id);
  MR_CHECK_EQ(summary.router_generation, prepared.router_generation);
  // Authority epochs strictly advance, so nothing bound to the previous process
  // can still be current.
  MR_CHECK(summary.router_epoch > prepared.router_epoch);
  MR_CHECK(summary.coordinator_epoch > prepared.coordinator_epoch);
  MR_CHECK(summary.router_boot > prepared.router_boot);
  MR_CHECK(summary.router_epoch.value() >= prepared.router_epoch.value() + 1);
  MR_CHECK(summary.coordinator_epoch.value() >= prepared.coordinator_epoch.value() + 1);
  MR_CHECK(summary.router_boot.value() >= prepared.router_boot.value() + 1);

  // Catalog and registration identity survive.
  MR_CHECK_EQ(summary.model_count, prepared.model_count);
  MR_CHECK_EQ(summary.model_count, std::uint32_t{3});
  MR_CHECK_EQ(summary.backend_count, prepared.backend_count);
  MR_CHECK_EQ(summary.backend_count, std::uint32_t{2});
  MR_CHECK_EQ(summary.route_decision_count, std::uint32_t{1});
  MR_CHECK_EQ(summary.fenced_boot_count, std::uint32_t{1});
  MR_CHECK_EQ(summary.counters.route_requests, prepared.counters.route_requests);
  MR_CHECK_EQ(summary.counters.route_decisions, prepared.counters.route_decisions);

  model_router::ModelDescriptor model;
  MR_CHECK(fixture->router->find_model(fixture->catalog.specialist_model, &model));
  MR_CHECK_EQ(model.model_generation, fixture->catalog.specialist_generation);
  MR_CHECK(model.capabilities.size() > std::size_t{0});

  model_router::BackendDescriptor backend;
  MR_CHECK(fixture->router->find_backend(BackendId(1), &backend));
  MR_CHECK_EQ(backend.backend_boot, BackendBootId(1));
  MR_CHECK_EQ(backend.model_bindings.size(), std::size_t{3});
  MR_CHECK(fixture->router->find_backend(BackendId(2), &backend));
  MR_CHECK_EQ(backend.backend_boot, BackendBootId(2));

  // The fenced incarnation keeps its real generation and fence time.
  const std::vector<model_router::FencedBootRecord> fenced = fixture->router->fenced_boots();
  MR_CHECK_EQ(fenced.size(), std::size_t{1});
  MR_CHECK_EQ(fenced[0].backend_id, BackendId(2));
  MR_CHECK_EQ(fenced[0].backend_generation, BackendGeneration(1));
  MR_CHECK_EQ(fenced[0].backend_boot, BackendBootId(1));
  MR_CHECK_EQ(fenced[0].reason, OutcomeCode::FENCED);
  MR_CHECK_EQ(fenced[0].fenced_at_unix_millis, prepared.fenced_at);
  MR_CHECK(fenced[0].fenced_at_unix_millis != 0);

  // The retained route is recovered with its identity intact.
  model_router::RouteDecision decision;
  MR_CHECK(fixture->router->find_decision(prepared.decision_id, &decision));
  MR_CHECK_EQ(decision.decision_id, prepared.decision_id);
  MR_CHECK_EQ(decision.request_id, prepared.request_id);
  MR_CHECK_EQ(decision.explanation.winner, prepared.winner);
  MR_CHECK_EQ(decision.authority, prepared.authority);
  MR_CHECK(fixture->router->check_invariants().ok());
}

MR_TEST(recovery, historical_route_is_inspectable_but_never_dispatchable) {
  TempPath file(unique_name("historical"));
  mrtest::Fixture author;
  const Prepared prepared = save_then_destroy(&author, file.path());
  const std::unique_ptr<mrtest::Fixture> fixture = recovered_fixture(file.path());

  model_router::RouteDecision decision;
  MR_CHECK(fixture->router->find_decision(prepared.decision_id, &decision));
  MR_CHECK_EQ(decision.status, model_router::RouteStatus::STALE);
  MR_CHECK_EQ(decision.explanation.currentness, model_router::Currentness::RECONSTRUCTED);
  MR_CHECK(!decision.dispatchable());
  MR_CHECK(decision.explanation.revalidation_required);
  MR_CHECK_EQ(decision.explanation.winner, prepared.winner);
  MR_CHECK_EQ(decision.code, OutcomeCode::ROUTED);

  // Inspection still works and names the decision.
  const std::string explanation = fixture->router->explain_route(prepared.decision_id);
  MR_CHECK(!explanation.empty());
  MR_CHECK(explanation.find(std::to_string(prepared.decision_id.value())) != std::string::npos);
  const std::vector<model_router::RouteDecision> history = fixture->router->route_history();
  MR_CHECK_EQ(history.size(), std::size_t{1});
  MR_CHECK_EQ(history[0].decision_id, prepared.decision_id);

  // No loaded decision is CURRENT.
  for (const model_router::RouteDecision& retained : history) {
    MR_CHECK(retained.status != model_router::RouteStatus::CURRENT);
    MR_CHECK(retained.explanation.currentness != model_router::Currentness::CURRENT);
  }
  for (const model_router::RouteSummary& retained : fixture->router->snapshot().routes) {
    MR_CHECK(retained.status != model_router::RouteStatus::CURRENT);
    MR_CHECK_EQ(retained.currentness, model_router::Currentness::RECONSTRUCTED);
  }

  // Dispatch is refused and the handler is never called.
  const model_router::DispatchRecord dispatched =
      fixture->router->dispatch(prepared.decision_id, kTenant);
  MR_CHECK(!dispatched.handed_off);
  MR_CHECK_EQ(dispatched.code, OutcomeCode::REJECT_STALE_ROUTER_EPOCH);
  MR_CHECK_EQ(fixture->dispatcher->calls, std::uint64_t{0});
}

MR_TEST(recovery, recovered_backends_have_no_current_evidence) {
  TempPath file(unique_name("evidence"));
  mrtest::Fixture author;
  save_then_destroy(&author, file.path());
  const std::unique_ptr<mrtest::Fixture> fixture = recovered_fixture(file.path());

  const model_router::RouterSnapshot snapshot = fixture->router->snapshot();
  MR_CHECK_EQ(snapshot.currentness, model_router::Currentness::RECONSTRUCTED);
  MR_CHECK_EQ(snapshot.backends.size(), std::size_t{2});
  for (const model_router::BackendSummary& backend : snapshot.backends) {
    MR_CHECK_EQ(backend.health, model_router::HealthState::UNKNOWN);
    MR_CHECK_EQ(backend.availability, model_router::AvailabilityState::UNKNOWN);
    MR_CHECK_EQ(backend.readiness, model_router::ReadinessState::UNKNOWN);
    MR_CHECK_EQ(backend.residency, model_router::ResidencyState::UNKNOWN);
    MR_CHECK_EQ(backend.capacity, model_router::CapacityState::UNKNOWN);
    MR_CHECK(!backend.evidence_current);
    MR_CHECK(!backend.fenced);
    MR_CHECK_EQ(backend.bound_model_count, std::uint32_t{3});
  }
  // The descriptor itself carries no resurrected generation either.
  model_router::BackendDescriptor backend;
  MR_CHECK(fixture->router->find_backend(BackendId(1), &backend));
  MR_CHECK_EQ(backend.health.state, model_router::HealthState::UNKNOWN);
  MR_CHECK(!backend.health.generation.valid());
  MR_CHECK_EQ(backend.availability.state, model_router::AvailabilityState::UNKNOWN);
  MR_CHECK(!backend.availability.generation.valid());
  MR_CHECK_EQ(backend.readiness.state, model_router::ReadinessState::UNKNOWN);
  MR_CHECK(!backend.readiness.generation.valid());
  MR_CHECK(!backend.latency.has_observation());
}

MR_TEST(recovery, authority_bound_to_the_old_router_epoch_is_rejected) {
  TempPath file(unique_name("old_epoch"));
  mrtest::Fixture author;
  const Prepared prepared = save_then_destroy(&author, file.path());
  const std::unique_ptr<mrtest::Fixture> fixture = recovered_fixture(file.path());

  model_router::RouteDecision decision;
  MR_CHECK(fixture->router->find_decision(prepared.decision_id, &decision));
  // The decision is bound to the epoch the previous process published.
  MR_CHECK_EQ(decision.authority.router_epoch, prepared.router_epoch);
  MR_CHECK_EQ(decision.authority.coordinator_epoch, prepared.coordinator_epoch);
  MR_CHECK(decision.authority.router_epoch != fixture->router->router_epoch());
  MR_CHECK(decision.authority.coordinator_epoch != fixture->router->coordinator_epoch());

  // Revalidating it reports the documented stale code.
  const MutationResult revalidated = fixture->router->revalidate(prepared.decision_id, kTenant);
  MR_CHECK_EQ(revalidated.code, OutcomeCode::REJECT_STALE_ROUTER_EPOCH);
  MR_CHECK(revalidated.rejected());
  MR_CHECK(revalidated.needs_revalidation());

  // The comparison itself names the epoch as the first difference.
  model_router::RouteAuthority current = decision.authority;
  current.router_epoch = fixture->router->router_epoch();
  current.coordinator_epoch = fixture->router->coordinator_epoch();
  const model_router::AuthorityComparison stale =
      model_router::compare_authority(decision.authority, current,
                                      decision.authority.required_mask());
  MR_CHECK(!stale.current());
  MR_CHECK_EQ(stale.code, OutcomeCode::REJECT_STALE_ROUTER_EPOCH);
  MR_CHECK_EQ(stale.differences.front().component, model_router::AuthorityComponent::ROUTER_EPOCH);
  MR_CHECK_EQ(stale.differences.front().bound, prepared.router_epoch.value());
  MR_CHECK_EQ(stale.differences.front().current, fixture->router->router_epoch().value());

  // A matching comparison is accepted, proving the difference above is the epoch.
  const model_router::AuthorityComparison fresh =
      model_router::compare_authority(current, current, current.required_mask());
  MR_CHECK(fresh.current());
  MR_CHECK_EQ(fresh.code, OutcomeCode::ACCEPTED);
}

MR_TEST(recovery, routing_needs_fresh_evidence_after_recovery) {
  TempPath file(unique_name("fresh_evidence"));
  mrtest::Fixture author;
  save_then_destroy(&author, file.path());
  const std::unique_ptr<mrtest::Fixture> fixture = recovered_fixture(file.path());

  // Policy is evidence, not durable state: it must be republished too.
  MR_CHECK(mrtest::install_open_policy(*fixture->router, kPolicy, kPolicyGeneration).accepted());
  model_router::RouteRequest request =
      mrtest::make_request(*fixture, kTenant, kPolicy, kPolicyGeneration);
  const RouteOutcome without_evidence = fixture->router->route(request);
  MR_CHECK_EQ(without_evidence.code, OutcomeCode::NO_ELIGIBLE_CANDIDATE);
  MR_CHECK(!without_evidence.has_decision);
  MR_CHECK(!without_evidence.route.rejections.empty());
  for (const model_router::RouteRejection& rejection : without_evidence.route.rejections) {
    MR_CHECK_EQ(rejection.code, OutcomeCode::REJECT_UNKNOWN_EVIDENCE);
  }

  // Fresh boot-bound evidence makes the same routing work again.
  fixture->publish_current(BackendId(1), BackendBootId(1));
  fixture->publish_current(BackendId(2), BackendBootId(2));
  request.request_id = model_router::allocate_id<model_router::RouteRequestTag>();
  const RouteOutcome routed = fixture->router->route(request);
  MR_CHECK_EQ(routed.code, OutcomeCode::ROUTED);
  MR_CHECK(routed.has_decision);
  MR_CHECK_EQ(routed.decision.status, model_router::RouteStatus::CURRENT);
  MR_CHECK_EQ(routed.decision.authority.router_epoch, fixture->router->router_epoch());
  const model_router::DispatchRecord dispatched =
      fixture->router->dispatch(routed.decision.decision_id, kTenant);
  MR_CHECK_EQ(dispatched.code, OutcomeCode::DISPATCHED);
  MR_CHECK(dispatched.handed_off);
  MR_CHECK_EQ(fixture->dispatcher->calls, std::uint64_t{1});
  MR_CHECK(fixture->router->check_invariants().ok());
}

MR_TEST(recovery, fenced_incarnations_stay_fenced_across_a_restart) {
  TempPath file(unique_name("fenced"));
  mrtest::Fixture author;
  save_then_destroy(&author, file.path());
  const std::unique_ptr<mrtest::Fixture> fixture = recovered_fixture(file.path());

  model_router::BackendDescriptor old_boot = model_router::reference::make_backend(
      fixture->catalog, BackendId(2), BackendGeneration(1), BackendBootId(1),
      BackendRegistrationGeneration(3), "127.0.0.1:7002", Provenance::SYNTHETIC, fixture->now());
  const MutationResult rejected = fixture->router->register_backend(std::move(old_boot));
  MR_CHECK_EQ(rejected.code, OutcomeCode::REJECT_STALE_BACKEND_BOOT);
  MR_CHECK(rejected.rejected());
  MR_CHECK(rejected.explanation.message().find("fenced") != std::string::npos);

  // Capability publication from the fenced incarnation is refused as well.
  model_router::BackendCapabilityPublication publication;
  publication.backend_id = BackendId(2);
  publication.backend_generation = BackendGeneration(1);
  publication.backend_boot = BackendBootId(1);
  publication.profile_id = model_router::CapabilityProfileId(202);
  publication.generation = model_router::CapabilityGeneration(1);
  MR_CHECK_EQ(fixture->router->publish_capabilities(std::move(publication)).code,
              OutcomeCode::REJECT_STALE_BACKEND_BOOT);

  // The current incarnation of the same backend is unaffected by the fence.
  model_router::BackendDescriptor current_boot = model_router::reference::make_backend(
      fixture->catalog, BackendId(2), BackendGeneration(1), BackendBootId(2),
      BackendRegistrationGeneration(3), "127.0.0.1:7002", Provenance::SYNTHETIC, fixture->now());
  MR_CHECK_EQ(fixture->router->register_backend(std::move(current_boot)).code,
              OutcomeCode::ACCEPTED);
  model_router::BackendDescriptor stored;
  MR_CHECK(fixture->router->find_backend(BackendId(2), &stored));
  MR_CHECK_EQ(stored.backend_boot, BackendBootId(2));
}

MR_TEST(recovery, corrupt_state_leaves_the_previous_state_unchanged) {
  TempPath good(unique_name("corrupt_good"));
  TempPath bad(unique_name("corrupt_bad"));
  mrtest::Fixture author;
  const Prepared prepared = save_then_destroy(&author, good.path());

  std::vector<std::uint8_t> bytes = read_bytes(good.path());
  MR_CHECK(bytes.size() > 72);
  bytes[40] ^= 0xFFu;  // semantic digest byte
  write_bytes(bad.path(), bytes);

  const ResourceLimits limits;
  const PersistenceLoad store_load = RouterStateStore::load(bad.path(), limits);
  MR_CHECK_EQ(store_load.code, OutcomeCode::REJECT_INVALID);
  MR_CHECK(!store_load.has_state);
  MR_CHECK(store_load.detail.find("semantic digest") != std::string::npos);

  // A router that already holds recovered state keeps it byte-for-byte.
  mrtest::Fixture holder;
  holder.clock = std::make_shared<model_router::LogicalClock>(1700000200000LL);
  model_router::ModelRouterOptions options;
  options.clock = holder.clock;
  options.providers.dispatcher = holder.dispatcher;
  holder.router = std::make_unique<model_router::ModelRouter>(std::move(options));
  const PersistenceResult first = holder.router->load(good.path());
  MR_CHECK_EQ(first.code, OutcomeCode::ACCEPTED);
  const std::string before_snapshot = holder.router->snapshot().to_text();
  const std::string before_summary = holder.router->summary().to_text();
  MR_CHECK_EQ(holder.router->summary().route_decision_count, std::uint32_t{1});

  const PersistenceResult second = holder.router->load(bad.path());
  MR_CHECK_EQ(second.code, OutcomeCode::REJECT_INVALID);
  MR_CHECK(second.detail.find("semantic digest") != std::string::npos);
  MR_CHECK_EQ(holder.router->snapshot().to_text(), before_snapshot);
  MR_CHECK_EQ(holder.router->summary().to_text(), before_summary);
  MR_CHECK_EQ(holder.router->router_id(), prepared.router_id);
  MR_CHECK(holder.router->check_invariants().ok());
  model_router::RouteDecision decision;
  MR_CHECK(holder.router->find_decision(prepared.decision_id, &decision));

  // A running recovered router is equally unaffected by a corrupt load.
  const std::unique_ptr<mrtest::Fixture> running = recovered_fixture(good.path());
  const std::string running_before = running->router->snapshot().to_text();
  const PersistenceResult refused = running->router->load(bad.path());
  MR_CHECK_EQ(refused.code, OutcomeCode::REJECT_INVALID);
  MR_CHECK_EQ(running->router->snapshot().to_text(), running_before);
  MR_CHECK(running->router->running());

  // The good file is untouched and still loads.
  MR_CHECK_EQ(RouterStateStore::load(good.path(), limits).code, OutcomeCode::ACCEPTED);
}
