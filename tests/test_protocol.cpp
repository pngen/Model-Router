// Model Router - reference framed wire protocol: codecs, framing, and bounded
// payload readers and writers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "model_router/detail/crc32c.hpp"
#include "model_router/distributed/protocol.hpp"
#include "router_fixture.hpp"

using model_router::BackendBootId;
using model_router::BackendGeneration;
using model_router::BackendId;
using model_router::BackendRegistrationGeneration;
using model_router::CapabilityGeneration;
using model_router::CapabilityProfileId;
using model_router::CapabilityState;
using model_router::FailureClass;
using model_router::OutcomeCode;
using model_router::PolicyGeneration;
using model_router::PolicyId;
using model_router::PriceGeneration;
using model_router::Provenance;
using model_router::ResourceLimits;
using model_router::RouteOutcome;
using model_router::TenantId;
using model_router::UnixMillis;
using model_router::distributed::AdminOp;
using model_router::distributed::AdminRequest;
using model_router::distributed::AdminResponse;
using model_router::distributed::BackendRegisterAckMessage;
using model_router::distributed::BackendRegisterMessage;
using model_router::distributed::CapabilityPublishMessage;
using model_router::distributed::DispatchMessage;
using model_router::distributed::DispatchResultMessage;
using model_router::distributed::ErrorMessage;
using model_router::distributed::Frame;
using model_router::distributed::FrameHeader;
using model_router::distributed::HelloAckMessage;
using model_router::distributed::HelloMessage;
using model_router::distributed::MessageType;
using model_router::distributed::PayloadReader;
using model_router::distributed::PayloadWriter;
using model_router::distributed::RouteRequestMessage;
using model_router::distributed::RouteResponseMessage;

namespace {

constexpr TenantId kTenant(1);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);
constexpr std::uint32_t kProtocolVersion = model_router::protocol_version;

// Frame header field offsets and the header checksum coverage.
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kTypeOffset = 6;
constexpr std::size_t kPayloadBytesOffset = 12;
constexpr std::size_t kHeaderCrcOffset = 16;
constexpr std::size_t kPayloadCrcOffset = 20;
constexpr std::size_t kHeaderCrcCoverage = 16;

[[nodiscard]] std::uint16_t get_u16(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return static_cast<std::uint16_t>(bytes[at] | (static_cast<std::uint16_t>(bytes[at + 1]) << 8));
}

