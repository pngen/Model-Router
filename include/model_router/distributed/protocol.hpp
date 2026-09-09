// Model Router - reference framed wire protocol.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// This protocol belongs to the reference multiprocess architecture. The core
// routing library does not depend on it.

#ifndef MODEL_ROUTER_DISTRIBUTED_PROTOCOL_HPP
#define MODEL_ROUTER_DISTRIBUTED_PROTOCOL_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "model_router/catalog.hpp"
#include "model_router/capability.hpp"
#include "model_router/decision.hpp"
#include "model_router/limits.hpp"
#include "model_router/request.hpp"

namespace model_router::distributed {

/// Frame magic. A frame whose magic does not match is rejected before any
/// length or type is trusted.
inline constexpr std::uint32_t frame_magic = 0x4D525446u;  // "MRTF"
inline constexpr std::uint16_t frame_header_bytes = 24;

enum class MessageType : std::uint16_t {
  INVALID = 0,
  HELLO = 1,
  HELLO_ACK = 2,
  BACKEND_REGISTER = 3,
  BACKEND_REGISTER_ACK = 4,
  CAPABILITY_PUBLISH = 5,
  HEALTH_UPDATE = 6,
  AVAILABILITY_UPDATE = 7,
  READINESS_UPDATE = 8,
  RESIDENCY_UPDATE = 9,
  CAPACITY_UPDATE = 10,
  LATENCY_UPDATE = 11,
  COST_UPDATE = 12,
  ROUTE_REQUEST = 13,
  ROUTE_RESPONSE = 14,
  DISPATCH = 15,
  DISPATCH_RESULT = 16,
  COMPLETION = 17,
  FENCE = 18,
  PING = 19,
  PONG = 20,
  BYE = 21,
  ERROR_MESSAGE = 22,
  SNAPSHOT_REQUEST = 23,
  SNAPSHOT_RESPONSE = 24,
  /// Reference control-plane request. This exists so the reference proof
  /// harness can drive current-evidence changes through the real coordinator
  /// process instead of poking library internals. It is not part of the routing
  /// contract.
  ADMIN_REQUEST = 25,
  ADMIN_RESPONSE = 26,
  kCount
};

/// Reference control-plane operations.
enum class AdminOp : std::uint16_t {
  NONE = 0,
  SNAPSHOT = 1,
  INVARIANTS,
  SUMMARY,
  REVALIDATE,
  DISPATCH,
  REROUTE,
  SET_POLICY_OPEN,
  SET_POLICY_DENY_BACKEND,
  SET_POLICY_DENY_MODEL,
  SET_BUDGET_DENIED,
  SET_BUDGET_ALLOWED,
  ADVANCE_PRICE,
  FENCE_BACKEND,
  RETIRE_MODEL,
  REVOKE_CAPABILITY,
  SET_READINESS,
  SET_AVAILABILITY,
  SAVE,
  LOAD,
  SHUTDOWN,
  BACKEND_COUNT,
  kCount
};

[[nodiscard]] std::string_view to_string(AdminOp op) noexcept;

/// A control-plane request. Every field is optional and interpreted by the op.
struct AdminRequest {
  AdminOp op{AdminOp::NONE};
  BackendId backend_id{};
  ModelId model_id{};
  RouteDecisionId decision_id{};
  std::uint64_t generation{0};
  std::uint64_t value{0};
  std::string detail;

  friend bool operator==(const AdminRequest&, const AdminRequest&) = default;
};

/// A control-plane response.
struct AdminResponse {
  OutcomeCode code{OutcomeCode::INTERNAL_ERROR};
  std::string detail;
  std::string json;

