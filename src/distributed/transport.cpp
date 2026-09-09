// Model Router - reference framed TCP transport (Winsock on Windows, poll on POSIX).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// The session loop is event-driven. On Windows the socket is registered with
// WSAEventSelect and the loop waits on three events with no timeout: the socket
// event, a wake event set when output is queued, and a stop event. A blocked
// receive is therefore interrupted by an explicit stop event rather than by
// shutdown() alone or by any timeout. On POSIX the same shape uses poll() with
// an infinite timeout plus a self-pipe wakeup.
//
// No canonical Model Router lock is ever held while this transport performs I/O.

#include "model_router/distributed/transport.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace model_router::distributed {
namespace {

#if defined(_WIN32)

/// Process-wide Winsock lifetime. A function-local static gives thread-safe
/// one-time initialization and deterministic cleanup at process exit, so no
/// caller has to balance start and stop counts.
class WinsockScope {
 public:
  WinsockScope() {
    WSADATA data{};
    ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }
  ~WinsockScope() {
    if (ok_) {
      WSACleanup();
    }
  }
  WinsockScope(const WinsockScope&) = delete;
  WinsockScope& operator=(const WinsockScope&) = delete;

  [[nodiscard]] bool ok() const noexcept { return ok_; }

  [[nodiscard]] static bool started() { return instance().ok(); }

 private:
  static WinsockScope& instance() {
    static WinsockScope scope;
    return scope;
  }

  bool ok_{false};
};

#endif

/// Shared accounting so a listener can bound the number of live connections.
struct ConnectionCounter {
  std::atomic<std::uint32_t> active{0};
  std::uint32_t limit{256};
};

[[nodiscard]] std::string describe_address(const std::string& host, std::uint16_t port) {
  return host + ":" + std::to_string(port);
}

}  // namespace

std::string_view to_string(CloseReason reason) noexcept {
  switch (reason) {
    case CloseReason::NONE:
      return "NONE";
    case CloseReason::LOCAL_CLOSE:
      return "LOCAL_CLOSE";
    case CloseReason::PEER_CLOSED:
      return "PEER_CLOSED";
    case CloseReason::TRANSPORT_ERROR:
      return "TRANSPORT_ERROR";
    case CloseReason::PROTOCOL_ERROR:
      return "PROTOCOL_ERROR";
    case CloseReason::RESOURCE_LIMIT:
      return "RESOURCE_LIMIT";
    default:
      return "INVALID";
  }
}

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------

struct FramedConnection::SessionState {
  TransportOptions options{};
#if defined(_WIN32)
  SOCKET socket{INVALID_SOCKET};
  WSAEVENT socket_event{WSA_INVALID_EVENT};
  WSAEVENT wake_event{WSA_INVALID_EVENT};
  WSAEVENT stop_event{WSA_INVALID_EVENT};
#else
  int socket{-1};
  int wake_pipe[2]{-1, -1};
#endif
  std::shared_ptr<ConnectionCounter> counter;
  std::thread worker;
  std::thread::id worker_id;
  std::atomic<bool> closed{false};
  /// Set before the stop event is signaled so a blocked receive() returns even
  /// if the session thread is between waits.
  std::atomic<bool> stop_requested{false};
  CloseReason reason{CloseReason::NONE};
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<Frame> inbound;
  std::deque<Frame> outbound;
  std::vector<std::uint8_t> receive_buffer;
  std::vector<std::uint8_t> pending_write;
  std::size_t pending_offset{0};
  std::string peer;
  std::string last_error;
  std::atomic<std::uint64_t> frames_sent{0};
  std::atomic<std::uint64_t> frames_received{0};
  std::atomic<std::uint64_t> bytes_sent{0};
  std::atomic<std::uint64_t> bytes_received{0};

  ~SessionState() { release_socket(); }