[[nodiscard]] std::uint32_t get_u32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(bytes[at + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return value;
}

void put_u16(std::vector<std::uint8_t>* bytes, std::size_t at, std::uint16_t value) {
  (*bytes)[at] = static_cast<std::uint8_t>(value & 0xFFu);
  (*bytes)[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void put_u32(std::vector<std::uint8_t>* bytes, std::size_t at, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    (*bytes)[at + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((value >> shift) & 0xFFu);
  }
}

/// Recomputes the frame header checksum over the covered prefix, so a test can
/// reach checks that run after the header checksum.
void refresh_header_crc(std::vector<std::uint8_t>* bytes) {
  put_u32(bytes, kHeaderCrcOffset,
          model_router::detail::crc32c(
              std::span<const std::uint8_t>(bytes->data(), kHeaderCrcCoverage)));
}

[[nodiscard]] model_router::CapabilityRequirement require_capability(std::string key) {
  model_router::CapabilityRequirement requirement;
  requirement.key = model_router::CapabilityKey(std::move(key));
  requirement.minimum_state = CapabilityState::DECLARED;
  return requirement;
}

[[nodiscard]] Frame make_frame(MessageType type, std::vector<std::uint8_t> payload) {
  Frame frame;
  frame.header.magic = model_router::distributed::frame_magic;
  frame.header.protocol_version = kProtocolVersion;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.payload = std::move(payload);
  return frame;
}

void check_header_fields(const FrameHeader& header, std::uint16_t type, std::uint32_t flags,
                         std::uint32_t payload_bytes) {
  MR_CHECK_EQ(header.magic, model_router::distributed::frame_magic);
  MR_CHECK_EQ(header.protocol_version, static_cast<std::uint16_t>(kProtocolVersion));
  MR_CHECK_EQ(header.message_type, type);
  MR_CHECK_EQ(header.flags, flags);
  MR_CHECK_EQ(header.payload_bytes, payload_bytes);
}

/// Encodes p message, decodes it again, and requires exact equality.
template <class Message, class Encode, class Decode>
void check_round_trip(const Message& message, Encode encode, Decode decode) {
  const ResourceLimits limits;
  std::vector<std::uint8_t> bytes;
  std::string error;
  MR_CHECK(encode(message, limits, &bytes, &error));
  MR_CHECK(error.empty());
  MR_CHECK(!bytes.empty());
  Message decoded{};
  MR_CHECK(decode(bytes, limits, &decoded, &error));
  MR_CHECK(error.empty());
  MR_CHECK(decoded == message);
  // A null output pointer is an explicit failure, never a crash.
  MR_CHECK(!encode(message, limits, nullptr, &error));
  MR_CHECK(!error.empty());
  MR_CHECK(!decode(bytes, limits, nullptr, &error));
  MR_CHECK(!error.empty());
}

/// RouteDecision declares no operator==, so a decoded decision is compared
/// field by field against the expected value.
void check_same_decision(const model_router::RouteDecision& lhs,
                         const model_router::RouteDecision& rhs) {
  MR_CHECK_EQ(lhs.decision_id, rhs.decision_id);
  MR_CHECK_EQ(lhs.decision_generation, rhs.decision_generation);
  MR_CHECK_EQ(lhs.request_id, rhs.request_id);
  MR_CHECK_EQ(lhs.request_generation, rhs.request_generation);
  MR_CHECK_EQ(lhs.status, rhs.status);
  MR_CHECK_EQ(lhs.code, rhs.code);
  MR_CHECK(lhs.authority == rhs.authority);
  MR_CHECK(lhs.fallbacks == rhs.fallbacks);
  MR_CHECK_EQ(lhs.dispatch_generation, rhs.dispatch_generation);
  MR_CHECK_EQ(lhs.dispatch_id, rhs.dispatch_id);
  MR_CHECK(lhs.reservation == rhs.reservation);
  MR_CHECK_EQ(lhs.created_at_unix_millis, rhs.created_at_unix_millis);
  MR_CHECK_EQ(lhs.superseded_at_unix_millis, rhs.superseded_at_unix_millis);
  MR_CHECK_EQ(lhs.explanation.request_id, rhs.explanation.request_id);
  MR_CHECK_EQ(lhs.explanation.request_generation, rhs.explanation.request_generation);
  MR_CHECK(lhs.explanation.winner == rhs.explanation.winner);
  MR_CHECK_EQ(lhs.explanation.eligible_candidate_count, rhs.explanation.eligible_candidate_count);
  MR_CHECK_EQ(lhs.explanation.rejected_candidate_count, rhs.explanation.rejected_candidate_count);
  MR_CHECK(lhs.explanation.rejections == rhs.explanation.rejections);
  MR_CHECK(lhs.explanation.ranking == rhs.explanation.ranking);
  MR_CHECK_EQ(lhs.explanation.tie_break_reason, rhs.explanation.tie_break_reason);
  MR_CHECK(lhs.explanation.authority == rhs.explanation.authority);
  MR_CHECK(lhs.explanation.cost == rhs.explanation.cost);
  MR_CHECK(lhs.explanation.slo == rhs.explanation.slo);
  MR_CHECK(lhs.explanation.budget == rhs.explanation.budget);
  MR_CHECK(lhs.explanation.policy == rhs.explanation.policy);
  MR_CHECK_EQ(lhs.explanation.policy_result, rhs.explanation.policy_result);
  MR_CHECK(lhs.explanation.fallback_order == rhs.explanation.fallback_order);
  MR_CHECK_EQ(lhs.explanation.currentness, rhs.explanation.currentness);
  MR_CHECK_EQ(lhs.explanation.revalidation_required, rhs.explanation.revalidation_required);
  MR_CHECK_EQ(lhs.explanation.requirement_digest, rhs.explanation.requirement_digest);
  MR_CHECK_EQ(lhs.explanation.semantic_digest, rhs.explanation.semantic_digest);
}

/// Round-trip helper for messages whose defaulted operator== is deleted because
/// a member type declares none. The comparator states the encoded contract.
template <class Message, class Encode, class Decode, class Compare>
void check_encoded_round_trip(const Message& message, Encode encode, Decode decode,
                              Compare compare) {
  const ResourceLimits limits;
  std::vector<std::uint8_t> bytes;
  std::string error;
  MR_CHECK(encode(message, limits, &bytes, &error));
  MR_CHECK(error.empty());
  MR_CHECK(!bytes.empty());
  Message decoded{};
  MR_CHECK(decode(bytes, limits, &decoded, &error));
  MR_CHECK(error.empty());
  compare(decoded, message);
  MR_CHECK(!encode(message, limits, nullptr, &error));
  MR_CHECK(!error.empty());
  MR_CHECK(!decode(bytes, limits, nullptr, &error));
  MR_CHECK(!error.empty());
}

/// Every BackendDescriptor field the wire format carries.
void check_same_backend_descriptor(const model_router::BackendDescriptor& lhs,
                                   const model_router::BackendDescriptor& rhs) {
  MR_CHECK_EQ(lhs.backend_id, rhs.backend_id);
  MR_CHECK_EQ(lhs.backend_generation, rhs.backend_generation);
  MR_CHECK_EQ(lhs.backend_boot, rhs.backend_boot);
  MR_CHECK_EQ(lhs.backend_registration_generation, rhs.backend_registration_generation);
  MR_CHECK_EQ(lhs.provider_id, rhs.provider_id);
  MR_CHECK_EQ(lhs.provider_generation, rhs.provider_generation);
  MR_CHECK_EQ(lhs.endpoint.endpoint_id, rhs.endpoint.endpoint_id);
  MR_CHECK_EQ(lhs.endpoint.endpoint_generation, rhs.endpoint.endpoint_generation);
  MR_CHECK_EQ(lhs.endpoint.backend_id, rhs.endpoint.backend_id);
  MR_CHECK_EQ(lhs.endpoint.backend_boot, rhs.endpoint.backend_boot);
  MR_CHECK_EQ(lhs.endpoint.reference, rhs.endpoint.reference);
  MR_CHECK_EQ(lhs.endpoint.protocol, rhs.endpoint.protocol);
  MR_CHECK_EQ(lhs.endpoint.protocol_version, rhs.endpoint.protocol_version);
  MR_CHECK_EQ(lhs.trust_profile_id, rhs.trust_profile_id);
  MR_CHECK_EQ(lhs.trust_generation, rhs.trust_generation);
  MR_CHECK_EQ(lhs.trust_domain, rhs.trust_domain);
  MR_CHECK_EQ(lhs.locality, rhs.locality);
  MR_CHECK_EQ(lhs.network_distance, rhs.network_distance);
  MR_CHECK_EQ(lhs.capability_profile_id, rhs.capability_profile_id);
  MR_CHECK_EQ(lhs.capability_generation, rhs.capability_generation);
  MR_CHECK_EQ(lhs.compatibility_profile_id, rhs.compatibility_profile_id);
  MR_CHECK_EQ(lhs.compatibility_generation, rhs.compatibility_generation);
  MR_CHECK(lhs.model_bindings == rhs.model_bindings);
  MR_CHECK_EQ(lhs.reliability_ppm, rhs.reliability_ppm);
  MR_CHECK_EQ(lhs.offline_capable, rhs.offline_capable);
  MR_CHECK_EQ(lhs.provenance, rhs.provenance);
  MR_CHECK_EQ(lhs.registered_at_unix_millis, rhs.registered_at_unix_millis);
  MR_CHECK_EQ(lhs.expires_at_unix_millis, rhs.expires_at_unix_millis);
}

/// Every RouteRequest field the wire format carries.
void check_same_request(const model_router::RouteRequest& lhs,
                        const model_router::RouteRequest& rhs) {
  MR_CHECK_EQ(lhs.request_id, rhs.request_id);
  MR_CHECK_EQ(lhs.request_generation, rhs.request_generation);
  MR_CHECK_EQ(lhs.tenant, rhs.tenant);
  MR_CHECK_EQ(lhs.name_space, rhs.name_space);
  MR_CHECK_EQ(lhs.policy_id, rhs.policy_id);
  MR_CHECK_EQ(lhs.policy_generation, rhs.policy_generation);
  MR_CHECK_EQ(lhs.budget_id, rhs.budget_id);
  MR_CHECK_EQ(lhs.budget_generation, rhs.budget_generation);
  MR_CHECK_EQ(lhs.slo_id, rhs.slo_id);
  MR_CHECK_EQ(lhs.slo_generation, rhs.slo_generation);
  MR_CHECK_EQ(lhs.correlation_id, rhs.correlation_id);
  MR_CHECK_EQ(lhs.caller.agent_runtime_id, rhs.caller.agent_runtime_id);
  MR_CHECK_EQ(lhs.caller.agent_run_id, rhs.caller.agent_run_id);
  MR_CHECK_EQ(lhs.caller.action_id, rhs.caller.action_id);
  MR_CHECK_EQ(lhs.caller.action_generation, rhs.caller.action_generation);
  MR_CHECK_EQ(lhs.caller.attempt_generation, rhs.caller.attempt_generation);
  MR_CHECK_EQ(lhs.caller.work_id, rhs.caller.work_id);
  MR_CHECK_EQ(lhs.caller.work_generation, rhs.caller.work_generation);
  MR_CHECK_EQ(lhs.estimated_input_tokens, rhs.estimated_input_tokens);
  MR_CHECK_EQ(lhs.estimated_output_tokens, rhs.estimated_output_tokens);
  MR_CHECK_EQ(lhs.created_at_unix_millis, rhs.created_at_unix_millis);
  MR_CHECK_EQ(lhs.expires_at_unix_millis, rhs.expires_at_unix_millis);

  const model_router::RouteRequirements& left = lhs.requirements;
  const model_router::RouteRequirements& right = rhs.requirements;
  MR_CHECK(left.required_capabilities == right.required_capabilities);
  MR_CHECK(left.preferred_capabilities == right.preferred_capabilities);
  MR_CHECK_EQ(left.minimum_capability_evidence, right.minimum_capability_evidence);
  MR_CHECK_EQ(left.min_context_tokens, right.min_context_tokens);
  MR_CHECK_EQ(left.max_output_tokens, right.max_output_tokens);
  MR_CHECK_EQ(left.required_input_modalities.bits(), right.required_input_modalities.bits());
  MR_CHECK_EQ(left.required_output_modalities.bits(), right.required_output_modalities.bits());
  MR_CHECK_EQ(left.require_structured_output, right.require_structured_output);
  MR_CHECK_EQ(left.require_json_schema, right.require_json_schema);
  MR_CHECK_EQ(left.require_tool_calling, right.require_tool_calling);
  MR_CHECK_EQ(left.require_streaming, right.require_streaming);
  MR_CHECK_EQ(left.require_logprobs, right.require_logprobs);
  MR_CHECK_EQ(left.require_deterministic_seed, right.require_deterministic_seed);
  MR_CHECK_EQ(left.quality_floor, right.quality_floor);
  MR_CHECK(left.model_allowlist == right.model_allowlist);
  MR_CHECK(left.model_denylist == right.model_denylist);
  MR_CHECK(left.provider_allowlist == right.provider_allowlist);
  MR_CHECK(left.provider_denylist == right.provider_denylist);
  MR_CHECK(left.backend_allowlist == right.backend_allowlist);
  MR_CHECK(left.backend_denylist == right.backend_denylist);
  MR_CHECK_EQ(left.local_only, right.local_only);
  MR_CHECK_EQ(left.offline_only, right.offline_only);
  MR_CHECK_EQ(left.remote_allowed, right.remote_allowed);
  MR_CHECK(left.required_localities == right.required_localities);
  MR_CHECK(left.denied_localities == right.denied_localities);
  MR_CHECK_EQ(left.minimum_trust, right.minimum_trust);
  MR_CHECK_EQ(left.require_known_cost, right.require_known_cost);
  MR_CHECK_EQ(left.enforce_cost_ceiling, right.enforce_cost_ceiling);
  MR_CHECK(left.maximum_cost == right.maximum_cost);
  MR_CHECK_EQ(left.max_latency_micros, right.max_latency_micros);
  MR_CHECK_EQ(left.deadline_micros, right.deadline_micros);
  MR_CHECK_EQ(left.require_slo, right.require_slo);
  MR_CHECK_EQ(left.require_available, right.require_available);
  MR_CHECK_EQ(left.require_healthy, right.require_healthy);
  MR_CHECK_EQ(left.require_ready, right.require_ready);
  MR_CHECK_EQ(left.require_resident, right.require_resident);
  MR_CHECK_EQ(left.require_capacity, right.require_capacity);
  MR_CHECK_EQ(left.require_reservation, right.require_reservation);
  MR_CHECK_EQ(left.require_known_latency, right.require_known_latency);
  MR_CHECK_EQ(left.require_known_compatibility, right.require_known_compatibility);
  MR_CHECK_EQ(left.required_protocol, right.required_protocol);
  MR_CHECK_EQ(left.required_protocol_version, right.required_protocol_version);
  MR_CHECK_EQ(left.affinity_model, right.affinity_model);
  MR_CHECK_EQ(left.affinity_provider, right.affinity_provider);
  MR_CHECK_EQ(left.affinity_backend, right.affinity_backend);
  MR_CHECK_EQ(left.stickiness.prefer_same_model_family, right.stickiness.prefer_same_model_family);
  MR_CHECK_EQ(left.stickiness.prefer_same_provider, right.stickiness.prefer_same_provider);
  MR_CHECK_EQ(left.stickiness.prefer_same_backend, right.stickiness.prefer_same_backend);
  MR_CHECK_EQ(left.stickiness.prefer_context_locality, right.stickiness.prefer_context_locality);
  MR_CHECK_EQ(left.stickiness.sticky_family, right.stickiness.sticky_family);
  MR_CHECK_EQ(left.stickiness.sticky_provider, right.stickiness.sticky_provider);
  MR_CHECK_EQ(left.stickiness.sticky_backend, right.stickiness.sticky_backend);
  MR_CHECK_EQ(left.allow_fallbacks, right.allow_fallbacks);
  MR_CHECK_EQ(left.fallback_policy, right.fallback_policy);
  MR_CHECK_EQ(left.retry_policy.allow_retry, right.retry_policy.allow_retry);
  MR_CHECK_EQ(left.retry_policy.allow_reroute, right.retry_policy.allow_reroute);
  MR_CHECK_EQ(left.retry_policy.allow_model_switch, right.retry_policy.allow_model_switch);
  MR_CHECK_EQ(left.retry_policy.allow_provider_switch, right.retry_policy.allow_provider_switch);
  MR_CHECK_EQ(left.retry_policy.max_attempts, right.retry_policy.max_attempts);
  MR_CHECK_EQ(left.retry_policy.max_fallbacks, right.retry_policy.max_fallbacks);
}

[[nodiscard]] std::size_t find_marker(const std::vector<std::uint8_t>& bytes,
                                      std::string_view marker) {
  const auto found = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
  if (found == bytes.end()) {
    return bytes.size();
  }
  return static_cast<std::size_t>(std::distance(bytes.begin(), found));
}

}  // namespace

MR_TEST(protocol, every_declared_message_type_and_admin_op_has_a_name) {
  const std::array<std::string_view, static_cast<std::size_t>(MessageType::kCount)> expected = {
      "INVALID",         "HELLO",           "HELLO_ACK",       "BACKEND_REGISTER",
      "BACKEND_REGISTER_ACK", "CAPABILITY_PUBLISH", "HEALTH_UPDATE", "AVAILABILITY_UPDATE",
      "READINESS_UPDATE", "RESIDENCY_UPDATE", "CAPACITY_UPDATE", "LATENCY_UPDATE",
      "COST_UPDATE",     "ROUTE_REQUEST",   "ROUTE_RESPONSE",  "DISPATCH",
      "DISPATCH_RESULT", "COMPLETION",      "FENCE",           "PING",
      "PONG",            "BYE",             "ERROR_MESSAGE",   "SNAPSHOT_REQUEST",
      "SNAPSHOT_RESPONSE", "ADMIN_REQUEST", "ADMIN_RESPONSE"};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    MR_CHECK_EQ(model_router::distributed::to_string(static_cast<MessageType>(index)),
                expected[index]);
  }
  MR_CHECK_EQ(model_router::distributed::to_string(static_cast<MessageType>(500)), "UNKNOWN");
  for (std::uint16_t index = 0; index < static_cast<std::uint16_t>(AdminOp::kCount); ++index) {
    MR_CHECK(model_router::distributed::to_string(static_cast<AdminOp>(index)) != "INVALID");
  }
  MR_CHECK_EQ(model_router::distributed::to_string(static_cast<AdminOp>(AdminOp::kCount)),
              "INVALID");
}

MR_TEST(protocol, hello_and_hello_ack_round_trip) {
  HelloMessage hello;
  hello.router_id = model_router::RouterId(11);
  hello.router_epoch = model_router::RouterEpoch(3);
  hello.coordinator_epoch = model_router::CoordinatorEpoch(4);
  hello.backend_id = BackendId(7);
  hello.backend_generation = BackendGeneration(2);
  hello.backend_boot = BackendBootId(9);
  hello.product = "model-router-worker 1.0.0";
  hello.protocol_version = kProtocolVersion;
  check_round_trip(hello, model_router::distributed::encode_hello,
                   model_router::distributed::decode_hello);

  HelloAckMessage ack;
  ack.router_id = model_router::RouterId(11);
  ack.router_epoch = model_router::RouterEpoch(3);
  ack.coordinator_epoch = model_router::CoordinatorEpoch(4);
  ack.code = OutcomeCode::REJECT_STALE_ROUTER_EPOCH;
  ack.detail = "bound epoch is stale";
  check_round_trip(ack, model_router::distributed::encode_hello_ack,
                   model_router::distributed::decode_hello_ack);
}

MR_TEST(protocol, backend_registration_messages_round_trip) {
  mrtest::Fixture fixture;
  model_router::BackendDescriptor backend = model_router::reference::make_backend(
      fixture.catalog, BackendId(3), BackendGeneration(2), BackendBootId(5),
      BackendRegistrationGeneration(4), "127.0.0.1:7003", Provenance::SYNTHETIC, fixture.now());
  backend.reliability_ppm = 9876;
  backend.expires_at_unix_millis = fixture.now() + 60000;
  backend.capability_generation = CapabilityGeneration(6);

  BackendRegisterMessage registration;
  registration.backend = backend;
  check_encoded_round_trip(
      registration, model_router::distributed::encode_backend_register,
      model_router::distributed::decode_backend_register,
      [](const BackendRegisterMessage& decoded, const BackendRegisterMessage& original) {
        check_same_backend_descriptor(decoded.backend, original.backend);
      });

  BackendRegisterAckMessage ack;
  ack.backend_id = BackendId(3);
  ack.backend_boot = BackendBootId(5);
  ack.code = OutcomeCode::ACCEPTED;
  ack.detail = "registered";
  check_round_trip(ack, model_router::distributed::encode_backend_register_ack,
                   model_router::distributed::decode_backend_register_ack);
}

MR_TEST(protocol, capability_publication_round_trips) {
  mrtest::Fixture fixture;
  CapabilityPublishMessage message;
  message.publication.backend_id = BackendId(2);
  message.publication.backend_generation = BackendGeneration(1);
  message.publication.backend_boot = BackendBootId(4);
  message.publication.profile_id = CapabilityProfileId(202);
  message.publication.generation = CapabilityGeneration(3);
  for (const std::string_view key : {model_router::capability_keys::code,
                                     model_router::capability_keys::vision_input}) {
    model_router::CapabilityEvidence claim;
    claim.key = model_router::CapabilityKey(std::string(key));
    claim.state = CapabilityState::OBSERVED;
    claim.generation = CapabilityGeneration(3);
    claim.profile_id = message.publication.profile_id;
    claim.model_generation = model_router::ModelGeneration(1);
    claim.backend_generation = BackendGeneration(1);
    claim.backend_boot = BackendBootId(4);
    claim.observed_at_unix_millis = fixture.now();
    claim.expires_at_unix_millis = fixture.now() + 1000;
    claim.source = "protocol-test";
    claim.detail = "observed by the protocol suite";
    message.publication.claims.push_back(std::move(claim));
  }
  check_round_trip(message, model_router::distributed::encode_capability_publish,
                   model_router::distributed::decode_capability_publish);
}

MR_TEST(protocol, route_request_round_trips_every_encoded_field) {
  mrtest::Fixture fixture;
  fixture.start();
  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  request.requirements.preferred_capabilities.push_back(
      model_router::CapabilityKey(std::string(model_router::capability_keys::reasoning)));
  request.requirements.minimum_capability_evidence = CapabilityState::OBSERVED;
  request.requirements.min_context_tokens = 8192;
  request.requirements.max_output_tokens = 512;
  request.requirements.require_structured_output = true;
  request.requirements.require_json_schema = true;
  request.requirements.require_tool_calling = true;
  request.requirements.require_streaming = true;
  request.requirements.require_logprobs = true;
  request.requirements.require_deterministic_seed = true;
  request.requirements.quality_floor = 40;
  request.requirements.model_allowlist.push_back(fixture.catalog.small_model);
  request.requirements.model_denylist.push_back(fixture.catalog.specialist_model);
  request.requirements.provider_allowlist.push_back(fixture.catalog.provider);
  request.requirements.provider_denylist.push_back(model_router::ProviderId(9));
  request.requirements.backend_allowlist.push_back(BackendId(1));
  request.requirements.backend_denylist.push_back(BackendId(2));
  request.requirements.local_only = true;
  request.requirements.offline_only = true;
  request.requirements.remote_allowed = false;
  request.requirements.required_localities.push_back(fixture.catalog.locality);
  request.requirements.denied_localities.push_back(model_router::LocalityKey("remote/region"));
  request.requirements.minimum_trust = model_router::TrustDomain::LOCAL;
  request.requirements.require_known_cost = true;
  request.requirements.enforce_cost_ceiling = true;
  request.requirements.maximum_cost.unit = model_router::reference::reference_cost_unit();
  request.requirements.maximum_cost.micros = 500000;
  request.requirements.max_latency_micros = 100000;
  request.requirements.deadline_micros = 200000;
  request.requirements.require_slo = true;
  request.requirements.require_available = true;
  request.requirements.require_healthy = true;
  request.requirements.require_ready = true;
  request.requirements.require_resident = true;
  request.requirements.require_capacity = true;
  request.requirements.require_reservation = true;
  request.requirements.require_known_latency = true;
  request.requirements.require_known_compatibility = true;
  request.requirements.required_protocol = "model_router.reference";
  request.requirements.required_protocol_version = kProtocolVersion;
  request.requirements.affinity_model = fixture.catalog.general_model;
  request.requirements.affinity_provider = fixture.catalog.provider;
  request.requirements.affinity_backend = BackendId(1);
  request.requirements.stickiness.prefer_same_model_family = true;
  request.requirements.stickiness.prefer_same_provider = true;
  request.requirements.stickiness.prefer_same_backend = true;
  request.requirements.stickiness.prefer_context_locality = true;
  request.requirements.stickiness.sticky_family = model_router::ModelFamilyId(2);
  request.requirements.stickiness.sticky_provider = fixture.catalog.provider;
  request.requirements.stickiness.sticky_backend = BackendId(1);
  request.requirements.allow_fallbacks = true;
  request.requirements.fallback_policy = model_router::FallbackPolicy::PERMITTED;
  request.requirements.retry_policy.allow_retry = true;
  request.requirements.retry_policy.allow_reroute = true;
  request.requirements.retry_policy.allow_model_switch = false;
  request.requirements.retry_policy.allow_provider_switch = false;
  request.requirements.retry_policy.max_attempts = 3;
  request.requirements.retry_policy.max_fallbacks = 4;
  request.caller.agent_runtime_id = 101;
  request.caller.agent_run_id = 102;
  request.caller.action_id = 103;
  request.caller.action_generation = 104;
  request.caller.attempt_generation = 105;
  request.caller.work_id = 106;
  request.caller.work_generation = 107;
  request.budget_id = model_router::BudgetId(9);
  request.budget_generation = model_router::BudgetGeneration(2);
  request.slo_id = model_router::SLOId(5);
  request.slo_generation = model_router::SLOGeneration(3);
  request.correlation_id = model_router::CorrelationId(77);
  request.estimated_input_tokens = 1234;
  request.estimated_output_tokens = 567;
  request.expires_at_unix_millis = fixture.now() + 60000;

  RouteRequestMessage message;
  message.request = request;
  check_encoded_round_trip(
      message, model_router::distributed::encode_route_request,
      model_router::distributed::decode_route_request,
      [](const RouteRequestMessage& decoded, const RouteRequestMessage& original) {
        check_same_request(decoded.request, original.request);
      });
}

MR_TEST(protocol, route_response_round_trips_a_real_decision) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  fixture.add_backend(BackendId(1), 1);
  fixture.publish_costs(BackendId(1), PriceGeneration(1), 100, 100, 100);
  // Backend 2 is registered but publishes no evidence, so the decision carries
  // a real hard rejection alongside the winner.
  model_router::BackendDescriptor backend_two = model_router::reference::make_backend(
      fixture.catalog, BackendId(2), BackendGeneration(1), BackendBootId(1),
      BackendRegistrationGeneration(1), "127.0.0.1:7002", Provenance::SYNTHETIC, fixture.now());
  MR_CHECK(fixture.router->register_backend(std::move(backend_two)).accepted());
  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require_capability(std::string(model_router::capability_keys::code)));
  const RouteOutcome outcome = fixture.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK(!outcome.decision.explanation.ranking.empty());
  MR_CHECK(!outcome.decision.explanation.rejections.empty());

  RouteResponseMessage message;
  message.code = outcome.code;
  message.has_decision = true;
  message.decision = outcome.decision;
  message.detail = "routed by the protocol suite";

  // The wire form carries the decision and its explanation; the fields that are
  // recomputed on decode are normalized before comparison.
  model_router::RouteDecision expected = message.decision;
  expected.explanation.request_id = expected.request_id;
  expected.explanation.request_generation = expected.request_generation;
  expected.explanation.authority = expected.authority;
  expected.explanation.fallback_order = expected.fallbacks;
  expected.explanation.slo = model_router::SloEvidence{};
  expected.explanation.budget = model_router::BudgetSnapshot{};
  expected.explanation.policy = model_router::PolicySnapshot{};
  expected.dispatch_id = model_router::DispatchId{};
  expected.dispatch_generation = model_router::DispatchGeneration{};
  expected.reservation = model_router::ReservationEvidence{};
  expected.superseded_at_unix_millis = 0;

  const ResourceLimits limits;
  std::vector<std::uint8_t> bytes;
  std::string error;
  MR_CHECK(model_router::distributed::encode_route_response(message, limits, &bytes, &error));
  MR_CHECK(error.empty());
  RouteResponseMessage decoded;
  MR_CHECK(model_router::distributed::decode_route_response(bytes, limits, &decoded, &error));
  MR_CHECK(error.empty());
  MR_CHECK_EQ(decoded.code, message.code);
  MR_CHECK_EQ(decoded.has_decision, true);
  MR_CHECK_EQ(decoded.detail, message.detail);
  check_same_decision(decoded.decision, expected);
  MR_CHECK_EQ(decoded.decision.explanation.ranking[0].key, message.decision.explanation.winner);
  MR_CHECK(decoded.decision.explanation.ranking[0].score ==
           message.decision.explanation.ranking[0].score);
  MR_CHECK(decoded.decision.explanation.ranking[0].factors ==
           message.decision.explanation.ranking[0].factors);
  MR_CHECK_EQ(decoded.decision.explanation.rejections.size(),
              message.decision.explanation.rejections.size());
  MR_CHECK(decoded.decision.explanation.rejections == message.decision.explanation.rejections);

  // An invalid ranking factor byte is rejected after the rest of the message
  // decoded cleanly.
  const std::string marker = "factor-marker-source";
  message.decision.explanation.ranking[0].factors[0].source = marker;
  MR_CHECK(model_router::distributed::encode_route_response(message, limits, &bytes, &error));
  const std::size_t marker_offset = find_marker(bytes, marker);
  MR_CHECK(marker_offset != bytes.size());
  // Bytes before the source string: its 4-byte length, evidence_generation,
  // contribution, weight_ppm, raw, normalized, known, and the factor tag.
  const std::size_t factor_offset = marker_offset - 4u - (5u * 8u) - 2u;
  std::vector<std::uint8_t> invalid_factor = bytes;
  invalid_factor[factor_offset] = static_cast<std::uint8_t>(model_router::RankingFactor::kCount);
  MR_CHECK(
      !model_router::distributed::decode_route_response(invalid_factor, limits, &decoded, &error));
  MR_CHECK(!error.empty());
}

