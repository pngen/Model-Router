// Model Router - reference framed TCP transport over real loopback sockets.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Every synchronisation point here is a latch, a join, or a blocking socket
// operation. There is no sleep and no timeout anywhere in this suite.

#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "model_router/distributed/protocol.hpp"
#include "model_router/distributed/transport.hpp"
#include "router_fixture.hpp"

using model_router::ResourceLimits;
using model_router::distributed::CloseReason;
using model_router::distributed::Frame;
using model_router::distributed::FrameConnector;
using model_router::distributed::FrameListener;
using model_router::distributed::FramedConnection;
using model_router::distributed::MessageType;
using model_router::distributed::TransportOptions;

namespace {

[[nodiscard]] Frame make_frame(MessageType type, std::vector<std::uint8_t> payload) {
  Frame frame;
  frame.header.magic = model_router::distributed::frame_magic;
  frame.header.protocol_version = model_router::protocol_version;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.payload = std::move(payload);
  return frame;
}

/// Encodes and decodes a frame so the in-memory value is exactly what the wire
/// form carries: a received frame is compared against this, not against the
/// caller's pre-encoding header.
[[nodiscard]] Frame canonical(const Frame& frame, const ResourceLimits& limits) {
  std::vector<std::uint8_t> bytes;
  std::string error;
  MR_CHECK(model_router::distributed::encode_frame(frame, limits, &bytes, &error));
  Frame decoded;
  MR_CHECK(model_router::distributed::decode_frame(bytes, limits, &decoded, &error));
  return decoded;
}

[[nodiscard]] std::vector<std::uint8_t> sequence_payload(std::uint64_t value,
                                                         std::size_t padding) {
  std::vector<std::uint8_t> payload(8 + padding, 0x5Au);
  for (int shift = 0; shift < 64; shift += 8) {
    payload[static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((value >> shift) & 0xFFu);
  }
  return payload;
}

[[nodiscard]] std::uint64_t read_sequence(const std::vector<std::uint8_t>& payload) {
  MR_CHECK(payload.size() >= std::size_t{8});
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(payload[static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return value;
}

void check_same_frame(const Frame& actual, const Frame& expected) {
  MR_CHECK_EQ(actual.header.magic, expected.header.magic);
  MR_CHECK_EQ(actual.header.protocol_version, expected.header.protocol_version);
  MR_CHECK_EQ(actual.header.message_type, expected.header.message_type);
  MR_CHECK_EQ(actual.header.flags, expected.header.flags);
  MR_CHECK_EQ(actual.header.payload_bytes, expected.header.payload_bytes);
  MR_CHECK_EQ(actual.header.header_crc32c, expected.header.header_crc32c);
  MR_CHECK_EQ(actual.header.payload_crc32c, expected.header.payload_crc32c);
  MR_CHECK(actual.payload == expected.payload);
}

/// A listener with one accepted server-side session and one client-side session.
struct Pair {
  FrameListener listener;
  std::unique_ptr<FramedConnection> server;
  std::unique_ptr<FramedConnection> client;
  TransportOptions options;
};

void open_pair(Pair* pair, const TransportOptions& options) {
  pair->options = options;
  std::string error;
  MR_CHECK(pair->listener.start(options, &error));
  MR_CHECK(error.empty());
  MR_CHECK(pair->listener.bound_port() != 0);
  pair->client =
      FrameConnector::connect("127.0.0.1", pair->listener.bound_port(), options, &error);
  MR_CHECK(pair->client != nullptr);
  MR_CHECK(error.empty());
  pair->server = pair->listener.accept(&error);
  MR_CHECK(pair->server != nullptr);
  MR_CHECK(error.empty());
}

/// A raw TCP peer. It exists so the suite can put bytes on the wire that the
/// framed API deliberately refuses to produce, and so a peer can accept a
/// connection without ever reading from it.
class RawSocket {
 public:
  RawSocket() = default;
  ~RawSocket() { close(); }
  RawSocket(const RawSocket&) = delete;
  RawSocket& operator=(const RawSocket&) = delete;

  [[nodiscard]] bool connect_to(std::uint16_t port) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      return false;
    }
    started_ = true;
    socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) {
      return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
      return false;
    }
    if (::connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
        SOCKET_ERROR) {
      return false;
    }
    return true;
  }

  [[nodiscard]] bool send_bytes(const std::vector<std::uint8_t>& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const int sent =
          ::send(socket_, reinterpret_cast<const char*>(bytes.data() + offset),
                 static_cast<int>(bytes.size() - offset), 0);
      if (sent <= 0) {
        return false;
      }
      offset += static_cast<std::size_t>(sent);
    }
    return true;
  }