  void set_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex);
    if (last_error.empty()) {
      last_error = message;
    }
  }

  void mark_closed(CloseReason close_reason, const std::string& message) {
    bool first = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!closed.exchange(true)) {
        first = true;
        reason = close_reason;
        if (!message.empty() && last_error.empty()) {
          last_error = message;
        }
      }
    }
    condition.notify_all();
    if (first) {
      release_socket();
      if (counter != nullptr && counter->active.load() > 0) {
        counter->active.fetch_sub(1);
      }
    }
  }

  void release_socket() {
#if defined(_WIN32)
    if (socket_event != WSA_INVALID_EVENT) {
      WSACloseEvent(socket_event);
      socket_event = WSA_INVALID_EVENT;
    }
    if (wake_event != WSA_INVALID_EVENT) {
      WSACloseEvent(wake_event);
      wake_event = WSA_INVALID_EVENT;
    }
    if (stop_event != WSA_INVALID_EVENT) {
      WSACloseEvent(stop_event);
      stop_event = WSA_INVALID_EVENT;
    }
    if (socket != INVALID_SOCKET) {
      closesocket(socket);
      socket = INVALID_SOCKET;
    }
#else
    if (socket >= 0) {
      ::close(socket);
      socket = -1;
    }
    for (int& descriptor : wake_pipe) {
      if (descriptor >= 0) {
        ::close(descriptor);
        descriptor = -1;
      }
    }
#endif
  }

  void wake_writer() {
#if defined(_WIN32)
    if (wake_event != WSA_INVALID_EVENT) {
      WSASetEvent(wake_event);
    }
#else
    if (wake_pipe[1] >= 0) {
      const std::uint8_t byte = 1;
      const ssize_t written = ::write(wake_pipe[1], &byte, 1);
      (void)written;
    }
#endif
  }

  void request_stop() {
#if defined(_WIN32)
    if (stop_event != WSA_INVALID_EVENT) {
      WSASetEvent(stop_event);
    }
#else
    if (wake_pipe[1] >= 0) {
      const std::uint8_t byte = 1;
      const ssize_t written = ::write(wake_pipe[1], &byte, 1);
      (void)written;
    }
#endif
  }
};

// ---------------------------------------------------------------------------
// FramedConnection
// ---------------------------------------------------------------------------

FramedConnection::FramedConnection() = default;

FramedConnection::FramedConnection(std::shared_ptr<SessionState> state) : state_(std::move(state)) {}

FramedConnection::~FramedConnection() { close(); }

bool FramedConnection::send(const Frame& frame, std::string* error) {
  if (state_ == nullptr) {
    if (error != nullptr) {
      *error = "connection is not attached to a session";
    }
    return false;
  }
  std::vector<std::uint8_t> encoded;
  std::string encode_error;
  if (!encode_frame(frame, state_->options.limits, &encoded, &encode_error)) {
    if (error != nullptr) {
      *error = encode_error;
    }
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closed.load()) {
      if (error != nullptr) {
        *error = "connection is closed";
      }
      return false;
    }
    // The bound covers queued frames plus the frame currently being written, so
    // it is observable even while the session thread is draining the queue.
    const std::size_t pending = state_->pending_write.empty() ? 0u : 1u;
    if (state_->outbound.size() + pending >= state_->options.max_send_queue_frames) {
      if (error != nullptr) {
        *error = "send queue is full";
      }
      return false;
    }
    state_->outbound.push_back(frame);
  }
  state_->wake_writer();
  return true;
}

bool FramedConnection::receive(Frame* out, std::string* error) {
  if (state_ == nullptr) {
    if (error != nullptr) {
      *error = "connection is not attached to a session";
    }
    return false;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  state_->condition.wait(lock, [this] {
    return state_->closed.load() || state_->stop_requested.load() || !state_->inbound.empty();
  });
  if (!state_->inbound.empty()) {
    if (out != nullptr) {
      *out = std::move(state_->inbound.front());
    }
    state_->inbound.pop_front();
    return true;
  }
  if (error != nullptr) {
    *error = state_->last_error.empty() ? std::string("connection is closed")
                                        : state_->last_error;
  }
  return false;
}

void FramedConnection::close() {
  if (state_ == nullptr) {
    return;
  }
  // Order matters: the session thread may be blocked in the event wait, so the
  // stop request must be signaled while the events still exist. The socket and
  // events are released only after the session thread has finished with them.
  state_->stop_requested.store(true);
  state_->request_stop();
  state_->wake_writer();
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
  }
  state_->condition.notify_all();
  if (state_->worker.joinable()) {
    if (std::this_thread::get_id() == state_->worker_id) {
      // The session thread is tearing down its own connection: detaching is
      // safe because the thread holds a shared_ptr to the session state.
      state_->worker.detach();
    } else {
      state_->worker.join();
    }
  }
  // Exactly once, and only after no thread can still be waiting on the events.
  state_->mark_closed(CloseReason::LOCAL_CLOSE, {});
  state_.reset();
}