MR_TEST(protocol, dispatch_and_error_messages_round_trip) {
  DispatchMessage dispatch;
  dispatch.dispatch_id = model_router::DispatchId(21);
  dispatch.dispatch_generation = model_router::DispatchGeneration(1);
  dispatch.decision_id = model_router::RouteDecisionId(22);
  dispatch.decision_generation = model_router::RouteDecisionGeneration(1);
  dispatch.target.model_id = model_router::ModelId(3);
  dispatch.target.model_generation = model_router::ModelGeneration(1);
  dispatch.target.artifact_generation = model_router::ArtifactGeneration(1);
  dispatch.target.backend_id = BackendId(1);
  dispatch.target.backend_generation = BackendGeneration(1);
  dispatch.target.backend_boot = BackendBootId(1);
  dispatch.target.endpoint_id = model_router::EndpointId(101);
  dispatch.target.endpoint_generation = model_router::EndpointGeneration(1);
  dispatch.attempt = 2;
  check_round_trip(dispatch, model_router::distributed::encode_dispatch,
                   model_router::distributed::decode_dispatch);

  DispatchResultMessage result;
  result.dispatch_id = model_router::DispatchId(21);
  result.decision_id = model_router::RouteDecisionId(22);
  result.code = OutcomeCode::FAILED;
  result.failure = FailureClass::TRANSIENT_BACKEND_FAILURE;
  result.latency_micros = 123456;
  result.detail = "backend connection reset";
  check_round_trip(result, model_router::distributed::encode_dispatch_result,
                   model_router::distributed::decode_dispatch_result);

  ErrorMessage error_message;
  error_message.code = OutcomeCode::REJECT_STALE_BACKEND_BOOT;
  error_message.detail = "backend incarnation is fenced";
  check_round_trip(error_message, model_router::distributed::encode_error,
                   model_router::distributed::decode_error);
}

