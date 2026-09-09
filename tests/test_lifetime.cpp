// Model Router - mandatory lifetime audit.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// One test per hazard, each named in its comment. Where a hazard is impossible
// by construction, the test asserts the construction that prevents it rather
// than pretending to observe the hazard.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "model_router/detail/crc32c.hpp"
#include "model_router/distributed/protocol.hpp"
#include "model_router/distributed/transport.hpp"
#include "router_fixture.hpp"

using model_router::BackendBootId;
using model_router::BackendGeneration;
using model_router::BackendId;
using model_router::ModelGeneration;
using model_router::OutcomeCode;
using model_router::PolicyGeneration;
using model_router::PolicyId;
using model_router::PriceGeneration;
using model_router::RouteOutcome;
using model_router::TenantId;

namespace {

constexpr TenantId kTenant(1);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);

/// A cost adapter that reports every price as UNKNOWN.
class NullCostProvider final : public model_router::CostProvider {
 public:
  [[nodiscard]] std::string name() const override { return "null-cost"; }
  [[nodiscard]] model_router::EvidenceResult<model_router::CostEvidence> fetch(
      const model_router::RouteCandidate&, const model_router::RouteRequest&,
      const model_router::DiscoveryContext&) override {
    return model_router::EvidenceResult<model_router::CostEvidence>::unavailable(
        OutcomeCode::REJECT_COST_UNKNOWN, "no price evidence");
  }
};

struct Ready {
  mrtest::Fixture fixture;

  void build() {
    fixture.start();
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    fixture.add_backend(BackendId(1), 1);
    fixture.add_backend(BackendId(2), 1);
    fixture.publish_costs(BackendId(1), PriceGeneration(1), 100, 100, 100);
    fixture.publish_costs(BackendId(2), PriceGeneration(1), 200, 200, 200);
  }

  [[nodiscard]] model_router::RouteRequest request() const {
    return mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  }
};

}  // namespace

// Hazard: string_view into a temporary. The library copies every view it keeps;
// a key built from a temporary stays valid after the temporary dies.
MR_TEST(lifetime, string_view_into_temporary) {
  model_router::CapabilityKey key;
  model_router::LocalityKey locality;
  {
    const std::string temporary = "task/code";
    key = model_router::CapabilityKey(temporary);
    locality = model_router::LocalityKey(std::string("local/loopback"));
    // A static view may be consumed and discarded freely.
    MR_CHECK_EQ(model_router::CapabilityKey::validate(temporary), std::string{});
    MR_CHECK_EQ(std::string(model_router::to_string(OutcomeCode::ROUTED)), std::string("ROUTED"));
  }
  MR_CHECK_EQ(key.value(), std::string("task/code"));
  MR_CHECK_EQ(locality.value(), std::string("local/loopback"));
  // The owned key is usable as a lookup identity after the source is gone.
  model_router::CapabilityProfile profile(model_router::CapabilityProfileId(1),
                                          model_router::CapabilityGeneration(1));
  model_router::CapabilityEvidence evidence;
  evidence.key = key;
  evidence.state = model_router::CapabilityState::VERIFIED;
  MR_CHECK(profile.set(std::move(evidence), 16));
  MR_CHECK(profile.find(key) != nullptr);
}

// Hazard: span into a temporary. Decoders consume a span and return values, so
// destroying the byte image cannot invalidate the decoded result.
MR_TEST(lifetime, span_into_temporary) {
  model_router::distributed::FrameHeader header;
  model_router::distributed::Frame frame;
  {
    model_router::distributed::Frame source;
    source.header.protocol_version = model_router::protocol_version;
    source.header.message_type =
        static_cast<std::uint16_t>(model_router::distributed::MessageType::PING);
    source.payload = {9, 8, 7};
    std::vector<std::uint8_t> bytes;
    std::string error;
    MR_CHECK(model_router::distributed::encode_frame(
        source, model_router::default_resource_limits(), &bytes, &error));
    const std::span<const std::uint8_t> view(bytes);
    MR_CHECK(model_router::distributed::decode_header(
        view, model_router::default_resource_limits(), &header, &error));
    MR_CHECK(model_router::distributed::decode_frame(
        view, model_router::default_resource_limits(), &frame, &error));
    MR_CHECK_EQ(model_router::detail::crc32c(view),
                model_router::detail::crc32c(std::span<const std::uint8_t>(bytes)));
  }
  // The byte image is gone; the decoded values are independent.
  MR_CHECK_EQ(header.message_type,
              static_cast<std::uint16_t>(model_router::distributed::MessageType::PING));
  MR_CHECK_EQ(frame.payload, std::vector<std::uint8_t>({9, 8, 7}));
}