bool FramedConnection::closed() const noexcept {
  return state_ == nullptr || state_->closed.load();
}

CloseReason FramedConnection::close_reason() const noexcept {
  if (state_ == nullptr) {
    return CloseReason::LOCAL_CLOSE;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->reason;
}

std::string FramedConnection::peer() const {
  if (state_ == nullptr) {
    return {};
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->peer;
}

std::string FramedConnection::last_error() const {
  if (state_ == nullptr) {
    return "connection was closed";
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->last_error;
}

std::uint64_t FramedConnection::frames_sent() const noexcept {
  return state_ == nullptr ? 0 : state_->frames_sent.load();
}
std::uint64_t FramedConnection::frames_received() const noexcept {
  return state_ == nullptr ? 0 : state_->frames_received.load();
}
std::uint64_t FramedConnection::bytes_sent() const noexcept {
  return state_ == nullptr ? 0 : state_->bytes_sent.load();
}
std::uint64_t FramedConnection::bytes_received() const noexcept {
  return state_ == nullptr ? 0 : state_->bytes_received.load();
}

// ---------------------------------------------------------------------------
// Session driver
// ---------------------------------------------------------------------------

namespace {

#if defined(_WIN32)

void session_loop_windows(const std::shared_ptr<FramedConnection::SessionState>& state) {
  state->worker_id = std::this_thread::get_id();
  HANDLE events[3] = {reinterpret_cast<HANDLE>(state->socket_event),
                      reinterpret_cast<HANDLE>(state->wake_event),
                      reinterpret_cast<HANDLE>(state->stop_event)};

  while (!state->closed.load()) {
    const DWORD index = WSAWaitForMultipleEvents(3, events, FALSE, WSA_INFINITE, FALSE);
    if (index == WSA_WAIT_FAILED) {
      state->mark_closed(CloseReason::TRANSPORT_ERROR, "event wait failed");
      return;
    }
    const DWORD slot = index - WSA_WAIT_EVENT_0;
    if (slot == 2) {
      state->mark_closed(CloseReason::LOCAL_CLOSE, {});
      return;
    }
    if (slot == 1) {
      WSAResetEvent(state->wake_event);
    } else {
      WSANETWORKEVENTS network_events{};
      if (WSAEnumNetworkEvents(state->socket, state->socket_event, &network_events) ==
          SOCKET_ERROR) {
        state->mark_closed(CloseReason::TRANSPORT_ERROR, "enum network events failed");
        return;
      }
      if ((network_events.lNetworkEvents & FD_CLOSE) != 0) {
        state->mark_closed(CloseReason::PEER_CLOSED, "peer closed the connection");
        return;
      }
    }

    // Drain and flush the whole outbound queue in one pass. The wake event is
    // binary, so a burst of send() calls coalesces into a single wake; draining
    // only one frame per wake would strand the rest of the queue forever.
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->pending_write.empty()) {
          if (state->outbound.empty()) {
            break;
          }
          Frame frame = std::move(state->outbound.front());
          state->outbound.pop_front();
          std::vector<std::uint8_t> encoded;
          std::string error;
          if (!encode_frame(frame, state->options.limits, &encoded, &error)) {
            state->set_error(error);
            continue;
          }
          state->pending_write = std::move(encoded);
          state->pending_offset = 0;
        }
      }
      std::size_t offset = 0;
      std::size_t remaining = 0;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        remaining = state->pending_write.size() - state->pending_offset;
        offset = state->pending_offset;
      }
      if (remaining == 0) {
        continue;
      }
      const int sent = ::send(state->socket,
                              reinterpret_cast<const char*>(state->pending_write.data() + offset),
                              static_cast<int>(remaining), 0);
      if (sent > 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->pending_offset += static_cast<std::size_t>(sent);
        state->bytes_sent += static_cast<std::uint64_t>(sent);
        if (state->pending_offset >= state->pending_write.size()) {
          state->pending_write.clear();
          state->pending_offset = 0;
          state->frames_sent += 1;
        }
        continue;
      }
      if (sent == SOCKET_ERROR) {
        const int code = WSAGetLastError();
        if (code == WSAEWOULDBLOCK) {
          break;
        }
        state->mark_closed(CloseReason::TRANSPORT_ERROR,
                           "send failed with code " + std::to_string(code));
        return;
      }
      break;
    }

    // Read whatever is available and decode complete frames.
    for (;;) {
      std::uint8_t buffer[4096];
      const int received = ::recv(state->socket, reinterpret_cast<char*>(buffer),
                                  static_cast<int>(sizeof(buffer)), 0);
      if (received > 0) {
        std::vector<Frame> decoded;
        std::string error;
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->bytes_received += static_cast<std::uint64_t>(received);
          state->receive_buffer.insert(state->receive_buffer.end(), buffer,
                                       buffer + received);
        }
        for (;;) {
          std::size_t available = 0;
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            available = state->receive_buffer.size();
          }
          if (available < frame_header_bytes) {
            break;
          }
          FrameHeader header;
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!decode_header(std::span<const std::uint8_t>(state->receive_buffer.data(),
                                                             frame_header_bytes),
                               state->options.limits, &header, &error)) {
              break;
            }
          }
          const std::size_t total = frame_header_bytes + header.payload_bytes;
          if (available < total) {
            break;
          }
          Frame frame;
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!decode_frame(
                    std::span<const std::uint8_t>(state->receive_buffer.data(), total),
                    state->options.limits, &frame, &error)) {
              break;
            }
            state->receive_buffer.erase(
                state->receive_buffer.begin(),
                state->receive_buffer.begin() + static_cast<std::ptrdiff_t>(total));
          }
          decoded.push_back(std::move(frame));
        }
        if (!decoded.empty()) {
          std::lock_guard<std::mutex> lock(state->mutex);
          if (state->inbound.size() + decoded.size() > state->options.max_receive_queue_frames) {
            state->set_error("receive queue is full");
            state->closed.store(true);
            state->reason = CloseReason::RESOURCE_LIMIT;
            state->condition.notify_all();
            return;
          }
          for (Frame& frame : decoded) {
            state->inbound.push_back(std::move(frame));
            state->frames_received += 1;
          }
          state->condition.notify_all();
        }
        if (!error.empty()) {
          state->mark_closed(CloseReason::PROTOCOL_ERROR, error);
          return;
        }
        continue;
      }
      if (received == 0) {
        state->mark_closed(CloseReason::PEER_CLOSED, "peer closed the connection");
        return;
      }
      const int code = WSAGetLastError();
      if (code == WSAEWOULDBLOCK) {
        break;
      }
      state->mark_closed(CloseReason::TRANSPORT_ERROR,
                         "recv failed with code " + std::to_string(code));
      return;
    }
  }
  state->mark_closed(CloseReason::LOCAL_CLOSE, {});
}