MR_TEST(protocol, dynamic_evidence_messages_round_trip) {
  mrtest::Fixture fixture;
  const UnixMillis now = fixture.now();
  model_router::HealthEvidence health;
  health.state = model_router::HealthState::DEGRADED;
  health.generation = model_router::HealthGeneration(4);
  health.backend_id = BackendId(1);
  health.backend_generation = BackendGeneration(2);
  health.backend_boot = BackendBootId(3);
  health.endpoint_generation = model_router::EndpointGeneration(5);
  health.observed_at_unix_millis = now;
  health.expires_at_unix_millis = now + 5000;
  health.source = "protocol-test";
  health.detail = "two of three probes failed";
  check_round_trip(health, model_router::distributed::encode_health,
                   model_router::distributed::decode_health);

  model_router::AvailabilityEvidence availability;
  availability.state = model_router::AvailabilityState::UNAVAILABLE;
  availability.generation = model_router::AvailabilityGeneration(2);
  availability.backend_id = BackendId(1);
  availability.backend_generation = BackendGeneration(2);
  availability.backend_boot = BackendBootId(3);
  availability.endpoint_generation = model_router::EndpointGeneration(5);
  availability.observed_at_unix_millis = now;
  availability.source = "protocol-test";
  check_round_trip(availability, model_router::distributed::encode_availability,
                   model_router::distributed::decode_availability);

  model_router::ReadinessEvidence readiness;
  readiness.state = model_router::ReadinessState::WARMING;
  readiness.generation = model_router::ReadinessGeneration(6);
  readiness.backend_id = BackendId(1);
  readiness.backend_generation = BackendGeneration(2);
  readiness.backend_boot = BackendBootId(3);
  readiness.endpoint_generation = model_router::EndpointGeneration(5);
  readiness.observed_at_unix_millis = now;
  readiness.expires_at_unix_millis = now + 1000;
  readiness.source = "protocol-test";
  check_round_trip(readiness, model_router::distributed::encode_readiness,
                   model_router::distributed::decode_readiness);

  model_router::ResidencyEvidence residency;
  residency.state = model_router::ResidencyState::LOADING;
  residency.generation = model_router::ResidencyGeneration(7);
  residency.backend_id = BackendId(1);
  residency.backend_generation = BackendGeneration(2);
  residency.backend_boot = BackendBootId(3);
  residency.endpoint_generation = model_router::EndpointGeneration(5);
  residency.observed_at_unix_millis = now;
  residency.source = "protocol-test";
  check_round_trip(residency, model_router::distributed::encode_residency,
                   model_router::distributed::decode_residency);

  model_router::LatencyEvidence latency;
  latency.health_generation = model_router::HealthGeneration(4);
  latency.backend_id = BackendId(1);
  latency.backend_boot = BackendBootId(3);
  latency.dispatch_micros = 1000;
  latency.time_to_first_token_micros = 120;
  latency.completed_micros = 5000;
  latency.tail_micros = 9000;
  latency.queue_micros = 50;
  latency.warm_start_penalty_micros = 10;
  latency.cold_start_penalty_micros = 900;
  latency.sample_count = 128;
  latency.observed_at_unix_millis = now;
  latency.source = "protocol-test";
  check_round_trip(latency, model_router::distributed::encode_latency,
                   model_router::distributed::decode_latency);

  model_router::CostEvidence cost = model_router::reference::make_cost_evidence(
      BackendId(1), fixture.catalog.general_model, PriceGeneration(2), 250, 750, 1000, now);
  cost.cache_read_micros_per_unit = 25;
  cost.cache_write_micros_per_unit = 30;
  cost.expires_at_unix_millis = now + 60000;
  check_round_trip(cost, model_router::distributed::encode_cost,
                   model_router::distributed::decode_cost);

  model_router::CapacityEvidence capacity;
  capacity.state = model_router::CapacityState::SATURATED;
  capacity.generation = model_router::CapacityGeneration(8);
  capacity.backend_id = BackendId(1);
  capacity.backend_generation = BackendGeneration(2);
  capacity.backend_boot = BackendBootId(3);
  capacity.endpoint_generation = model_router::EndpointGeneration(5);
  capacity.observed_at_unix_millis = now;
  capacity.source = "protocol-test";
  model_router::CapacityDetail detail;
  detail.generation = model_router::CapacityGeneration(8);
  detail.backend_id = BackendId(1);
  detail.backend_boot = BackendBootId(3);
  detail.available_slots = 0;
  detail.total_slots = 16;
  detail.queue_depth = 12;
  detail.max_queue_depth = 64;
  detail.observed_at_unix_millis = now;
  detail.expires_at_unix_millis = now + 1000;
  const ResourceLimits limits;
  std::vector<std::uint8_t> bytes;
  std::string error;
  MR_CHECK(model_router::distributed::encode_capacity(capacity, detail, limits, &bytes, &error));
  MR_CHECK(error.empty());
  model_router::CapacityEvidence decoded_capacity;
  model_router::CapacityDetail decoded_detail;
  MR_CHECK(model_router::distributed::decode_capacity(bytes, limits, &decoded_capacity,
                                                       &decoded_detail, &error));
  MR_CHECK(error.empty());
  MR_CHECK(decoded_capacity == capacity);
  MR_CHECK(decoded_detail == detail);
  MR_CHECK(!model_router::distributed::decode_capacity(bytes, limits, &decoded_capacity, nullptr,
                                                       &error));
  MR_CHECK(!error.empty());
}