  void close() {
    if (socket_ != INVALID_SOCKET) {
      closesocket(socket_);
      socket_ = INVALID_SOCKET;
    }
    // The process-wide Winsock lifetime belongs to the library, which
    // initializes it on first use; this peer only closes its own socket.
    started_ = false;
  }

 private:
  SOCKET socket_{INVALID_SOCKET};
  bool started_{false};
};

[[nodiscard]] std::unique_ptr<FramedConnection> accept_raw_session(FrameListener* listener,
                                                                   RawSocket* raw,
                                                                   const TransportOptions& options) {
  std::string error;
  MR_CHECK(listener->start(options, &error));
  MR_CHECK(listener->bound_port() != 0);
  MR_CHECK(raw->connect_to(listener->bound_port()));
  std::unique_ptr<FramedConnection> server = listener->accept(&error);
  MR_CHECK(server != nullptr);
  MR_CHECK(error.empty());
  return server;
}

/// Ends the session from the peer side and waits until the session observes the
/// close, so destroying the connection never races a session thread that is
/// still inside its event wait.
void close_from_peer(RawSocket* raw, std::unique_ptr<FramedConnection>* connection) {
  raw->close();
  for (std::uint32_t spin = 0; spin < 1000000u && !(*connection)->closed(); ++spin) {
  }
  MR_CHECK((*connection)->closed());
  MR_CHECK((*connection)->close_reason() != CloseReason::NONE);
  MR_CHECK((*connection)->close_reason() != CloseReason::LOCAL_CLOSE);
  connection->reset();
}

[[nodiscard]] std::size_t current_thread_count() {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return 0;
  }
  THREADENTRY32 entry{};
  entry.dwSize = static_cast<DWORD>(sizeof(THREADENTRY32));
  std::size_t count = 0;
  const DWORD process = GetCurrentProcessId();
  if (Thread32First(snapshot, &entry) != FALSE) {
    do {
      if (entry.th32OwnerProcessID == process) {
        ++count;
      }
      entry.dwSize = static_cast<DWORD>(sizeof(THREADENTRY32));
    } while (Thread32Next(snapshot, &entry) != FALSE);
  }
  CloseHandle(snapshot);
  return count;
}

/// Reads the thread count until two consecutive samples agree. This is a bounded
/// spin, never a sleep, and it only settles sessions that are already ending.
[[nodiscard]] std::size_t stable_thread_count() {
  std::size_t previous = current_thread_count();
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::size_t current = current_thread_count();
    if (current == previous) {
      return current;
    }
    previous = current;
  }
  return previous;
}

}  // namespace

MR_TEST(distributed, close_reasons_are_named) {
  MR_CHECK_EQ(model_router::distributed::to_string(CloseReason::NONE), "NONE");
  MR_CHECK_EQ(model_router::distributed::to_string(CloseReason::LOCAL_CLOSE), "LOCAL_CLOSE");
  MR_CHECK_EQ(model_router::distributed::to_string(CloseReason::PEER_CLOSED), "PEER_CLOSED");
  MR_CHECK_EQ(model_router::distributed::to_string(CloseReason::TRANSPORT_ERROR),
              "TRANSPORT_ERROR");
  MR_CHECK_EQ(model_router::distributed::to_string(CloseReason::PROTOCOL_ERROR),
              "PROTOCOL_ERROR");
  MR_CHECK_EQ(model_router::distributed::to_string(CloseReason::RESOURCE_LIMIT), "RESOURCE_LIMIT");
  MR_CHECK_EQ(model_router::distributed::to_string(CloseReason::kCount), "INVALID");
}