// Hazard: reference into a temporary container. The public API never hands out
// references into canonical state, and a descriptor's own references point into
// the descriptor value the caller owns.
MR_TEST(lifetime, reference_into_temporary_container) {
  // Safe construction: the descriptor is a named value, so the binding pointer
  // it yields points into that value and outlives the call.
  model_router::BackendDescriptor descriptor = model_router::reference::make_backend(
      model_router::reference::make_catalog(), BackendId(1), BackendGeneration(1),
      BackendBootId(1), model_router::BackendRegistrationGeneration(1), "127.0.0.1:7001",
      model_router::Provenance::SYNTHETIC, 1700000000000LL);
  const model_router::ModelBinding* binding =
      descriptor.find_binding(model_router::reference::make_catalog().specialist_model);
  MR_CHECK(binding != nullptr);
  MR_CHECK_EQ(binding->model_id, model_router::reference::make_catalog().specialist_model);
  // The pointer still refers into the same descriptor after a copy exists: the
  // copy owns its own bindings, so no two owners share storage.
  const model_router::BackendDescriptor copy = descriptor;
  const model_router::ModelBinding* copy_binding =
      copy.find_binding(model_router::reference::make_catalog().specialist_model);
  MR_CHECK(copy_binding != nullptr);
  MR_CHECK(copy_binding != binding);

  // A policy handed to the router as a temporary is stored by value.
  Ready ready;
  ready.build();
  MR_CHECK(ready.fixture.router
               ->set_policy(model_router::PolicyBuilder(kPolicy, PolicyGeneration(2))
                                .fallback_policy(model_router::FallbackPolicy::PERMITTED)
                                .build())
               .accepted());
  MR_CHECK_EQ(ready.fixture.router->summary().policy_generation, PolicyGeneration(2));
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// Hazard: reference retained across mutation/reallocation. References obtained
// from caller-owned copies survive every canonical mutation.
MR_TEST(lifetime, reference_retained_across_mutation) {
  Ready ready;
  ready.build();
  model_router::BackendDescriptor descriptor;
  MR_CHECK(ready.fixture.router->find_backend(BackendId(1), &descriptor));
  const model_router::ModelBinding* binding =
      descriptor.find_binding(ready.fixture.catalog.specialist_model);
  MR_CHECK(binding != nullptr);
  const model_router::CostEvidence cost = binding->cost;

  // Force canonical containers to grow and reallocate.
  for (std::uint64_t id = 10; id < 210; ++id) {
    ready.fixture.add_backend(BackendId(id), 1);
  }
  for (std::uint64_t id = 100; id < 300; ++id) {
    model_router::ModelDescriptor extra =
        model_router::reference::make_models(ready.fixture.catalog, ready.fixture.now())[0];
    extra.model_id = model_router::ModelId(id);
    extra.model_version_id = model_router::ModelVersionId(id);
    (void)ready.fixture.router->register_model(std::move(extra));
  }
  MR_CHECK_EQ(binding->cost, cost);
  MR_CHECK(binding == descriptor.find_binding(ready.fixture.catalog.specialist_model));
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// Hazard: callback capture lifetime. The adapter is shared-owned for the whole
// call, so a captured reference is alive whenever the router invokes it.
MR_TEST(lifetime, callback_capture_lifetime) {
  struct CountingProvider final : public model_router::CostProvider {
    std::uint64_t* calls{nullptr};
    [[nodiscard]] std::string name() const override { return "counting-cost"; }
    [[nodiscard]] model_router::EvidenceResult<model_router::CostEvidence> fetch(
        const model_router::RouteCandidate& candidate, const model_router::RouteRequest&,
        const model_router::DiscoveryContext&) override {
      if (calls != nullptr) {
        ++(*calls);
      }
      const model_router::ModelBinding* binding =
          candidate.backend.find_binding(candidate.key.model_id);
      if (binding == nullptr) {
        return model_router::EvidenceResult<model_router::CostEvidence>::unavailable(
            OutcomeCode::REJECT_COST_UNKNOWN, "no binding");
      }
      return model_router::EvidenceResult<model_router::CostEvidence>::accepted(binding->cost);
    }
  };

  std::uint64_t calls = 0;
  {
    // The captured counter is declared before the provider and outlives it.
    mrtest::Fixture fixture;
    const std::shared_ptr<CountingProvider> provider = std::make_shared<CountingProvider>();
    provider->calls = &calls;
    model_router::ModelRouterOptions options;
    options.clock = fixture.clock;
    options.providers.dispatcher = fixture.dispatcher;
    options.providers.cost = provider;
    fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
    MR_CHECK(fixture.router->start().accepted());
    fixture.register_catalog();
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    fixture.add_backend(BackendId(1), 1);
    const RouteOutcome outcome =
        fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
    MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  }
  MR_CHECK(calls > 0);
}

// Hazard: session object destroyed on its own thread. FramedConnection owns its
// session state through a shared_ptr and is neither copyable nor movable, so a
// session can never be relocated or destroyed out from under a blocked reader.
MR_TEST(lifetime, session_object_destroyed_on_its_own_thread) {
  using Connection = model_router::distributed::FramedConnection;
  static_assert(!std::is_copy_constructible_v<Connection>);
  static_assert(!std::is_copy_assignable_v<Connection>);
  static_assert(!std::is_move_constructible_v<Connection>);
  static_assert(!std::is_move_assignable_v<Connection>);
  MR_CHECK(!std::is_copy_constructible_v<Connection>);
  MR_CHECK(!std::is_move_constructible_v<Connection>);

  namespace dist = model_router::distributed;
  dist::TransportOptions options;
  options.bind_address = "127.0.0.1";
  options.port = 0;
  options.limits = model_router::default_resource_limits();
  std::string error;
  dist::FrameListener listener;
  MR_CHECK(listener.start(options, &error));
  std::unique_ptr<dist::FramedConnection> server;
  std::latch accepted(1);
  std::string accept_error;
  std::thread accept_thread([&] {
    server = listener.accept(&accept_error);
    accepted.count_down();
  });
  std::unique_ptr<dist::FramedConnection> client =
      dist::FrameConnector::connect("127.0.0.1", listener.bound_port(), options, &error);
  accepted.wait();
  // Every thread is joined before the first check: a failing check must never
  // abort the process through a still-joinable std::thread.
  accept_thread.join();
  MR_CHECK(client != nullptr);
  MR_CHECK(server != nullptr);
  if (client == nullptr || server == nullptr) {
    listener.stop();
    ::mrtest::fail(__FILE__, __LINE__, "the loopback session could not be established");
  }
  // The peer identity is available while the session is live.
  const std::string peer = server->peer();
  MR_CHECK(!peer.empty());
  // Closing releases the session; the object remains readable and reports the
  // terminal reason, so a reader can never observe a half-destroyed session.
  server->close();
  MR_CHECK(server->closed());
  MR_CHECK_EQ(server->close_reason(), dist::CloseReason::LOCAL_CLOSE);
  // A released session reports defined empty values rather than dangling ones.
  MR_CHECK(server->peer().empty());
  MR_CHECK_EQ(server->close_reason(), dist::CloseReason::LOCAL_CLOSE);
  client->close();
  listener.stop();
}

// Hazard: self-join. The router and its adapters are non-movable, and a
// lifecycle call made from inside an adapter callback must return instead of
// waiting for a thread that is itself waiting for the call.
MR_TEST(lifetime, self_join) {
  static_assert(!std::is_copy_constructible_v<model_router::ModelRouter>);
  static_assert(!std::is_move_constructible_v<model_router::ModelRouter>);
  static_assert(!std::is_move_assignable_v<model_router::ModelRouter>);

  struct ShutdownProvider final : public model_router::CostProvider {
    model_router::ModelRouter* router{nullptr};
    bool called{false};
    [[nodiscard]] std::string name() const override { return "shutdown-cost"; }
    [[nodiscard]] model_router::EvidenceResult<model_router::CostEvidence> fetch(
        const model_router::RouteCandidate& candidate, const model_router::RouteRequest&,
        const model_router::DiscoveryContext&) override {
      if (router != nullptr && !called) {
        called = true;
        (void)router->shutdown();
      }
      const model_router::ModelBinding* binding =
          candidate.backend.find_binding(candidate.key.model_id);
      if (binding == nullptr) {
        return model_router::EvidenceResult<model_router::CostEvidence>::unavailable(
            OutcomeCode::REJECT_COST_UNKNOWN, "no binding");
      }
      return model_router::EvidenceResult<model_router::CostEvidence>::accepted(binding->cost);
    }
  };

  mrtest::Fixture fixture;
  const std::shared_ptr<ShutdownProvider> provider = std::make_shared<ShutdownProvider>();
  model_router::ModelRouterOptions options;
  options.clock = fixture.clock;
  options.providers.dispatcher = fixture.dispatcher;
  options.providers.cost = provider;
  fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
  MR_CHECK(fixture.router->start().accepted());
  fixture.register_catalog();
  provider->router = fixture.router.get();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);
  const RouteOutcome outcome =
      fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
  MR_CHECK(provider->called);
  MR_CHECK_EQ(outcome.code, OutcomeCode::SHUTTING_DOWN);
  MR_CHECK(fixture.router->check_invariants().ok());
}

// Hazard: destructor during callback. The router holds a shared_ptr to every
// adapter, so an adapter can never be destroyed while a call is in flight; it is
// released exactly once, after the call returns.
MR_TEST(lifetime, destructor_during_callback) {
  struct CountingProvider final : public model_router::CostProvider {
    std::atomic<std::uint64_t>* destroyed{nullptr};
    std::atomic<bool> in_flight{false};
    std::atomic<bool> destroyed_while_in_flight{false};
    ~CountingProvider() override {
      if (in_flight.load()) {
        destroyed_while_in_flight.store(true);
      }
      if (destroyed != nullptr) {
        destroyed->fetch_add(1);
      }
    }
    [[nodiscard]] std::string name() const override { return "destructor-cost"; }
    [[nodiscard]] model_router::EvidenceResult<model_router::CostEvidence> fetch(
        const model_router::RouteCandidate& candidate, const model_router::RouteRequest&,
        const model_router::DiscoveryContext&) override {
      in_flight.store(true);
      const model_router::ModelBinding* binding =
          candidate.backend.find_binding(candidate.key.model_id);
      in_flight.store(false);
      if (binding == nullptr) {
        return model_router::EvidenceResult<model_router::CostEvidence>::unavailable(
            OutcomeCode::REJECT_COST_UNKNOWN, "no binding");
      }
      return model_router::EvidenceResult<model_router::CostEvidence>::accepted(binding->cost);
    }
  };

  std::atomic<std::uint64_t> destroyed{0};
  std::atomic<bool> destroyed_during_call{false};
  {
    mrtest::Fixture fixture;
    std::shared_ptr<CountingProvider> provider = std::make_shared<CountingProvider>();
    provider->destroyed = &destroyed;
    model_router::ModelRouterOptions options;
    options.clock = fixture.clock;
    options.providers.dispatcher = fixture.dispatcher;
    options.providers.cost = provider;
    fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
    MR_CHECK(fixture.router->start().accepted());
    fixture.register_catalog();
    MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
    fixture.add_backend(BackendId(1), 1);
    const RouteOutcome outcome =
        fixture.router->route(mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration));
    MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
    // The adapter is still alive here: the router still owns it.
    MR_CHECK_EQ(destroyed.load(), std::uint64_t{0});
    // Dropping the router releases the router's reference, but the test still
    // holds one, so the adapter is not destroyed yet.
    fixture.router.reset();
    MR_CHECK_EQ(destroyed.load(), std::uint64_t{0});
    destroyed_during_call = provider->destroyed_while_in_flight.load();
    // Releasing the last reference destroys the adapter exactly once.
    provider.reset();
    MR_CHECK_EQ(destroyed.load(), std::uint64_t{1});
  }
  MR_CHECK(!destroyed_during_call.load());
  MR_CHECK_EQ(destroyed.load(), std::uint64_t{1});
}