MR_TEST(protocol, admin_messages_round_trip) {
  AdminRequest request;
  request.op = AdminOp::FENCE_BACKEND;
  request.backend_id = BackendId(2);
  request.model_id = model_router::ModelId(3);
  request.decision_id = model_router::RouteDecisionId(44);
  request.generation = 5;
  request.value = 6;
  request.detail = "fence the previous incarnation";
  check_round_trip(request, model_router::distributed::encode_admin_request,
                   model_router::distributed::decode_admin_request);

  AdminResponse response;
  response.code = OutcomeCode::ACCEPTED;
  response.detail = "fenced";
  response.json = "{\"fenced\":true,\"boot\":1}";
  check_round_trip(response, model_router::distributed::encode_admin_response,
                   model_router::distributed::decode_admin_response);

  // An unknown admin operation is rejected.
  const ResourceLimits limits;
  PayloadWriter writer(limits);
  writer.put_u16(static_cast<std::uint16_t>(AdminOp::kCount));
  writer.put_id(BackendId(1));
  writer.put_id(model_router::ModelId(1));
  writer.put_id(model_router::RouteDecisionId(1));
  writer.put_u64(1);
  writer.put_u64(1);
  writer.put_string("invalid op");
  MR_CHECK(writer.ok());
  AdminRequest decoded;
  std::string error;
  MR_CHECK(
      !model_router::distributed::decode_admin_request(writer.bytes(), limits, &decoded, &error));
  MR_CHECK(!error.empty());
}