MR_TEST(distributed, listener_reports_a_real_port_and_frames_flow_both_ways) {
  Pair pair;
  open_pair(&pair, TransportOptions{});
  const ResourceLimits limits;
  MR_CHECK(!pair.client->peer().empty());
  MR_CHECK(!pair.server->peer().empty());
  MR_CHECK(!pair.client->closed());
  MR_CHECK(!pair.server->closed());
  MR_CHECK_EQ(pair.client->frames_sent(), std::uint64_t{0});
  MR_CHECK_EQ(pair.server->frames_received(), std::uint64_t{0});

  std::string error;
  const Frame to_server =
      canonical(make_frame(MessageType::HELLO, sequence_payload(1, 16)), limits);
  MR_CHECK(pair.client->send(to_server, &error));
  MR_CHECK(error.empty());
  Frame received;
  MR_CHECK(pair.server->receive(&received, &error));
  MR_CHECK(error.empty());
  check_same_frame(received, to_server);
  MR_CHECK_EQ(pair.client->frames_sent(), std::uint64_t{1});
  MR_CHECK_EQ(pair.server->frames_received(), std::uint64_t{1});
  MR_CHECK(pair.client->bytes_sent() > std::uint64_t{0});
  MR_CHECK(pair.server->bytes_received() > std::uint64_t{0});

  const Frame to_client =
      canonical(make_frame(MessageType::ROUTE_RESPONSE, sequence_payload(2, 40)), limits);
  MR_CHECK(pair.server->send(to_client, &error));
  MR_CHECK(error.empty());
  Frame returned;
  MR_CHECK(pair.client->receive(&returned, &error));
  MR_CHECK(error.empty());
  check_same_frame(returned, to_client);
  MR_CHECK_EQ(pair.server->frames_sent(), std::uint64_t{1});
  MR_CHECK_EQ(pair.client->frames_received(), std::uint64_t{1});
}

MR_TEST(distributed, many_frames_keep_their_order_in_both_directions) {
  Pair pair;
  open_pair(&pair, TransportOptions{});
  const ResourceLimits limits;
  const std::uint32_t count = 200;
  std::string error;
  // Each frame is received before the next one is queued, so every frame keeps
  // its own handoff and the observed order is the sent order.
  for (std::uint32_t index = 0; index < count; ++index) {
    const Frame frame = canonical(
        make_frame(MessageType::HEALTH_UPDATE, sequence_payload(index, 64)), limits);
    MR_CHECK(pair.client->send(frame, &error));
    MR_CHECK(error.empty());
    Frame received;
    MR_CHECK(pair.server->receive(&received, &error));
    MR_CHECK(error.empty());
    MR_CHECK_EQ(received.header.message_type,
                static_cast<std::uint16_t>(MessageType::HEALTH_UPDATE));
    MR_CHECK_EQ(received.header.payload_crc32c, frame.header.payload_crc32c);
    MR_CHECK_EQ(read_sequence(received.payload), static_cast<std::uint64_t>(index));
    MR_CHECK_EQ(received.payload.size(), std::size_t{72});
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    const Frame frame = canonical(
        make_frame(MessageType::LATENCY_UPDATE, sequence_payload(100000u + index, 32)), limits);
    MR_CHECK(pair.server->send(frame, &error));
    MR_CHECK(error.empty());
    Frame received;
    MR_CHECK(pair.client->receive(&received, &error));
    MR_CHECK(error.empty());
    MR_CHECK_EQ(received.header.message_type,
                static_cast<std::uint16_t>(MessageType::LATENCY_UPDATE));
    MR_CHECK_EQ(read_sequence(received.payload), 100000u + static_cast<std::uint64_t>(index));
  }
  MR_CHECK_EQ(pair.server->frames_received(), static_cast<std::uint64_t>(count));
  MR_CHECK_EQ(pair.client->frames_received(), static_cast<std::uint64_t>(count));
  MR_CHECK_EQ(pair.client->frames_sent(), static_cast<std::uint64_t>(count));
  MR_CHECK_EQ(pair.server->frames_sent(), static_cast<std::uint64_t>(count));
}