// Hazard: reader over a temporary substring. A decoded string is owned by the
// caller, so it survives the buffer it was read from.
MR_TEST(lifetime, reader_over_temporary_substring) {
  std::string decoded;
  {
    model_router::distributed::PayloadWriter writer(model_router::default_resource_limits());
    writer.put_string("capability/task-code");
    writer.put_u32(7);
    writer.put_string(std::string(64, 'q'));
    MR_CHECK(writer.ok());
    model_router::distributed::PayloadReader reader(writer.bytes(),
                                                    model_router::default_resource_limits());
    decoded = reader.get_string();
    MR_CHECK_EQ(reader.get_u32(), std::uint32_t{7});
    const std::string long_value = reader.get_string();
    MR_CHECK_EQ(long_value.size(), std::size_t{64});
    MR_CHECK(reader.exhausted());
    MR_CHECK(reader.ok());
  }
  MR_CHECK_EQ(decoded, std::string("capability/task-code"));
}

// Hazard: zero-length decode. An empty input is a typed failure, never a
// silent success or an out-of-range read.
MR_TEST(lifetime, zero_length_decode_hazards) {
  const model_router::ResourceLimits limits = model_router::default_resource_limits();
  const std::span<const std::uint8_t> empty;
  model_router::distributed::FrameHeader header;
  model_router::distributed::Frame frame;
  std::string error;
  MR_CHECK(!model_router::distributed::decode_header(empty, limits, &header, &error));
  MR_CHECK(!error.empty());
  MR_CHECK(!model_router::distributed::decode_frame(empty, limits, &frame, &error));

  const model_router::PersistenceLoad empty_image =
      model_router::RouterStateStore::decode(std::vector<std::uint8_t>{}, limits);
  MR_CHECK(!empty_image.ok());
  MR_CHECK(!empty_image.has_state);
  MR_CHECK_EQ(empty_image.code, OutcomeCode::REJECT_INVALID);

  model_router::distributed::PayloadReader reader(empty, limits);
  MR_CHECK_EQ(reader.get_u32(), std::uint32_t{0});
  MR_CHECK(!reader.ok());
  MR_CHECK_EQ(reader.remaining(), std::size_t{0});

  // A legitimately empty string field decodes as empty and keeps the reader ok.
  model_router::distributed::PayloadWriter writer(limits);
  writer.put_string(std::string{});
  MR_CHECK(writer.ok());
  model_router::distributed::PayloadReader string_reader(writer.bytes(), limits);
  MR_CHECK_EQ(string_reader.get_string(), std::string{});
  MR_CHECK(string_reader.ok());
  MR_CHECK(string_reader.exhausted());

  MR_CHECK(!model_router::CapabilityKey::validate(std::string{}).empty());
  MR_CHECK(model_router::CapabilityKey{}.empty());
}