MR_TEST(protocol, invalid_enum_tags_are_rejected) {
  const ResourceLimits limits;
  std::string error;

  // Capability state beyond the last declared state.
  {
    PayloadWriter writer(limits);
    writer.put_id(BackendId(1));
    writer.put_generation(BackendGeneration(1));
    writer.put_generation(BackendBootId(1));
    writer.put_id(CapabilityProfileId(1));
    writer.put_generation(CapabilityGeneration(1));
    writer.put_u32(1);
    writer.put_string(std::string(model_router::capability_keys::code));
    writer.put_u8(static_cast<std::uint8_t>(CapabilityState::kCount));
    writer.put_generation(CapabilityGeneration(1));
    writer.put_id(CapabilityProfileId(1));
    writer.put_generation(model_router::ModelGeneration(1));
    writer.put_generation(BackendGeneration(1));
    writer.put_generation(BackendBootId(1));
    writer.put_i64(1);
    writer.put_i64(0);
    writer.put_string("source");
    writer.put_string("detail");
    MR_CHECK(writer.ok());
    CapabilityPublishMessage decoded;
    MR_CHECK(!model_router::distributed::decode_capability_publish(writer.bytes(), limits, &decoded,
                                                                   &error));
    MR_CHECK(!error.empty());
  }
  // Failure class beyond the last declared class.
  {
    PayloadWriter writer(limits);
    writer.put_id(model_router::DispatchId(1));
    writer.put_id(model_router::RouteDecisionId(1));
    writer.put_u16(static_cast<std::uint16_t>(OutcomeCode::FAILED));
    writer.put_u8(static_cast<std::uint8_t>(FailureClass::kCount));
    writer.put_u32(1000);
    writer.put_string("invalid failure class");
    MR_CHECK(writer.ok());
    DispatchResultMessage decoded;
    MR_CHECK(!model_router::distributed::decode_dispatch_result(writer.bytes(), limits, &decoded,
                                                                &error));
    MR_CHECK(!error.empty());
  }
  // Cost knownness beyond the last declared state.
  {
    PayloadWriter writer(limits);
    writer.put_id(model_router::CostEvidenceId(1));
    writer.put_generation(PriceGeneration(1));
    writer.put_id(BackendId(1));
    writer.put_id(model_router::ModelId(1));
    writer.put_string("USD");
    writer.put_string("per-million-tokens");
    writer.put_i64(1);
    writer.put_i64(2);
    writer.put_i64(0);
    writer.put_i64(0);
    writer.put_i64(0);
    writer.put_i64(3);
    writer.put_u8(static_cast<std::uint8_t>(model_router::CostKnownness::kCount));
    writer.put_i64(1);
    writer.put_i64(0);
    writer.put_string("source");
    MR_CHECK(writer.ok());
    model_router::CostEvidence decoded;
    MR_CHECK(!model_router::distributed::decode_cost(writer.bytes(), limits, &decoded, &error));
    MR_CHECK(!error.empty());
  }
  // Health state beyond the last declared state.
  {
    PayloadWriter writer(limits);
    writer.put_u8(static_cast<std::uint8_t>(model_router::HealthState::kCount));
    writer.put_generation(model_router::HealthGeneration(1));
    writer.put_id(BackendId(1));
    writer.put_generation(BackendGeneration(1));
    writer.put_generation(BackendBootId(1));
    writer.put_generation(model_router::EndpointGeneration(1));
    writer.put_i64(1);
    writer.put_i64(0);
    writer.put_string("source");
    writer.put_string("detail");
    MR_CHECK(writer.ok());
    model_router::HealthEvidence decoded;
    MR_CHECK(!model_router::distributed::decode_health(writer.bytes(), limits, &decoded, &error));
    MR_CHECK(!error.empty());
  }
}