#else

void session_loop_posix(const std::shared_ptr<FramedConnection::SessionState>& state) {
  state->worker_id = std::this_thread::get_id();
  while (!state->closed.load()) {
    pollfd descriptors[2];
    descriptors[0].fd = state->socket;
    descriptors[0].events = POLLIN | POLLOUT;
    descriptors[0].revents = 0;
    descriptors[1].fd = state->wake_pipe[0];
    descriptors[1].events = POLLIN;
    descriptors[1].revents = 0;
    const int ready = ::poll(descriptors, 2, -1);
    if (ready < 0) {
      state->mark_closed(CloseReason::TRANSPORT_ERROR, "poll failed");
      return;
    }
    if ((descriptors[1].revents & POLLIN) != 0) {
      std::uint8_t byte = 0;
      const ssize_t read_count = ::read(state->wake_pipe[0], &byte, 1);
      (void)read_count;
    }
    if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      state->mark_closed(CloseReason::PEER_CLOSED, "peer closed the connection");
      return;
    }
    if ((descriptors[0].revents & POLLIN) != 0) {
      std::uint8_t buffer[4096];
      const ssize_t received = ::recv(state->socket, buffer, sizeof(buffer), 0);
      if (received > 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->bytes_received += static_cast<std::uint64_t>(received);
        state->receive_buffer.insert(state->receive_buffer.end(), buffer,
                                     buffer + received);
      } else if (received == 0) {
        state->mark_closed(CloseReason::PEER_CLOSED, "peer closed the connection");
        return;
      }
    }
    if ((descriptors[0].revents & POLLOUT) != 0) {
      for (;;) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->pending_write.empty()) {
          if (state->outbound.empty()) {
            break;
          }
          Frame frame = std::move(state->outbound.front());
          state->outbound.pop_front();
          std::vector<std::uint8_t> encoded;
          std::string error;
          if (!encode_frame(frame, state->options.limits, &encoded, &error)) {
            continue;
          }
          state->pending_write = std::move(encoded);
          state->pending_offset = 0;
        }
        const ssize_t sent =
            ::send(state->socket, state->pending_write.data() + state->pending_offset,
                   state->pending_write.size() - state->pending_offset, 0);
        if (sent <= 0) {
          break;
        }
        state->pending_offset += static_cast<std::size_t>(sent);
        state->bytes_sent += static_cast<std::uint64_t>(sent);
        if (state->pending_offset >= state->pending_write.size()) {
          state->pending_write.clear();
          state->pending_offset = 0;
          state->frames_sent += 1;
        }
      }
    }
    // Decode complete frames from the receive buffer.
    for (;;) {
      Frame frame;
      std::string error;
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->receive_buffer.size() < frame_header_bytes) {
        break;
      }
      FrameHeader header;
      if (!decode_header(
              std::span<const std::uint8_t>(state->receive_buffer.data(), frame_header_bytes),
              state->options.limits, &header, &error)) {
        state->set_error(error);
        state->closed.store(true);
        state->reason = CloseReason::PROTOCOL_ERROR;
        state->condition.notify_all();
        return;
      }
      const std::size_t total = frame_header_bytes + header.payload_bytes;
      if (state->receive_buffer.size() < total) {
        break;
      }
      if (!decode_frame(std::span<const std::uint8_t>(state->receive_buffer.data(), total),
                        state->options.limits, &frame, &error)) {
        state->set_error(error);
        state->closed.store(true);
        state->reason = CloseReason::PROTOCOL_ERROR;
        state->condition.notify_all();
        return;
      }
      state->receive_buffer.erase(
          state->receive_buffer.begin(),
          state->receive_buffer.begin() + static_cast<std::ptrdiff_t>(total));
      state->inbound.push_back(std::move(frame));
      state->frames_received += 1;
      state->condition.notify_all();
    }
  }
  state->mark_closed(CloseReason::LOCAL_CLOSE, {});
}