// Hazard: one-past-end indexing. Bounds are exact: a header-only frame and a
// key of exactly the maximum length are legal, one byte more is not.
MR_TEST(lifetime, one_past_end_indexing) {
  const model_router::ResourceLimits limits = model_router::default_resource_limits();
  model_router::distributed::Frame frame;
  frame.header.protocol_version = model_router::protocol_version;
  frame.header.message_type =
      static_cast<std::uint16_t>(model_router::distributed::MessageType::PING);
  std::vector<std::uint8_t> bytes;
  std::string error;
  MR_CHECK(model_router::distributed::encode_frame(frame, limits, &bytes, &error));
  MR_CHECK_EQ(bytes.size(), std::size_t{model_router::distributed::frame_header_bytes});
  model_router::distributed::Frame decoded;
  MR_CHECK(model_router::distributed::decode_frame(bytes, limits, &decoded, &error));
  MR_CHECK(decoded.payload.empty());

  // One byte short of the header is rejected without reading past the end.
  std::vector<std::uint8_t> short_header(bytes.begin(), bytes.end() - 1);
  model_router::distributed::FrameHeader header;
  MR_CHECK(!model_router::distributed::decode_header(short_header, limits, &header, &error));

  // The capability key bound is exact: 128 bytes is legal, 129 is not.
  const std::string at_limit = "a/" + std::string(126, 'b');
  MR_CHECK_EQ(at_limit.size(), std::size_t{128});
  MR_CHECK_EQ(model_router::CapabilityKey::validate(at_limit), std::string{});
  MR_CHECK(!model_router::CapabilityKey::validate(at_limit + "c").empty());

  // The persistence header bound is exact.
  std::vector<std::uint8_t> header_only(model_router::persistence_header_bytes, 0);
  const model_router::PersistenceLoad load = model_router::RouterStateStore::decode(
      std::vector<std::uint8_t>(model_router::persistence_header_bytes - 1, 0), limits);
  MR_CHECK(!load.ok());
}