MR_TEST(protocol, frame_round_trips_every_header_field) {
  const ResourceLimits limits;
  const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  Frame frame = make_frame(MessageType::ROUTE_REQUEST, payload);
  frame.header.flags = 0x2Au;
  std::vector<std::uint8_t> bytes;
  std::string error;
  MR_CHECK(model_router::distributed::encode_frame(frame, limits, &bytes, &error));
  MR_CHECK(error.empty());
  MR_CHECK_EQ(bytes.size(), model_router::distributed::frame_header_bytes + payload.size());
  MR_CHECK_EQ(get_u16(bytes, kTypeOffset),
              static_cast<std::uint16_t>(MessageType::ROUTE_REQUEST));
  MR_CHECK_EQ(get_u32(bytes, kPayloadBytesOffset), static_cast<std::uint32_t>(payload.size()));
  MR_CHECK_EQ(get_u32(bytes, kPayloadCrcOffset),
              model_router::detail::crc32c(
                  std::span<const std::uint8_t>(payload.data(), payload.size())));
  MR_CHECK_EQ(get_u32(bytes, kHeaderCrcOffset),
              model_router::detail::crc32c(
                  std::span<const std::uint8_t>(bytes.data(), kHeaderCrcCoverage)));

  Frame decoded;
  MR_CHECK(model_router::distributed::decode_frame(bytes, limits, &decoded, &error));
  MR_CHECK(error.empty());
  check_header_fields(decoded.header, static_cast<std::uint16_t>(MessageType::ROUTE_REQUEST),
                      frame.header.flags, static_cast<std::uint32_t>(payload.size()));
  MR_CHECK_EQ(decoded.header.header_crc32c, get_u32(bytes, kHeaderCrcOffset));
  MR_CHECK_EQ(decoded.header.payload_crc32c, get_u32(bytes, kPayloadCrcOffset));
  MR_CHECK(decoded.payload == payload);

  FrameHeader header;
  MR_CHECK(model_router::distributed::decode_header(
      std::span<const std::uint8_t>(bytes.data(), model_router::distributed::frame_header_bytes),
      limits, &header, &error));
  MR_CHECK(error.empty());
  MR_CHECK_EQ(header.payload_bytes, decoded.header.payload_bytes);
  MR_CHECK_EQ(header.header_crc32c, decoded.header.header_crc32c);
  MR_CHECK_EQ(header.payload_crc32c, decoded.header.payload_crc32c);

  // Re-encoding the decoded frame reproduces the identical wire bytes.
  std::vector<std::uint8_t> again;
  MR_CHECK(model_router::distributed::encode_frame(decoded, limits, &again, &error));
  MR_CHECK(again == bytes);

  // An empty payload is a legal frame.
  const Frame empty = make_frame(MessageType::PING, {});
  MR_CHECK(model_router::distributed::encode_frame(empty, limits, &bytes, &error));
  MR_CHECK_EQ(bytes.size(), static_cast<std::size_t>(model_router::distributed::frame_header_bytes));
  MR_CHECK(model_router::distributed::decode_frame(bytes, limits, &decoded, &error));
  MR_CHECK(decoded.payload.empty());
  check_header_fields(decoded.header, static_cast<std::uint16_t>(MessageType::PING), 0, 0);
}

MR_TEST(protocol, malformed_frames_are_rejected) {
  const ResourceLimits limits;
  const std::vector<std::uint8_t> payload = {7, 7, 7, 7};
  const Frame frame = make_frame(MessageType::HEALTH_UPDATE, payload);
  std::vector<std::uint8_t> bytes;
  std::string error;
  MR_CHECK(model_router::distributed::encode_frame(frame, limits, &bytes, &error));
  Frame decoded;

  // Bad magic.
  {
    std::vector<std::uint8_t> bad = bytes;
    bad[0] ^= 0x01u;
    MR_CHECK(!model_router::distributed::decode_frame(bad, limits, &decoded, &error));
    MR_CHECK(error.find("magic") != std::string::npos);
    FrameHeader header;
    MR_CHECK(!model_router::distributed::decode_header(bad, limits, &header, &error));
    MR_CHECK(error.find("magic") != std::string::npos);
  }
  // Unsupported protocol version.
  {
    std::vector<std::uint8_t> bad = bytes;
    put_u16(&bad, kVersionOffset, static_cast<std::uint16_t>(kProtocolVersion + 1));
    MR_CHECK(!model_router::distributed::decode_frame(bad, limits, &decoded, &error));
    MR_CHECK(error.find("protocol version") != std::string::npos);
  }
  // Unknown message type: zero and one past the last declared type.
  for (const std::uint16_t type : {std::uint16_t{0},
                                   static_cast<std::uint16_t>(MessageType::kCount)}) {
    std::vector<std::uint8_t> bad = bytes;
    put_u16(&bad, kTypeOffset, type);
    MR_CHECK(!model_router::distributed::decode_frame(bad, limits, &decoded, &error));
    MR_CHECK(error.find("message type") != std::string::npos);
  }
  // Header checksum mismatch.
  {
    std::vector<std::uint8_t> bad = bytes;
    bad[kHeaderCrcOffset] ^= 0x01u;
    MR_CHECK(!model_router::distributed::decode_frame(bad, limits, &decoded, &error));
    MR_CHECK(error.find("header checksum") != std::string::npos);
  }
  // Payload checksum mismatch: the header checksum still covers correctly.
  {
    std::vector<std::uint8_t> bad = bytes;
    bad[kPayloadCrcOffset] ^= 0x01u;
    MR_CHECK(!model_router::distributed::decode_frame(bad, limits, &decoded, &error));
    MR_CHECK(error.find("payload checksum") != std::string::npos);
  }
  // A declared payload length beyond the configured bound.
  {
    ResourceLimits small = limits;
    small.max_frame_payload_bytes = 4;
    std::vector<std::uint8_t> bad = bytes;
    put_u32(&bad, kPayloadBytesOffset, 4096u);
    refresh_header_crc(&bad);
    FrameHeader header;
    MR_CHECK(!model_router::distributed::decode_header(bad, small, &header, &error));
    MR_CHECK(error.find("payload length exceeds") != std::string::npos);
  }
  // Truncated header and truncated frame.
  {
    std::vector<std::uint8_t> short_header(
        bytes.begin(),
        bytes.begin() + static_cast<std::ptrdiff_t>(model_router::distributed::frame_header_bytes -
                                                    1));
    FrameHeader header;
    MR_CHECK(!model_router::distributed::decode_header(short_header, limits, &header, &error));
    MR_CHECK(error.find("header is truncated") != std::string::npos);
    MR_CHECK(!model_router::distributed::decode_frame(short_header, limits, &decoded, &error));
    MR_CHECK(!error.empty());
  }
  {
    std::vector<std::uint8_t> truncated(bytes.begin(),
                                        bytes.end() - 1);  // one payload byte missing
    MR_CHECK(!model_router::distributed::decode_frame(truncated, limits, &decoded, &error));
    MR_CHECK(error.find("truncated") != std::string::npos);
    std::vector<std::uint8_t> header_only(
        bytes.begin(),
        bytes.begin() + static_cast<std::ptrdiff_t>(model_router::distributed::frame_header_bytes));
    MR_CHECK(!model_router::distributed::decode_frame(header_only, limits, &decoded, &error));
    MR_CHECK(error.find("truncated") != std::string::npos);
  }
  // Trailing bytes.
  {
    std::vector<std::uint8_t> padded = bytes;
    padded.push_back(0x00u);
    MR_CHECK(!model_router::distributed::decode_frame(padded, limits, &decoded, &error));
    MR_CHECK(error.find("trailing bytes") != std::string::npos);
  }
}

MR_TEST(protocol, frame_encoding_rejects_invalid_frames) {
  const ResourceLimits limits;
  std::vector<std::uint8_t> bytes;
  std::string error;

  Frame bad_magic = make_frame(MessageType::PING, {});
  bad_magic.header.magic = 0;
  MR_CHECK(!model_router::distributed::encode_frame(bad_magic, limits, &bytes, &error));
  MR_CHECK(error.find("magic") != std::string::npos);

  Frame bad_version = make_frame(MessageType::PING, {});
  bad_version.header.protocol_version = static_cast<std::uint16_t>(kProtocolVersion + 3);
  MR_CHECK(!model_router::distributed::encode_frame(bad_version, limits, &bytes, &error));
  MR_CHECK(error.find("protocol version") != std::string::npos);

  Frame bad_type = make_frame(MessageType::INVALID, {});
  MR_CHECK(!model_router::distributed::encode_frame(bad_type, limits, &bytes, &error));
  MR_CHECK(error.find("message type") != std::string::npos);

  Frame null_out = make_frame(MessageType::PING, {});
  MR_CHECK(!model_router::distributed::encode_frame(null_out, limits, nullptr, &error));
  MR_CHECK(error.find("null") != std::string::npos);

  Frame oversized = make_frame(MessageType::HEALTH_UPDATE, std::vector<std::uint8_t>(128, 0x11));
  ResourceLimits payload_limits = limits;
  payload_limits.max_frame_payload_bytes = 64;
  payload_limits.max_payload_bytes = 64;
  MR_CHECK(!model_router::distributed::encode_frame(oversized, payload_limits, &bytes, &error));
  MR_CHECK(error.find("payload exceeds") != std::string::npos);

  Frame fine = make_frame(MessageType::HEALTH_UPDATE, std::vector<std::uint8_t>(32, 0x22));
  ResourceLimits frame_limits = limits;
  frame_limits.max_frame_bytes = 24;
  MR_CHECK(!model_router::distributed::encode_frame(fine, frame_limits, &bytes, &error));
  MR_CHECK(error.find("byte limit") != std::string::npos);
  MR_CHECK(bytes.empty());

  // A valid header with a null output pointer is an explicit failure.
  MR_CHECK(model_router::distributed::encode_frame(fine, limits, &bytes, &error));
  FrameHeader header;
  MR_CHECK(!model_router::distributed::decode_header(
      std::span<const std::uint8_t>(bytes.data(), model_router::distributed::frame_header_bytes),
      limits, nullptr, &error));
  MR_CHECK(error.find("null") != std::string::npos);
}