#endif

}  // namespace

// ---------------------------------------------------------------------------
// Listener
// ---------------------------------------------------------------------------

struct FrameListener::ListenerState {
  TransportOptions options{};
  std::shared_ptr<ConnectionCounter> counter = std::make_shared<ConnectionCounter>();
  std::mutex mutex;
  std::string last_error;
  std::atomic<bool> stopped{false};
  std::uint16_t bound_port{0};
#if defined(_WIN32)
  SOCKET socket{INVALID_SOCKET};
  WSAEVENT accept_event{WSA_INVALID_EVENT};
  WSAEVENT stop_event{WSA_INVALID_EVENT};
#else
  int socket{-1};
  int stop_pipe[2]{-1, -1};
#endif

  ~ListenerState() { release(); }

  void release() {
#if defined(_WIN32)
    if (accept_event != WSA_INVALID_EVENT) {
      WSACloseEvent(accept_event);
      accept_event = WSA_INVALID_EVENT;
    }
    if (stop_event != WSA_INVALID_EVENT) {
      WSACloseEvent(stop_event);
      stop_event = WSA_INVALID_EVENT;
    }
    if (socket != INVALID_SOCKET) {
      closesocket(socket);
      socket = INVALID_SOCKET;
    }
#else
    if (socket >= 0) {
      ::close(socket);
      socket = -1;
    }
    for (int& descriptor : stop_pipe) {
      if (descriptor >= 0) {
        ::close(descriptor);
        descriptor = -1;
      }
    }
#endif
  }
};