MR_TEST(distributed, a_burst_of_frames_from_a_raw_peer_is_received_in_order) {
  FrameListener listener;
  RawSocket raw;
  std::unique_ptr<FramedConnection> server =
      accept_raw_session(&listener, &raw, TransportOptions{});
  const ResourceLimits limits;
  const std::uint32_t count = 200;
  std::vector<std::uint8_t> stream;
  std::string error;
  for (std::uint32_t index = 0; index < count; ++index) {
    std::vector<std::uint8_t> bytes;
    MR_CHECK(model_router::distributed::encode_frame(
        make_frame(MessageType::RESIDENCY_UPDATE, sequence_payload(index, 16)), limits, &bytes,
        &error));
    stream.insert(stream.end(), bytes.begin(), bytes.end());
  }
  // One write carrying many complete frames.
  MR_CHECK(raw.send_bytes(stream));
  for (std::uint32_t index = 0; index < count; ++index) {
    Frame frame;
    MR_CHECK(server->receive(&frame, &error));
    MR_CHECK(error.empty());
    MR_CHECK_EQ(frame.header.message_type,
                static_cast<std::uint16_t>(MessageType::RESIDENCY_UPDATE));
    MR_CHECK_EQ(read_sequence(frame.payload), static_cast<std::uint64_t>(index));
  }
  MR_CHECK_EQ(server->frames_received(), static_cast<std::uint64_t>(count));
  MR_CHECK(!server->closed());
  close_from_peer(&raw, &server);
}

MR_TEST(distributed, a_large_payload_near_the_configured_limit_round_trips) {
  TransportOptions options;
  options.limits.max_frame_payload_bytes = 512u * 1024u;
  options.limits.max_payload_bytes = 512u * 1024u;
  options.limits.max_frame_bytes = 2u * 1024u * 1024u;
  Pair pair;
  open_pair(&pair, options);

  std::vector<std::uint8_t> big(500u * 1024u);
  for (std::size_t index = 0; index < big.size(); ++index) {
    big[index] = static_cast<std::uint8_t>((index * 31u + 7u) & 0xFFu);
  }
  const Frame frame = canonical(make_frame(MessageType::CAPABILITY_PUBLISH, big), options.limits);
  std::string error;
  MR_CHECK(pair.client->send(frame, &error));
  MR_CHECK(error.empty());
  Frame received;
  MR_CHECK(pair.server->receive(&received, &error));
  MR_CHECK(error.empty());
  MR_CHECK_EQ(received.header.payload_bytes, static_cast<std::uint32_t>(big.size()));
  MR_CHECK(received.payload == big);

  // The same size travels back the other way.
  MR_CHECK(pair.server->send(frame, &error));
  MR_CHECK(pair.client->receive(&received, &error));
  MR_CHECK(received.payload == big);

  // One byte over the configured limit is refused before anything is queued.
  const std::vector<std::uint8_t> too_big(512u * 1024u + 1u, 0x5Au);
  MR_CHECK(!pair.client->send(make_frame(MessageType::CAPABILITY_PUBLISH, too_big), &error));
  MR_CHECK(!error.empty());
  MR_CHECK_EQ(pair.client->frames_sent(), std::uint64_t{1});
}

MR_TEST(distributed, close_wakes_a_blocked_receive_without_any_timeout) {
  Pair pair;
  open_pair(&pair, TransportOptions{});
  std::latch entered{1};
  std::atomic<bool> returned{false};
  std::atomic<bool> received{true};
  std::string error;
  Frame frame;
  std::thread receiver([&pair, &entered, &returned, &received, &error, &frame] {
    entered.count_down();
    received.store(pair.server->receive(&frame, &error));
    returned.store(true);
  });
  entered.wait();
  // Nothing was sent, so the reader is inside receive() and has not returned.
  MR_CHECK(!returned.load());
  MR_CHECK(!pair.server->closed());

  pair.server->close();
  // join() has no timeout: a blocked receive that close() failed to wake would
  // hang here, which is exactly the defect this test exists to catch.
  receiver.join();
  MR_CHECK(returned.load());
  MR_CHECK(!received.load());
  MR_CHECK(!error.empty());
  MR_CHECK(pair.server->closed());
  MR_CHECK_EQ(pair.server->close_reason(), CloseReason::LOCAL_CLOSE);
}

MR_TEST(distributed, peer_close_is_observed_as_peer_closed) {
  Pair pair;
  open_pair(&pair, TransportOptions{});
  // Destroying the client connection closes its socket, which the server
  // observes as a peer close.
  pair.client.reset();
  Frame frame;
  std::string error;
  MR_CHECK(!pair.server->receive(&frame, &error));
  MR_CHECK(!error.empty());
  MR_CHECK(pair.server->closed());
  MR_CHECK_EQ(pair.server->close_reason(), CloseReason::PEER_CLOSED);
  MR_CHECK(pair.server->last_error().find("peer closed") != std::string::npos);
}