  friend bool operator==(const AdminResponse&, const AdminResponse&) = default;
};

[[nodiscard]] bool encode_admin_request(const AdminRequest& message, const ResourceLimits& limits,
                                        std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_admin_request(std::span<const std::uint8_t> payload,
                                        const ResourceLimits& limits, AdminRequest* out,
                                        std::string* error);
[[nodiscard]] bool encode_admin_response(const AdminResponse& message, const ResourceLimits& limits,
                                         std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_admin_response(std::span<const std::uint8_t> payload,
                                         const ResourceLimits& limits, AdminResponse* out,
                                         std::string* error);

[[nodiscard]] std::string_view to_string(MessageType type) noexcept;

/// Fixed 24-byte frame header.
struct FrameHeader {
  std::uint32_t magic{frame_magic};
  std::uint16_t protocol_version{0};
  std::uint16_t message_type{0};
  std::uint32_t flags{0};
  std::uint32_t payload_bytes{0};
  std::uint32_t header_crc32c{0};
  std::uint32_t payload_crc32c{0};
};

/// Bounded, checked writer for message payloads. Every length is written before
/// the bytes it describes and every size is validated against ResourceLimits.
class PayloadWriter {
 public:
  explicit PayloadWriter(const ResourceLimits& limits) : limits_(limits) {}

  void put_u8(std::uint8_t value);
  void put_u16(std::uint16_t value);
  void put_u32(std::uint32_t value);
  void put_u64(std::uint64_t value);
  void put_i64(std::int64_t value);
  void put_bool(bool value);
  /// Bounded, length-prefixed string. Throws nothing: an oversized string is
  /// rejected by the caller through ok().
  void put_string(std::string_view value);
  void put_bytes(std::span<const std::uint8_t> value);
  template <class Id>
  void put_id(Id id) {
    put_u64(id.value());
  }
  template <class Gen>
  void put_generation(Gen generation) {
    put_u64(generation.value());
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

 private:
  void fail(std::string message);

  ResourceLimits limits_;
  std::vector<std::uint8_t> bytes_;
  bool ok_{true};
  std::string error_;
};

/// Bounded, checked reader. Every read validates the remaining length and every
/// declared length before allocating. Out-of-range reads fail the reader rather
/// than reading past the buffer.
class PayloadReader {
 public:
  PayloadReader(std::span<const std::uint8_t> bytes, const ResourceLimits& limits)
      : bytes_(bytes), limits_(limits) {}

  [[nodiscard]] std::uint8_t get_u8();
  [[nodiscard]] std::uint16_t get_u16();
  [[nodiscard]] std::uint32_t get_u32();
  [[nodiscard]] std::uint64_t get_u64();
  [[nodiscard]] std::int64_t get_i64();
  [[nodiscard]] bool get_bool();
  [[nodiscard]] std::string get_string();
  [[nodiscard]] std::vector<std::uint8_t> get_bytes();
  template <class Id>
  [[nodiscard]] Id get_id() {
    return Id(get_u64());
  }
  template <class Gen>
  [[nodiscard]] Gen get_generation() {
    return Gen(get_u64());
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
  /// True when every declared field was consumed exactly.
  [[nodiscard]] bool exhausted() const noexcept { return offset_ == bytes_.size(); }

 private:
  void fail(std::string message);
  [[nodiscard]] bool require(std::size_t count);

  std::span<const std::uint8_t> bytes_;
  ResourceLimits limits_;
  std::size_t offset_{0};
  bool ok_{true};
  std::string error_;
};

/// One decoded frame.
struct Frame {
  FrameHeader header{};
  std::vector<std::uint8_t> payload;

  friend bool operator==(const Frame&, const Frame&) = default;
};

/// Encodes a frame into wire bytes. Validates the magic, protocol version,
/// message type, and payload bounds.
[[nodiscard]] bool encode_frame(const Frame& frame, const ResourceLimits& limits,
                                std::vector<std::uint8_t>* out, std::string* error);

/// Decodes one frame header. Requires exactly frame_header_bytes bytes.
[[nodiscard]] bool decode_header(std::span<const std::uint8_t> bytes, const ResourceLimits& limits,
                                 FrameHeader* out, std::string* error);

/// Decodes a complete frame from wire bytes.
[[nodiscard]] bool decode_frame(std::span<const std::uint8_t> bytes, const ResourceLimits& limits,
                                Frame* out, std::string* error);

// --- Typed payloads --------------------------------------------------------

struct HelloMessage {
  RouterId router_id{};
  RouterEpoch router_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  BackendId backend_id{};
  BackendGeneration backend_generation{};
  BackendBootId backend_boot{};
  std::string product;
  std::uint32_t protocol_version{0};

  friend bool operator==(const HelloMessage&, const HelloMessage&) = default;
};

struct HelloAckMessage {
  RouterId router_id{};
  RouterEpoch router_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  OutcomeCode code{OutcomeCode::ACCEPTED};
  std::string detail;

  friend bool operator==(const HelloAckMessage&, const HelloAckMessage&) = default;
};

/// A backend registration. Only registration identity and static metadata are
/// carried: dynamic evidence is always published separately and is boot-bound.
struct BackendRegisterMessage {
  BackendDescriptor backend{};

  friend bool operator==(const BackendRegisterMessage&, const BackendRegisterMessage&) = default;
};

struct BackendRegisterAckMessage {
  BackendId backend_id{};
  BackendBootId backend_boot{};
  OutcomeCode code{OutcomeCode::ACCEPTED};
  std::string detail;

  friend bool operator==(const BackendRegisterAckMessage&,
                         const BackendRegisterAckMessage&) = default;
};

struct CapabilityPublishMessage {
  BackendCapabilityPublication publication{};

  friend bool operator==(const CapabilityPublishMessage&,
                         const CapabilityPublishMessage&) = default;
};

struct RouteRequestMessage {
  RouteRequest request{};

  friend bool operator==(const RouteRequestMessage&, const RouteRequestMessage&) = default;
};

struct RouteResponseMessage {
  OutcomeCode code{OutcomeCode::INTERNAL_ERROR};
  RouteDecision decision{};
  bool has_decision{false};
  std::string detail;

  friend bool operator==(const RouteResponseMessage&, const RouteResponseMessage&) = default;
};

struct DispatchMessage {
  DispatchId dispatch_id{};
  DispatchGeneration dispatch_generation{};
  RouteDecisionId decision_id{};
  RouteDecisionGeneration decision_generation{};
  CandidateKey target{};
  std::uint32_t attempt{0};

  friend bool operator==(const DispatchMessage&, const DispatchMessage&) = default;
};

struct DispatchResultMessage {
  DispatchId dispatch_id{};
  RouteDecisionId decision_id{};
  OutcomeCode code{OutcomeCode::INTERNAL_ERROR};
  FailureClass failure{FailureClass::UNKNOWN};
  std::uint32_t latency_micros{0};
  std::string detail;

  friend bool operator==(const DispatchResultMessage&, const DispatchResultMessage&) = default;
};

struct ErrorMessage {
  OutcomeCode code{OutcomeCode::INTERNAL_ERROR};
  std::string detail;

  friend bool operator==(const ErrorMessage&, const ErrorMessage&) = default;
};

// --- Encoders / decoders ---------------------------------------------------

[[nodiscard]] bool encode_hello(const HelloMessage& message, const ResourceLimits& limits,
                                std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_hello(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                                HelloMessage* out, std::string* error);

[[nodiscard]] bool encode_hello_ack(const HelloAckMessage& message, const ResourceLimits& limits,
                                    std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_hello_ack(std::span<const std::uint8_t> payload,
                                    const ResourceLimits& limits, HelloAckMessage* out,
                                    std::string* error);

[[nodiscard]] bool encode_backend_register(const BackendRegisterMessage& message,
                                           const ResourceLimits& limits,
                                           std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_backend_register(std::span<const std::uint8_t> payload,
                                           const ResourceLimits& limits,
                                           BackendRegisterMessage* out, std::string* error);

[[nodiscard]] bool encode_backend_register_ack(const BackendRegisterAckMessage& message,
                                               const ResourceLimits& limits,
                                               std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_backend_register_ack(std::span<const std::uint8_t> payload,
                                               const ResourceLimits& limits,
                                               BackendRegisterAckMessage* out, std::string* error);

[[nodiscard]] bool encode_capability_publish(const CapabilityPublishMessage& message,
                                             const ResourceLimits& limits,
                                             std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_capability_publish(std::span<const std::uint8_t> payload,
                                             const ResourceLimits& limits,
                                             CapabilityPublishMessage* out, std::string* error);

[[nodiscard]] bool encode_route_request(const RouteRequestMessage& message,
                                        const ResourceLimits& limits,
                                        std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_route_request(std::span<const std::uint8_t> payload,
                                        const ResourceLimits& limits, RouteRequestMessage* out,
                                        std::string* error);

[[nodiscard]] bool encode_route_response(const RouteResponseMessage& message,
                                         const ResourceLimits& limits,
                                         std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_route_response(std::span<const std::uint8_t> payload,
                                         const ResourceLimits& limits, RouteResponseMessage* out,
                                         std::string* error);

[[nodiscard]] bool encode_dispatch(const DispatchMessage& message, const ResourceLimits& limits,
                                   std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_dispatch(std::span<const std::uint8_t> payload,
                                   const ResourceLimits& limits, DispatchMessage* out,
                                   std::string* error);

[[nodiscard]] bool encode_dispatch_result(const DispatchResultMessage& message,
                                          const ResourceLimits& limits,
                                          std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_dispatch_result(std::span<const std::uint8_t> payload,
                                          const ResourceLimits& limits, DispatchResultMessage* out,
                                          std::string* error);

[[nodiscard]] bool encode_error(const ErrorMessage& message, const ResourceLimits& limits,
                                std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_error(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                                ErrorMessage* out, std::string* error);

/// Encodes the boot-bound dynamic evidence messages.
[[nodiscard]] bool encode_health(const HealthEvidence& evidence, const ResourceLimits& limits,
                                 std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_health(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                                 HealthEvidence* out, std::string* error);

[[nodiscard]] bool encode_availability(const AvailabilityEvidence& evidence,
                                       const ResourceLimits& limits,
                                       std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_availability(std::span<const std::uint8_t> payload,
                                       const ResourceLimits& limits, AvailabilityEvidence* out,
                                       std::string* error);

[[nodiscard]] bool encode_readiness(const ReadinessEvidence& evidence, const ResourceLimits& limits,
                                    std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_readiness(std::span<const std::uint8_t> payload,
                                    const ResourceLimits& limits, ReadinessEvidence* out,
                                    std::string* error);

[[nodiscard]] bool encode_residency(const ResidencyEvidence& evidence, const ResourceLimits& limits,
                                    std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_residency(std::span<const std::uint8_t> payload,
                                    const ResourceLimits& limits, ResidencyEvidence* out,
                                    std::string* error);

[[nodiscard]] bool encode_capacity(const CapacityEvidence& evidence, const CapacityDetail& detail,
                                   const ResourceLimits& limits, std::vector<std::uint8_t>* out,
                                   std::string* error);
[[nodiscard]] bool decode_capacity(std::span<const std::uint8_t> payload,
                                   const ResourceLimits& limits, CapacityEvidence* out,
                                   CapacityDetail* detail, std::string* error);

[[nodiscard]] bool encode_latency(const LatencyEvidence& evidence, const ResourceLimits& limits,
                                  std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_latency(std::span<const std::uint8_t> payload,
                                  const ResourceLimits& limits, LatencyEvidence* out,
                                  std::string* error);

/// Boot-bound price evidence published by a backend worker.
[[nodiscard]] bool encode_cost(const CostEvidence& evidence, const ResourceLimits& limits,
                               std::vector<std::uint8_t>* out, std::string* error);
[[nodiscard]] bool decode_cost(std::span<const std::uint8_t> payload, const ResourceLimits& limits,
                               CostEvidence* out, std::string* error);

}  // namespace model_router::distributed

#endif  // MODEL_ROUTER_DISTRIBUTED_PROTOCOL_HPP