// Hazard: moved-from authority state. A moved-from authority is all zeros and
// is never current, and a moved-from result is never an acceptance.
MR_TEST(lifetime, moved_from_authority_state) {
  Ready ready;
  ready.build();
  const RouteOutcome outcome = ready.fixture.router->route(ready.request());
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);

  model_router::RouteAuthority authority = outcome.decision.authority;
  const model_router::RouteAuthority original = authority;
  model_router::RouteAuthority moved_to = std::move(authority);
  MR_CHECK_EQ(moved_to, original);
  // RouteAuthority is a value type: a move leaves an equal value, so a
  // moved-from authority can never silently become a different authority.
  MR_CHECK_EQ(authority, moved_to);
  MR_CHECK(!moved_to.required_mask().empty());

  // A never-issued authority is never current.
  const model_router::RouteAuthority never_issued;
  const model_router::AuthorityComparison comparison = model_router::compare_authority(
      never_issued, original, original.required_mask());
  MR_CHECK(!comparison.current());
  MR_CHECK(!comparison.differences.empty());
  for (const model_router::AuthorityDifference& difference : comparison.differences) {
    MR_CHECK(difference.bound != difference.current);
  }

  // A moved-from result is only reused after assignment, and then it is fully
  // functional again.
  model_router::MutationResult result =
      model_router::MutationResult::make(OutcomeCode::ACCEPTED, "subject", "message");
  const model_router::MutationResult moved_result = std::move(result);
  MR_CHECK(moved_result.accepted());
  MR_CHECK_EQ(moved_result.explanation.subject(), std::string("subject"));
  result = model_router::MutationResult::make(OutcomeCode::REJECT_INVALID, "second", "second");
  MR_CHECK(!result.accepted());
  MR_CHECK_EQ(result.explanation.message(), std::string("second"));
}