MR_TEST(distributed, malformed_frames_end_the_session_with_a_protocol_error) {
  // Bad magic.
  {
    FrameListener listener;
    RawSocket raw;
    const std::unique_ptr<FramedConnection> server =
        accept_raw_session(&listener, &raw, TransportOptions{});
    const ResourceLimits limits;
    const Frame valid = canonical(make_frame(MessageType::PING, {}), limits);
    std::vector<std::uint8_t> bytes;
    std::string error;
    MR_CHECK(model_router::distributed::encode_frame(valid, limits, &bytes, &error));
    bytes[0] ^= 0xFFu;  // corrupt the magic
    MR_CHECK(raw.send_bytes(bytes));
    Frame frame;
    MR_CHECK(!server->receive(&frame, &error));
    MR_CHECK_EQ(server->close_reason(), CloseReason::PROTOCOL_ERROR);
    MR_CHECK(server->last_error().find("magic") != std::string::npos);
  }
  // Corrupted header checksum on an otherwise well-formed frame.
  {
    FrameListener listener;
    RawSocket raw;
    const std::unique_ptr<FramedConnection> server =
        accept_raw_session(&listener, &raw, TransportOptions{});
    const ResourceLimits limits;
    const Frame valid = canonical(make_frame(MessageType::PING, sequence_payload(3, 0)), limits);
    std::vector<std::uint8_t> bytes;
    std::string error;
    MR_CHECK(model_router::distributed::encode_frame(valid, limits, &bytes, &error));
    bytes[16] ^= 0x01u;  // header checksum field
    MR_CHECK(raw.send_bytes(bytes));
    Frame frame;
    MR_CHECK(!server->receive(&frame, &error));
    MR_CHECK_EQ(server->close_reason(), CloseReason::PROTOCOL_ERROR);
    MR_CHECK(server->last_error().find("checksum") != std::string::npos);
  }
}

MR_TEST(distributed, send_fails_on_a_closed_or_detached_connection) {
  Pair pair;
  open_pair(&pair, TransportOptions{});
  pair.client->close();
  MR_CHECK(pair.client->closed());
  std::string error;
  MR_CHECK(!pair.client->send(make_frame(MessageType::PING, {}), &error));
  MR_CHECK(!error.empty());
  MR_CHECK(error.find("closed") != std::string::npos ||
           error.find("session") != std::string::npos);
  Frame frame;
  MR_CHECK(!pair.client->receive(&frame, &error));
  MR_CHECK(!error.empty());

  // A connection that was never attached to a session reports that explicitly.
  FramedConnection detached;
  MR_CHECK(detached.closed());
  MR_CHECK(!detached.send(make_frame(MessageType::PING, {}), &error));
  MR_CHECK(error.find("not attached") != std::string::npos);
  MR_CHECK(!detached.receive(&frame, &error));
  MR_CHECK(error.find("not attached") != std::string::npos);
}

MR_TEST(distributed, exceeding_the_send_queue_bound_is_refused) {
  // A queue bound of zero refuses every frame, whatever the socket can absorb.
  {
    TransportOptions options;
    options.max_send_queue_frames = 0;
    FrameListener listener;
    RawSocket raw;
    std::unique_ptr<FramedConnection> server = accept_raw_session(&listener, &raw, options);
    std::string error;
    MR_CHECK(!server->send(make_frame(MessageType::PING, {}), &error));
    MR_CHECK(error.find("send queue") != std::string::npos);
    close_from_peer(&raw, &server);
  }
  // With a bound of one and a peer that never reads, the queue must eventually
  // refuse rather than grow without limit.
  {
    TransportOptions options;
    options.max_send_queue_frames = 1;
    FrameListener listener;
    RawSocket raw;
    std::unique_ptr<FramedConnection> server = accept_raw_session(&listener, &raw, options);
    const std::vector<std::uint8_t> payload(1024u * 1024u, 0x3Cu);
    const Frame frame =
        canonical(make_frame(MessageType::CAPABILITY_PUBLISH, payload), options.limits);
    std::uint32_t accepted = 0;
    bool refused = false;
    for (std::uint32_t attempt = 0; attempt < 64u && !refused; ++attempt) {
      std::string error;
      if (server->send(frame, &error)) {
        ++accepted;
      } else {
        refused = true;
        MR_CHECK(error.find("send queue") != std::string::npos);
      }
    }
    MR_CHECK(accepted >= 1u);
    MR_CHECK(refused);
    close_from_peer(&raw, &server);
  }
}