FrameListener::FrameListener() : state_(std::make_shared<ListenerState>()) {}

FrameListener::~FrameListener() { stop(); }

bool FrameListener::start(const TransportOptions& options, std::string* error) {
  if (state_ == nullptr) {
    state_ = std::make_shared<ListenerState>();
  }
#if defined(_WIN32)
  if (!WinsockScope::started()) {
    if (error != nullptr) {
      *error = "Winsock could not be initialized";
    }
    return false;
  }
#endif
  state_->options = options;
  state_->counter->limit = options.limits.max_connections;
  state_->stopped.store(false);

#if defined(_WIN32)
  const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    if (error != nullptr) {
      *error = "listener socket could not be created";
    }
    return false;
  }
  BOOL exclusive = TRUE;
  (void)::setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(options.port);
  if (::inet_pton(AF_INET, options.bind_address.c_str(), &address.sin_addr) != 1) {
    closesocket(listener);
    if (error != nullptr) {
      *error = "bind address is not a valid IPv4 literal";
    }
    return false;
  }
  if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
      SOCKET_ERROR) {
    closesocket(listener);
    if (error != nullptr) {
      *error = "listener could not bind " + describe_address(options.bind_address, options.port);
    }
    return false;
  }
  if (::listen(listener, SOMAXCONN) == SOCKET_ERROR) {
    closesocket(listener);
    if (error != nullptr) {
      *error = "listener could not listen";
    }
    return false;
  }
  sockaddr_in bound{};
  int bound_length = sizeof(bound);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
    state_->bound_port = ntohs(bound.sin_port);
  }
  state_->socket = listener;
  state_->accept_event = WSACreateEvent();
  state_->stop_event = WSACreateEvent();
  if (state_->accept_event == WSA_INVALID_EVENT || state_->stop_event == WSA_INVALID_EVENT) {
    state_->release();
    if (error != nullptr) {
      *error = "listener events could not be created";
    }
    return false;
  }
  if (WSAEventSelect(listener, state_->accept_event, FD_ACCEPT) == SOCKET_ERROR) {
    state_->release();
    if (error != nullptr) {
      *error = "listener could not register for accept events";
    }
    return false;
  }
#else
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    if (error != nullptr) {
      *error = "listener socket could not be created";
    }
    return false;
  }
  const int reuse = 1;
  (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(options.port);
  if (::inet_pton(AF_INET, options.bind_address.c_str(), &address.sin_addr) != 1) {
    ::close(listener);
    if (error != nullptr) {
      *error = "bind address is not a valid IPv4 literal";
    }
    return false;
  }
  if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(listener);
    if (error != nullptr) {
      *error = "listener could not bind " + describe_address(options.bind_address, options.port);
    }
    return false;
  }
  if (::listen(listener, SOMAXCONN) != 0) {
    ::close(listener);
    if (error != nullptr) {
      *error = "listener could not listen";
    }
    return false;
  }
  sockaddr_in bound{};
  socklen_t bound_length = sizeof(bound);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
    state_->bound_port = ntohs(bound.sin_port);
  }
  state_->socket = listener;
  if (::pipe(state_->stop_pipe) != 0) {
    state_->release();
    if (error != nullptr) {
      *error = "listener wake pipe could not be created";
    }
    return false;
  }
#endif
  return true;
}