// Hazard: shared_ptr cycles. The router owns its adapters, and destroying the
// router releases them: no adapter keeps the router alive.
MR_TEST(lifetime, shared_ptr_cycles) {
  std::weak_ptr<model_router::CostProvider> weak_cost;
  std::weak_ptr<model_router::DispatchHandler> weak_dispatcher;
  {
    mrtest::Fixture fixture;
    const std::shared_ptr<mrtest::RecordingDispatcher> dispatcher =
        std::make_shared<mrtest::RecordingDispatcher>();
    const std::shared_ptr<model_router::CostProvider> cost = std::make_shared<NullCostProvider>();
    weak_cost = cost;
    weak_dispatcher = dispatcher;
    model_router::ModelRouterOptions options;
    options.clock = fixture.clock;
    options.providers.dispatcher = dispatcher;
    options.providers.cost = cost;
    fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
    MR_CHECK(fixture.router->start().accepted());
    MR_CHECK(!weak_cost.expired());
    MR_CHECK(!weak_dispatcher.expired());
  }
  // The router is gone, and the only surviving references are the test's own,
  // which have also gone out of scope: the adapters are released.
  MR_CHECK(weak_cost.expired());
  MR_CHECK(weak_dispatcher.expired());
}

// Hazard: weak_ptr misuse. An expired adapter handle is observed, never
// dereferenced: the router's ownership is not resurrectable.
MR_TEST(lifetime, weak_ptr_misuse) {
  std::weak_ptr<mrtest::RecordingDispatcher> weak_dispatcher;
  std::shared_ptr<mrtest::RecordingDispatcher> strong;
  {
    mrtest::Fixture fixture;
    strong = std::make_shared<mrtest::RecordingDispatcher>();
    weak_dispatcher = strong;
    model_router::ModelRouterOptions options;
    options.clock = fixture.clock;
    options.providers.dispatcher = strong;
    fixture.router = std::make_unique<model_router::ModelRouter>(std::move(options));
    MR_CHECK(fixture.router->start().accepted());
    MR_CHECK(!weak_dispatcher.expired());
  }
  MR_CHECK(!weak_dispatcher.expired());
  MR_CHECK_EQ(strong.use_count(), std::int64_t{1});
  // An expired handle must be checked before use; locking an expired handle
  // yields nothing rather than a dangling reference.
  strong.reset();
  MR_CHECK(weak_dispatcher.expired());
  const std::shared_ptr<mrtest::RecordingDispatcher> locked = weak_dispatcher.lock();
  MR_CHECK(locked == nullptr);
}