MR_TEST(distributed, repeated_start_stop_is_safe_and_leaves_no_threads) {
  const std::size_t before = stable_thread_count();
  MR_CHECK(before > std::size_t{0});
  const ResourceLimits limits;
  for (std::uint32_t iteration = 0; iteration < 60u; ++iteration) {
    TransportOptions options;
    FrameListener listener;
    std::string error;
    MR_CHECK(listener.start(options, &error));
    MR_CHECK(error.empty());
    const std::uint16_t port = listener.bound_port();
    MR_CHECK(port != 0);
    std::unique_ptr<FramedConnection> client =
        FrameConnector::connect("127.0.0.1", port, options, &error);
    MR_CHECK(client != nullptr);
    std::unique_ptr<FramedConnection> server = listener.accept(&error);
    MR_CHECK(server != nullptr);
    MR_CHECK(client->send(canonical(make_frame(MessageType::PING,
                                               sequence_payload(iteration, 8)),
                                    limits),
                          &error));
    Frame frame;
    MR_CHECK(server->receive(&frame, &error));
    MR_CHECK(error.empty());
    MR_CHECK_EQ(read_sequence(frame.payload), static_cast<std::uint64_t>(iteration));
    server->close();
    client->close();
    listener.stop();
    MR_CHECK(server->closed());
    MR_CHECK(client->closed());
  }
  const std::size_t after = stable_thread_count();
  MR_CHECK_EQ(after, before);
}

MR_TEST(distributed, two_concurrent_sessions_on_one_listener_are_independent) {
  TransportOptions options;
  FrameListener listener;
  std::string error;
  MR_CHECK(listener.start(options, &error));
  MR_CHECK(error.empty());
  const std::uint16_t port = listener.bound_port();
  MR_CHECK(port != 0);

  std::unique_ptr<FramedConnection> client_a = FrameConnector::connect("127.0.0.1", port, options,
                                                                       &error);
  MR_CHECK(client_a != nullptr);
  std::unique_ptr<FramedConnection> server_a = listener.accept(&error);
  MR_CHECK(server_a != nullptr);
  std::unique_ptr<FramedConnection> client_b = FrameConnector::connect("127.0.0.1", port, options,
                                                                       &error);
  MR_CHECK(client_b != nullptr);
  std::unique_ptr<FramedConnection> server_b = listener.accept(&error);
  MR_CHECK(server_b != nullptr);

  // Both connectors address the same listener, while each accepted session
  // has its own peer address.
  MR_CHECK_EQ(client_a->peer(), client_b->peer());
  MR_CHECK(server_a->peer() != server_b->peer());

  const ResourceLimits limits;
  const Frame frame_a = canonical(make_frame(MessageType::HELLO, sequence_payload(11, 8)), limits);
  const Frame frame_b = canonical(make_frame(MessageType::HELLO, sequence_payload(22, 8)), limits);
  MR_CHECK(client_a->send(frame_a, &error));
  MR_CHECK(client_b->send(frame_b, &error));

  Frame received_a;
  MR_CHECK(server_a->receive(&received_a, &error));
  check_same_frame(received_a, frame_a);
  MR_CHECK_EQ(read_sequence(received_a.payload), std::uint64_t{11});
  Frame received_b;
  MR_CHECK(server_b->receive(&received_b, &error));
  check_same_frame(received_b, frame_b);
  MR_CHECK_EQ(read_sequence(received_b.payload), std::uint64_t{22});
  MR_CHECK_EQ(server_a->frames_received(), std::uint64_t{1});
  MR_CHECK_EQ(server_b->frames_received(), std::uint64_t{1});

  // Closing one session leaves the other fully usable.
  server_a->close();
  MR_CHECK(server_a->closed());
  MR_CHECK(!server_b->closed());
  const Frame second = canonical(make_frame(MessageType::HEALTH_UPDATE, sequence_payload(33, 0)),
                                 limits);
  MR_CHECK(client_b->send(second, &error));
  MR_CHECK(server_b->receive(&received_b, &error));
  check_same_frame(received_b, second);
  MR_CHECK_EQ(server_b->frames_received(), std::uint64_t{2});
}