std::unique_ptr<FramedConnection> FrameListener::accept(std::string* error) {
  if (state_ == nullptr || state_->stopped.load()) {
    if (error != nullptr) {
      *error = "listener is stopped";
    }
    return nullptr;
  }
#if defined(_WIN32)
  HANDLE events[2] = {reinterpret_cast<HANDLE>(state_->accept_event),
                      reinterpret_cast<HANDLE>(state_->stop_event)};
  for (;;) {
    const DWORD index = WSAWaitForMultipleEvents(2, events, FALSE, WSA_INFINITE, FALSE);
    if (index == WSA_WAIT_FAILED) {
      if (error != nullptr) {
        *error = "listener event wait failed";
      }
      return nullptr;
    }
    if (index - WSA_WAIT_EVENT_0 == 1) {
      if (error != nullptr) {
        *error = "listener is stopped";
      }
      return nullptr;
    }
    WSANETWORKEVENTS network_events{};
    if (WSAEnumNetworkEvents(state_->socket, state_->accept_event, &network_events) ==
        SOCKET_ERROR) {
      if (error != nullptr) {
        *error = "listener could not enumerate network events";
      }
      return nullptr;
    }
    if ((network_events.lNetworkEvents & FD_ACCEPT) == 0) {
      continue;
    }
    sockaddr_in peer{};
    int peer_length = sizeof(peer);
    const SOCKET accepted =
        ::accept(state_->socket, reinterpret_cast<sockaddr*>(&peer), &peer_length);
    if (accepted == INVALID_SOCKET) {
      continue;
    }
    if (state_->counter->active.load() >= state_->counter->limit) {
      closesocket(accepted);
      if (error != nullptr) {
        *error = "connection limit reached";
      }
      return nullptr;
    }
    auto session = std::make_shared<FramedConnection::SessionState>();
    session->options = state_->options;
    session->socket = accepted;
    session->counter = state_->counter;
    session->socket_event = WSACreateEvent();
    session->wake_event = WSACreateEvent();
    session->stop_event = WSACreateEvent();
    if (session->socket_event == WSA_INVALID_EVENT ||
        session->wake_event == WSA_INVALID_EVENT || session->stop_event == WSA_INVALID_EVENT) {
      if (error != nullptr) {
        *error = "session events could not be created";
      }
      return nullptr;
    }
    if (WSAEventSelect(accepted, session->socket_event,
                       FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR) {
      if (error != nullptr) {
        *error = "session could not register for socket events";
      }
      return nullptr;
    }
    char peer_text[64] = {};
    if (::inet_ntop(AF_INET, &peer.sin_addr, peer_text, sizeof(peer_text)) != nullptr) {
      session->peer = std::string(peer_text) + ":" + std::to_string(ntohs(peer.sin_port));
    }
    state_->counter->active.fetch_add(1);
    session->worker = std::thread(session_loop_windows, session);
    return std::unique_ptr<FramedConnection>(new FramedConnection(std::move(session)));
  }
#else
  for (;;) {
    pollfd descriptors[2];
    descriptors[0].fd = state_->socket;
    descriptors[0].events = POLLIN;
    descriptors[0].revents = 0;
    descriptors[1].fd = state_->stop_pipe[0];
    descriptors[1].events = POLLIN;
    descriptors[1].revents = 0;
    if (::poll(descriptors, 2, -1) < 0) {
      if (error != nullptr) {
        *error = "listener poll failed";
      }
      return nullptr;
    }
    if ((descriptors[1].revents & POLLIN) != 0) {
      if (error != nullptr) {
        *error = "listener is stopped";
      }
      return nullptr;
    }
    if ((descriptors[0].revents & POLLIN) == 0) {
      continue;
    }
    sockaddr_in peer{};
    socklen_t peer_length = sizeof(peer);
    const int accepted = ::accept(state_->socket, reinterpret_cast<sockaddr*>(&peer), &peer_length);
    if (accepted < 0) {
      continue;
    }
    if (state_->counter->active.load() >= state_->counter->limit) {
      ::close(accepted);
      if (error != nullptr) {
        *error = "connection limit reached";
      }
      return nullptr;
    }
    auto session = std::make_shared<FramedConnection::SessionState>();
    session->options = state_->options;
    session->socket = accepted;
    session->counter = state_->counter;
    if (::pipe(session->wake_pipe) != 0) {
      if (error != nullptr) {
        *error = "session wake pipe could not be created";
      }
      return nullptr;
    }
    char peer_text[64] = {};
    if (::inet_ntop(AF_INET, &peer.sin_addr, peer_text, sizeof(peer_text)) != nullptr) {
      session->peer = std::string(peer_text) + ":" + std::to_string(ntohs(peer.sin_port));
    }
    state_->counter->active.fetch_add(1);
    session->worker = std::thread(session_loop_posix, session);
    return std::unique_ptr<FramedConnection>(new FramedConnection(std::move(session)));
  }
#endif
}

void FrameListener::stop() {
  if (state_ == nullptr) {
    return;
  }
  if (!state_->stopped.exchange(true)) {
#if defined(_WIN32)
    if (state_->stop_event != WSA_INVALID_EVENT) {
      WSASetEvent(state_->stop_event);
    }
#else
    if (state_->stop_pipe[1] >= 0) {
      const std::uint8_t byte = 1;
      const ssize_t written = ::write(state_->stop_pipe[1], &byte, 1);
      (void)written;
    }
#endif
  }
  state_->release();
}

std::uint16_t FrameListener::bound_port() const noexcept {
  return state_ == nullptr ? 0 : state_->bound_port;
}

std::string FrameListener::last_error() const {
  if (state_ == nullptr) {
    return {};
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->last_error;
}

// ---------------------------------------------------------------------------
// Connector
// ---------------------------------------------------------------------------

std::unique_ptr<FramedConnection> FrameConnector::connect(const std::string& host,
                                                          std::uint16_t port,
                                                          const TransportOptions& options,
                                                          std::string* error) {
#if defined(_WIN32)
  if (!WinsockScope::started()) {
    if (error != nullptr) {
      *error = "Winsock could not be initialized";
    }
    return nullptr;
  }
  const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == INVALID_SOCKET) {
    if (error != nullptr) {
      *error = "connector socket could not be created";
    }
    return nullptr;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    closesocket(socket);
    if (error != nullptr) {
      *error = "connect host is not a valid IPv4 literal";
    }
    return nullptr;
  }
  if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
      SOCKET_ERROR) {
    const int code = WSAGetLastError();
    closesocket(socket);
    if (error != nullptr) {
      *error = "connect failed with code " + std::to_string(code);
    }
    return nullptr;
  }
  auto session = std::make_shared<FramedConnection::SessionState>();
  session->options = options;
  session->socket = socket;
  session->peer = describe_address(host, port);
  session->socket_event = WSACreateEvent();
  session->wake_event = WSACreateEvent();
  session->stop_event = WSACreateEvent();
  if (session->socket_event == WSA_INVALID_EVENT || session->wake_event == WSA_INVALID_EVENT ||
      session->stop_event == WSA_INVALID_EVENT) {
    if (error != nullptr) {
      *error = "session events could not be created";
    }
    return nullptr;
  }
  if (WSAEventSelect(socket, session->socket_event, FD_READ | FD_WRITE | FD_CLOSE) ==
      SOCKET_ERROR) {
    if (error != nullptr) {
      *error = "session could not register for socket events";
    }
    return nullptr;
  }
  session->worker = std::thread(session_loop_windows, session);
  return std::unique_ptr<FramedConnection>(new FramedConnection(std::move(session)));
#else
  const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket < 0) {
    if (error != nullptr) {
      *error = "connector socket could not be created";
    }
    return nullptr;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    ::close(socket);
    if (error != nullptr) {
      *error = "connect host is not a valid IPv4 literal";
    }
    return nullptr;
  }
  if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(socket);
    if (error != nullptr) {
      *error = "connect failed";
    }
    return nullptr;
  }
  auto session = std::make_shared<FramedConnection::SessionState>();
  session->options = options;
  session->socket = socket;
  session->peer = describe_address(host, port);
  if (::pipe(session->wake_pipe) != 0) {
    if (error != nullptr) {
      *error = "session wake pipe could not be created";
    }
    return nullptr;
  }
  session->worker = std::thread(session_loop_posix, session);
  return std::unique_ptr<FramedConnection>(new FramedConnection(std::move(session)));
#endif
}

}  // namespace model_router::distributed