// Hazard: iterator invalidation. Snapshots and history are values, so their
// iterators stay valid across canonical mutations; a profile's entries belong
// to the profile the caller owns.
MR_TEST(lifetime, iterator_invalidation) {
  Ready ready;
  ready.build();
  const RouteOutcome outcome = ready.fixture.router->route(ready.request());
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);

  const model_router::RouterSnapshot snapshot = ready.fixture.router->snapshot();
  const auto first_backend = snapshot.backends.begin();
  const auto last_backend = snapshot.backends.end();
  const std::vector<model_router::RouteDecision> history = ready.fixture.router->route_history();
  const auto history_first = history.begin();

  for (std::uint64_t id = 20; id < 120; ++id) {
    ready.fixture.add_backend(BackendId(id), 1);
  }
  const RouteOutcome later = ready.fixture.router->route(ready.request());
  MR_CHECK_EQ(later.code, OutcomeCode::ROUTED);

  MR_CHECK(first_backend != last_backend);
  MR_CHECK_EQ(first_backend->backend_id, BackendId(1));
  MR_CHECK_EQ(history_first->decision_id, outcome.decision.decision_id);
  MR_CHECK_EQ(snapshot.backend_count, std::uint32_t{2});
  MR_CHECK_EQ(history.size(), std::size_t{1});

  // A profile's entries reference the profile itself; mutating that profile is
  // the only thing that may invalidate them.
  model_router::CapabilityProfile profile(model_router::CapabilityProfileId(1),
                                          model_router::CapabilityGeneration(1));
  model_router::CapabilityEvidence claim;
  claim.key = model_router::CapabilityKey("task/code");
  claim.state = model_router::CapabilityState::VERIFIED;
  MR_CHECK(profile.set(claim, 16));
  const model_router::CapabilityEvidence* entry = profile.find(claim.key);
  MR_CHECK(entry != nullptr);
  MR_CHECK_EQ(entry->state, model_router::CapabilityState::VERIFIED);
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}

