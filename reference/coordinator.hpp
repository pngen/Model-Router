// Model Router - reference coordinator. Reference architecture only.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// The coordinator hosts one ModelRouter and accepts framed TCP connections from
// independent backend worker processes. It owns the routing decision; it never
// performs model inference.

#ifndef MODEL_ROUTER_REFERENCE_COORDINATOR_HPP
#define MODEL_ROUTER_REFERENCE_COORDINATOR_HPP

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model_router/distributed/transport.hpp"
#include "model_router/model_router.hpp"
#include "model_router/reference_profiles.hpp"

namespace model_router::reference {

struct CoordinatorOptions {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{0};
  ResourceLimits limits{};
  std::string state_path{};
  bool load_on_start{false};
  bool save_on_shutdown{false};
  RouterGeneration router_generation{1};
};

/// Per-backend session link. A backend worker may own several models; one
/// process incarnation owns exactly one link.
struct BackendLink {
  BackendId backend_id{};
  BackendGeneration backend_generation{};
  BackendBootId backend_boot{};
  std::shared_ptr<distributed::FramedConnection> connection;

  std::mutex mutex;
  std::condition_variable condition;
  std::map<DispatchId, distributed::DispatchResultMessage> results;
  bool closed{false};
};

/// The reference coordinator process.
class Coordinator {
 public:
  explicit Coordinator(CoordinatorOptions options);
  ~Coordinator();

  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  [[nodiscard]] bool start(std::string* error);
  void stop();

  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] std::string last_error() const;

  [[nodiscard]] ModelRouter& router() noexcept { return *router_; }
  [[nodiscard]] std::uint64_t sessions_served() const noexcept;
  [[nodiscard]] std::uint64_t frames_handled() const noexcept;
  [[nodiscard]] std::uint32_t live_links() const;

 private:
  void accept_loop();
  void serve(std::shared_ptr<distributed::FramedConnection> connection);
  [[nodiscard]] bool handle_frame(const std::shared_ptr<distributed::FramedConnection>& connection,
                                  const distributed::Frame& frame, bool* keep_going);
  [[nodiscard]] distributed::AdminResponse handle_admin(const distributed::AdminRequest& request);

  CoordinatorOptions options_;
  reference::Catalog catalog_;
  distributed::FrameListener listener_;
  std::unique_ptr<ModelRouter> router_;
  std::thread accept_thread_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex mutex_;
  std::string last_error_;
  std::vector<std::thread> sessions_;
  std::map<BackendId, std::shared_ptr<BackendLink>> links_;
  std::map<RouteDecisionId, RouteRequest> requests_;
  std::map<BackendId, std::uint64_t> evidence_generations_;
  std::atomic<std::uint64_t> sessions_served_{0};
  std::atomic<std::uint64_t> frames_handled_{0};

  /// Dispatch handler that forwards a revalidated route to the owning backend
  /// worker over its real connection and waits for the observed result.
  class Dispatcher final : public DispatchHandler {
   public:
    explicit Dispatcher(Coordinator* coordinator) : coordinator_(coordinator) {}
    [[nodiscard]] std::string name() const override { return "reference-coordinator-dispatcher"; }
    [[nodiscard]] DispatchRecord handoff(const RouteDecision& decision, const RoutePlan& plan,
                                         const DiscoveryContext& context) override;

   private:
    Coordinator* coordinator_;
  };
};

}  // namespace model_router::reference

#endif  // MODEL_ROUTER_REFERENCE_COORDINATOR_HPP