MR_TEST(protocol, payload_writer_and_reader_round_trip_primitives) {
  const ResourceLimits limits;
  PayloadWriter writer(limits);
  writer.put_u8(0x12u);
  writer.put_u16(0x3456u);
  writer.put_u32(0x789ABCDEu);
  writer.put_u64(0x0123456789ABCDEFull);
  writer.put_i64(-42);
  writer.put_bool(true);
  writer.put_string("hello wire");
  writer.put_bytes(std::vector<std::uint8_t>{9, 8, 7});
  writer.put_id(BackendId(77));
  writer.put_generation(BackendGeneration(3));
  MR_CHECK(writer.ok());
  MR_CHECK(writer.error().empty());
  const std::vector<std::uint8_t>& bytes = writer.bytes();

  PayloadReader reader(std::span<const std::uint8_t>(bytes.data(), bytes.size()), limits);
  MR_CHECK_EQ(reader.get_u8(), std::uint8_t{0x12u});
  MR_CHECK_EQ(reader.get_u16(), std::uint16_t{0x3456u});
  MR_CHECK_EQ(reader.get_u32(), std::uint32_t{0x789ABCDEu});
  MR_CHECK_EQ(reader.get_u64(), std::uint64_t{0x0123456789ABCDEFull});
  MR_CHECK_EQ(reader.get_i64(), std::int64_t{-42});
  MR_CHECK_EQ(reader.get_bool(), true);
  MR_CHECK_EQ(reader.get_string(), std::string("hello wire"));
  MR_CHECK(reader.get_bytes() == std::vector<std::uint8_t>({9, 8, 7}));
  MR_CHECK_EQ(reader.get_id<BackendId>(), BackendId(77));
  MR_CHECK_EQ(reader.get_generation<BackendGeneration>(), BackendGeneration(3));
  MR_CHECK(reader.ok());
  MR_CHECK(reader.exhausted());
  MR_CHECK_EQ(reader.remaining(), std::size_t{0});
  MR_CHECK(reader.error().empty());
}

MR_TEST(protocol, payload_reader_rejects_out_of_range_and_over_limit_reads) {
  const ResourceLimits limits;
  // Reading past the end fails and sets the error instead of reading memory.
  {
    const std::uint8_t short_bytes[] = {0x01u, 0x02u};
    PayloadReader reader(std::span<const std::uint8_t>(short_bytes, sizeof(short_bytes)), limits);
    MR_CHECK_EQ(reader.remaining(), std::size_t{2});
    // The reader consumes what is present and reports failure: the caller must
    // check ok(), never the partial value.
    MR_CHECK_EQ(reader.get_u64(), std::uint64_t{0x0201});
    MR_CHECK(!reader.ok());
    MR_CHECK(!reader.error().empty());
    MR_CHECK(reader.error().find("declared field") != std::string::npos);
  }
  // An over-limit declared string length is rejected before any allocation.
  {
    ResourceLimits small = limits;
    small.max_string_bytes = 8;
    PayloadWriter writer(small);
    writer.put_u32(9);
    MR_CHECK(writer.ok());
    PayloadReader reader(std::span<const std::uint8_t>(writer.bytes().data(),
                                                       writer.bytes().size()),
                         small);
    MR_CHECK(reader.get_string().empty());
    MR_CHECK(!reader.ok());
    MR_CHECK(reader.error().find("declared string length exceeds") != std::string::npos);
  }
  // An over-limit declared byte block length is rejected as well.
  {
    ResourceLimits small = limits;
    small.max_payload_bytes = 4;
    PayloadWriter writer(small);
    writer.put_u32(5);
    MR_CHECK(writer.ok());
    PayloadReader reader(std::span<const std::uint8_t>(writer.bytes().data(),
                                                       writer.bytes().size()),
                         small);
    MR_CHECK(reader.get_bytes().empty());
    MR_CHECK(!reader.ok());
    MR_CHECK(reader.error().find("declared byte block length exceeds") != std::string::npos);
  }
  // A string whose declared length exceeds the remaining bytes fails.
  {
    PayloadWriter writer(limits);
    writer.put_u32(4);
    writer.put_u8(0xAAu);
    writer.put_u8(0xBBu);
    MR_CHECK(writer.ok());
    PayloadReader reader(std::span<const std::uint8_t>(writer.bytes().data(),
                                                       writer.bytes().size()),
                         limits);
    MR_CHECK(reader.get_string().empty());
    MR_CHECK(!reader.ok());
    MR_CHECK(reader.error().find("declared field") != std::string::npos);
  }
  // A message with trailing bytes is rejected.
  {
    ErrorMessage message;
    message.code = OutcomeCode::ACCEPTED;
    message.detail = "ok";
    std::vector<std::uint8_t> bytes;
    std::string error;
    MR_CHECK(model_router::distributed::encode_error(message, limits, &bytes, &error));
    bytes.push_back(0x00u);
    ErrorMessage decoded;
    MR_CHECK(!model_router::distributed::decode_error(bytes, limits, &decoded, &error));
    MR_CHECK(error.find("trailing bytes") != std::string::npos);
  }
  // A message payload larger than the configured bound is rejected before decode.
  {
    ErrorMessage message;
    message.code = OutcomeCode::ACCEPTED;
    message.detail = "a detail long enough to exceed one byte";
    std::vector<std::uint8_t> bytes;
    std::string error;
    MR_CHECK(model_router::distributed::encode_error(message, limits, &bytes, &error));
    ResourceLimits tiny = limits;
    tiny.max_payload_bytes = 1;
    ErrorMessage decoded;
    MR_CHECK(!model_router::distributed::decode_error(bytes, tiny, &decoded, &error));
    MR_CHECK(error.find("payload exceeds the configured limit") != std::string::npos);
  }
}

MR_TEST(protocol, payload_writer_rejects_oversized_values) {
  // An oversized string fails with a message and writes no length prefix.
  {
    ResourceLimits small;
    small.max_string_bytes = 4;
    PayloadWriter writer(small);
    writer.put_string("12345");
    MR_CHECK(!writer.ok());
    MR_CHECK(!writer.error().empty());
    MR_CHECK(writer.error().find("string exceeds the configured byte limit") != std::string::npos);
    MR_CHECK(writer.bytes().empty());
    // The failure is sticky: later writes never resurrect ok().
    writer.put_string("ok");
    MR_CHECK(!writer.ok());
  }
  // An oversized byte block fails with a message and writes nothing.
  {
    ResourceLimits small;
    small.max_payload_bytes = 4;
    PayloadWriter writer(small);
    const std::vector<std::uint8_t> block(5, 0x7Fu);
    writer.put_bytes(block);
    MR_CHECK(!writer.ok());
    MR_CHECK(writer.error().find("byte block exceeds the configured limit") != std::string::npos);
    MR_CHECK(writer.bytes().empty());
  }
  // Values at the limit are accepted.
  {
    ResourceLimits small;
    small.max_string_bytes = 4;
    small.max_payload_bytes = 4;
    PayloadWriter writer(small);
    writer.put_string("1234");
    writer.put_bytes(std::vector<std::uint8_t>(4, 0x01u));
    MR_CHECK(writer.ok());
    MR_CHECK_EQ(writer.bytes().size(), std::size_t{16});
  }
}