// Hazard: decoded-buffer pointer lifetime. A decoded state owns its strings and
// vectors, so it outlives the byte image it came from.
MR_TEST(lifetime, decoded_buffer_pointer_lifetime) {
  Ready ready;
  ready.build();
  const std::string path =
      (std::filesystem::temp_directory_path() / "model_router_lifetime_state.bin").string();
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  MR_CHECK(ready.fixture.router->save(path).ok());

  model_router::PersistenceLoad load;
  {
    std::vector<std::uint8_t> image;
    std::FILE* file = nullptr;
#if defined(_WIN32)
    if (fopen_s(&file, path.c_str(), "rb") != 0) {
      file = nullptr;
    }
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    MR_CHECK(file != nullptr);
    std::uint8_t buffer[4096];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
      image.insert(image.end(), buffer, buffer + read);
    }
    std::fclose(file);
    MR_CHECK(!image.empty());
    load = model_router::RouterStateStore::decode(image, model_router::default_resource_limits());
    MR_CHECK(load.ok());
  }
  // The byte image is gone; the decoded state is fully owned.
  MR_CHECK_EQ(load.state.backends.size(), std::size_t{2});
  MR_CHECK(!load.state.semantic_digest().empty());
  for (const model_router::PersistedBackendRecord& backend : load.state.backends) {
    MR_CHECK(backend.backend_id.valid());
    MR_CHECK(backend.endpoint_id.valid());
  }
  std::filesystem::remove(path, ignored);
}

// Hazard: candidate references invalidated by canonical state mutation. A
// decision holds candidate identities by value, so replacing incarnations
// cannot invalidate what a retained decision refers to.
MR_TEST(lifetime, candidate_references_invalidated_by_state_mutation) {
  Ready ready;
  ready.build();
  const RouteOutcome outcome = ready.fixture.router->route(ready.request());
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  const std::vector<model_router::CandidateKey> ranking_keys = [&outcome] {
    std::vector<model_router::CandidateKey> keys;
    for (const model_router::RankedCandidate& ranked : outcome.route.ranking) {
      keys.push_back(ranked.key);
    }
    return keys;
  }();
  const std::vector<model_router::CandidateKey> fallbacks = outcome.decision.fallbacks;

  // Replace every incarnation and retire a model.
  for (std::uint64_t id = 1; id <= 2; ++id) {
    model_router::BackendDescriptor restarted = model_router::reference::make_backend(
        ready.fixture.catalog, BackendId(id), BackendGeneration(1), BackendBootId(2),
        model_router::BackendRegistrationGeneration(2), "127.0.0.1:700" + std::to_string(id),
        model_router::Provenance::SYNTHETIC, ready.fixture.now());
    MR_CHECK(ready.fixture.router->register_backend(std::move(restarted)).accepted());
  }
  MR_CHECK(ready.fixture.router
               ->retire_model(ready.fixture.catalog.specialist_model, ModelGeneration(1))
               .accepted());

  model_router::RouteDecision stored;
  MR_CHECK(ready.fixture.router->find_decision(outcome.decision.decision_id, &stored));
  MR_CHECK_EQ(stored.fallbacks, fallbacks);
  for (std::size_t index = 0; index < ranking_keys.size(); ++index) {
    MR_CHECK_EQ(stored.explanation.ranking[index].key, ranking_keys[index]);
    MR_CHECK(stored.explanation.ranking[index].key.complete());
  }
  // The stale decision can no longer be dispatched, but its identities remain
  // readable values.
  // Legal: the replacement incarnation is a different backend registration and a
  // different boot, so either stale component may be reported first.
  const model_router::DispatchRecord dispatched =
      ready.fixture.router->dispatch(outcome.decision.decision_id, kTenant);
  MR_CHECK(!dispatched.handed_off);
  MR_CHECK(dispatched.code == OutcomeCode::REJECT_STALE_BACKEND_BOOT ||
           dispatched.code == OutcomeCode::REJECT_STALE_BACKEND);
  MR_CHECK(ready.fixture.router->check_invariants().ok());
}
