// Model Router - reference framed TCP transport.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Reference multiprocess architecture support. The core routing library does
// not depend on this header.
//
// Session termination is event-driven: on Windows the socket is registered with
// WSAEventSelect and the session loop waits on the socket event, a wake event
// used to signal queued output, and a stop event. A blocked receive is therefore
// interrupted by an explicit stop event rather than by shutdown() alone or by
// any timeout.

#ifndef MODEL_ROUTER_DISTRIBUTED_TRANSPORT_HPP
#define MODEL_ROUTER_DISTRIBUTED_TRANSPORT_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "model_router/distributed/protocol.hpp"
#include "model_router/limits.hpp"

namespace model_router::distributed {

struct TransportOptions {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{0};
  ResourceLimits limits{};
  /// Maximum number of queued outbound frames per connection.
  std::uint32_t max_send_queue_frames{4096};
  /// Maximum number of decoded frames buffered for the reader.
  std::uint32_t max_receive_queue_frames{4096};
  /// Maximum time a connect attempt may take. Zero means the operating-system
  /// default. This is a connection-establishment bound, not a test timeout.
  std::uint32_t connect_bound_millis{0};
};

/// Reason a connection ended.
enum class CloseReason : std::uint8_t {
  NONE = 0,
  LOCAL_CLOSE,
  PEER_CLOSED,
  TRANSPORT_ERROR,
  PROTOCOL_ERROR,
  RESOURCE_LIMIT,
  kCount
};

[[nodiscard]] std::string_view to_string(CloseReason reason) noexcept;

/// A framed connection. Send is safe to call from any thread; receive is
/// intended for a single reader.
class FramedConnection {
 public:
  FramedConnection();
  ~FramedConnection();

  FramedConnection(const FramedConnection&) = delete;
  FramedConnection& operator=(const FramedConnection&) = delete;

  /// Queues p frame for transmission. Returns false when the connection is
  /// closed or the send queue is full; the frame is not partially sent.
  [[nodiscard]] bool send(const Frame& frame, std::string* error);

  /// Blocks until a complete frame is decoded, the connection closes, or
  /// stop() is called. Returns false on close.
  [[nodiscard]] bool receive(Frame* out, std::string* error);

  /// Terminates the session and wakes any blocked receive. Idempotent.
  void close();

  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] CloseReason close_reason() const noexcept;
  [[nodiscard]] std::string peer() const;
  [[nodiscard]] std::string last_error() const;

  /// Diagnostics.
  [[nodiscard]] std::uint64_t frames_sent() const noexcept;
  [[nodiscard]] std::uint64_t frames_received() const noexcept;
  [[nodiscard]] std::uint64_t bytes_sent() const noexcept;
  [[nodiscard]] std::uint64_t bytes_received() const noexcept;

  /// Opaque session state. The definition is private to the implementation; the
  /// name is visible only so the session driver can share ownership of it.
  struct SessionState;

 private:
  friend class FrameListener;
  friend class FrameConnector;

  explicit FramedConnection(std::shared_ptr<SessionState> state);

  std::shared_ptr<SessionState> state_;
};

/// A loopback TCP listener.
class FrameListener {
 public:
  FrameListener();
  ~FrameListener();

  FrameListener(const FrameListener&) = delete;
  FrameListener& operator=(const FrameListener&) = delete;

  [[nodiscard]] bool start(const TransportOptions& options, std::string* error);
  /// Blocks until a connection is accepted, the listener is stopped, or an
  /// error occurs.
  [[nodiscard]] std::unique_ptr<FramedConnection> accept(std::string* error);
  void stop();

  [[nodiscard]] std::uint16_t bound_port() const noexcept;
  [[nodiscard]] std::string last_error() const;

 private:
  struct ListenerState;
  std::shared_ptr<ListenerState> state_;
};

/// Outbound connector.
class FrameConnector {
 public:
  /// Connects to p host : p port and returns a ready framed connection.
  [[nodiscard]] static std::unique_ptr<FramedConnection> connect(
      const std::string& host, std::uint16_t port, const TransportOptions& options,
      std::string* error);
};

}  // namespace model_router::distributed

#endif  // MODEL_ROUTER_DISTRIBUTED_TRANSPORT_HPP
