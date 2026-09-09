// Model Router - the routing runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_ROUTER_HPP
#define MODEL_ROUTER_ROUTER_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "model_router/authority.hpp"
#include "model_router/candidate.hpp"
#include "model_router/decision.hpp"
#include "model_router/persistence.hpp"
#include "model_router/policy.hpp"
#include "model_router/providers.hpp"
#include "model_router/request.hpp"

namespace model_router {

/// Construction options. Every dependency is injected: there is no global
/// mutable state and no hidden singleton.
struct ModelRouterOptions {
  /// 0 means "allocate a fresh RouterId".
  RouterId router_id{};
  RouterGeneration router_generation{};
  /// RouterGeneration to start at when no durable state is loaded.
  ResourceLimits limits{};
  RankingWeights weights{};
  RankingScales scales{};
  /// Clock used for every freshness decision. Defaults to the system clock.
  std::shared_ptr<Clock> clock{};
  ProviderSet providers{};
  /// When set, start() loads durable state from this path.
  std::string persistence_path{};
  bool load_on_start{false};
  /// When true, shutdown() persists state to persistence_path.
  bool save_on_shutdown{false};
  /// Enables the built-in catalog-backed discovery and evidence adapters. The
  /// library remains fully usable in-process without the reference transport.
  bool use_catalog_adapters{true};

  [[nodiscard]] std::string validate() const;
};

/// One backend as seen in a snapshot. Dynamic facts are reported with their
/// currentness so an inspector can never read stale data as live.
struct BackendSummary {
  BackendId backend_id{};
  BackendGeneration backend_generation{};
  BackendBootId backend_boot{};
  ProviderId provider_id{};
  EndpointId endpoint_id{};
  EndpointGeneration endpoint_generation{};
  TrustDomain trust_domain{TrustDomain::UNKNOWN};
  LocalityKey locality{};
  HealthState health{HealthState::UNKNOWN};
  AvailabilityState availability{AvailabilityState::UNKNOWN};
  ReadinessState readiness{ReadinessState::UNKNOWN};
  ResidencyState residency{ResidencyState::UNKNOWN};
  CapacityState capacity{CapacityState::UNKNOWN};
  std::uint32_t bound_model_count{0};
  bool fenced{false};
  bool evidence_current{false};
  /// True when every model binding carries known current price evidence.
  bool price_known{false};
  Provenance provenance{Provenance::SYNTHETIC};
};

/// One retained route decision as seen in a snapshot.
struct RouteSummary {
  RouteDecisionId decision_id{};
  RouteDecisionGeneration decision_generation{};
  RouteRequestId request_id{};
  CandidateKey winner{};
  RouteStatus status{RouteStatus::CURRENT};
  OutcomeCode code{OutcomeCode::ROUTED};
  Currentness currentness{Currentness::CURRENT};
  std::uint32_t fallback_count{0};
  UnixMillis created_at_unix_millis{0};
  std::string semantic_digest;
};

/// An immutable view of router state. A snapshot binds the authority under which
/// it was taken and carries a content-derived digest.
struct RouterSnapshot {
  RouterId router_id{};
  RouterGeneration router_generation{};
  RouterEpoch router_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  RouterBootId router_boot{};

  PolicyGeneration policy_generation{};
  PriceGeneration price_generation{};
  BudgetGeneration budget_generation{};
  SLOGeneration slo_generation{};
  CapabilityGeneration capability_generation{};

  std::uint32_t model_count{0};
  std::uint32_t backend_count{0};
  std::uint32_t fenced_boot_count{0};
  std::uint32_t route_decision_count{0};
  std::uint32_t tenant_count{0};

  std::vector<BackendSummary> backends;
  std::vector<RouteSummary> routes;

  Currentness currentness{Currentness::CURRENT};
  bool shutting_down{false};
  UnixMillis taken_at_unix_millis{0};
  std::string semantic_digest;

  /// A stale snapshot may be inspected but may never authorize dispatch.
  [[nodiscard]] bool authorizes_dispatch() const noexcept {
    return currentness == Currentness::CURRENT && !shutting_down;
  }
  [[nodiscard]] std::string to_json() const;
  [[nodiscard]] std::string to_text() const;
};

/// Aggregate counters and current generation set.
struct RouterSummary {
  RouterId router_id{};
  RouterGeneration router_generation{};
  RouterEpoch router_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  RouterBootId router_boot{};

  std::uint32_t model_count{0};
  std::uint32_t provider_count{0};
  std::uint32_t backend_count{0};
  std::uint32_t fenced_boot_count{0};
  std::uint32_t route_decision_count{0};
  std::uint32_t tenant_count{0};
  std::uint32_t eligible_last_request{0};
  std::uint32_t rejected_last_request{0};

  PersistenceCounters counters{};

  PolicyGeneration policy_generation{};
  PriceGeneration price_generation{};
  BudgetGeneration budget_generation{};
  SLOGeneration slo_generation{};
  CapabilityGeneration capability_generation{};

  bool running{false};
  bool shutting_down{false};
  bool recovered{false};
  UnixMillis started_at_unix_millis{0};
  std::string semantic_digest;

  [[nodiscard]] std::string to_text() const;
  [[nodiscard]] std::string to_json() const;
};

/// One violated invariant.
struct InvariantViolation {
  std::string name;
  std::string detail;

  friend bool operator==(const InvariantViolation&, const InvariantViolation&) = default;
};

/// Result of a full invariant scan over canonical state.
struct InvariantReport {
  std::vector<InvariantViolation> violations;
  std::uint32_t checks_run{0};

  [[nodiscard]] bool ok() const noexcept { return violations.empty(); }
  [[nodiscard]] std::string to_text() const;
  [[nodiscard]] std::string to_json() const;
};

/// The routing runtime.
///
/// The pipeline is: admission, requirement normalization, candidate discovery,
/// hard eligibility filtering, external policy/cost/SLO/availability validation,
/// deterministic factor construction, deterministic ranking, route-plan creation,
/// route authority binding, optional reservation binding, pre-dispatch
/// revalidation, dispatch handoff, completion observation, outcome recording, and
/// policy-bound retry/reroute.
///
/// Canonical locks are never held across network I/O, filesystem I/O, adapter
/// callbacks, or thread joins.
class ModelRouter {
 public:
  explicit ModelRouter(ModelRouterOptions options);
  ~ModelRouter();

  ModelRouter(const ModelRouter&) = delete;
  ModelRouter& operator=(const ModelRouter&) = delete;
  ModelRouter(ModelRouter&&) = delete;
  ModelRouter& operator=(ModelRouter&&) = delete;

  // --- lifecycle ---------------------------------------------------------

  /// Publishes the router identity and advances the router and coordinator
  /// epochs. Loading durable state, when configured, happens here.
  [[nodiscard]] MutationResult start();
  /// Stops accepting new route requests and prevents new executable route
  /// authority. Idempotent; repeated start/stop is safe.
  [[nodiscard]] MutationResult shutdown();
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] bool shutting_down() const noexcept;

  // --- canonical mutation ------------------------------------------------

  [[nodiscard]] MutationResult register_model(ModelDescriptor model);
  [[nodiscard]] MutationResult register_provider(ProviderDescriptor provider);
  [[nodiscard]] MutationResult register_backend(BackendDescriptor backend);
  [[nodiscard]] MutationResult unregister_backend(BackendId backend_id,
                                                  BackendGeneration generation);
  [[nodiscard]] MutationResult publish_capabilities(BackendCapabilityPublication publication);
  [[nodiscard]] MutationResult retire_model(ModelId model_id, ModelGeneration generation);
  [[nodiscard]] MutationResult fence_backend_boot(BackendId backend_id, BackendGeneration generation,
                                                  BackendBootId backend_boot, OutcomeCode reason);

  [[nodiscard]] MutationResult update_health(HealthEvidence evidence);
  [[nodiscard]] MutationResult update_availability(AvailabilityEvidence evidence);
  [[nodiscard]] MutationResult update_readiness(ReadinessEvidence evidence);
  [[nodiscard]] MutationResult update_residency(ResidencyEvidence evidence);
  [[nodiscard]] MutationResult update_capacity(CapacityEvidence evidence, CapacityDetail detail);
  [[nodiscard]] MutationResult update_latency(LatencyEvidence evidence);

  [[nodiscard]] MutationResult set_policy(PolicySnapshot policy);
  [[nodiscard]] MutationResult set_budget(BudgetSnapshot budget);
  [[nodiscard]] MutationResult set_slo(SloEvidence slo);
  [[nodiscard]] MutationResult set_compatibility(CompatibilityEvidence evidence);
  [[nodiscard]] MutationResult set_trust(TrustEvidence evidence);
  [[nodiscard]] MutationResult set_cost(CostEvidence evidence);

  // --- routing -----------------------------------------------------------

  /// Runs the complete routing pipeline for p request.
  [[nodiscard]] RouteOutcome route(RouteRequest request);

  /// Revalidates a previously produced decision against current authority.
  /// When \p caller_tenant or \p caller_namespace is valid it must match the
  /// decision's bound tenancy, which prevents cross-tenant route reuse.
  [[nodiscard]] MutationResult revalidate(RouteDecisionId decision_id,
                                          TenantId caller_tenant = TenantId{},
                                          NamespaceId caller_namespace = NamespaceId{});

  /// Performs pre-dispatch revalidation and hands the route to the dispatcher.
  /// A stale, fenced, policy-denied, budget-denied, or retired route is never
  /// dispatched.
  [[nodiscard]] DispatchRecord dispatch(RouteDecisionId decision_id,
                                        TenantId caller_tenant = TenantId{},
                                        NamespaceId caller_namespace = NamespaceId{});

  /// Records an observed completion. Model Router does not own the request
  /// semantics; it records what the caller observed.
  [[nodiscard]] MutationResult record_completion(const CompletionRecord& completion);

  /// Attempts a policy-bound reroute or fallback after p failure.
  [[nodiscard]] RouteOutcome reroute(RouteDecisionId decision_id, FailureClass failure,
                                     RouteRequest request);

  // --- inspection --------------------------------------------------------

  [[nodiscard]] RouterSnapshot snapshot() const;
  [[nodiscard]] RouterSummary summary() const;
  [[nodiscard]] InvariantReport check_invariants() const;

  [[nodiscard]] bool find_model(ModelId model_id, ModelDescriptor* out) const;
  [[nodiscard]] bool find_backend(BackendId backend_id, BackendDescriptor* out) const;
  [[nodiscard]] bool find_decision(RouteDecisionId decision_id, RouteDecision* out) const;
  [[nodiscard]] std::vector<RouteDecision> route_history() const;
  [[nodiscard]] std::vector<FencedBootRecord> fenced_boots() const;

  /// Canonical, deterministic explanation of a retained decision.
  [[nodiscard]] std::string explain_route(RouteDecisionId decision_id) const;

  [[nodiscard]] RouterId router_id() const noexcept;
  [[nodiscard]] RouterEpoch router_epoch() const noexcept;
  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept;
  [[nodiscard]] RouterBootId router_boot() const noexcept;

  // --- persistence -------------------------------------------------------

  [[nodiscard]] PersistenceResult save(const std::string& path);
  [[nodiscard]] PersistenceResult load(const std::string& path);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  friend class Impl;
};

}  // namespace model_router

#endif  // MODEL_ROUTER_ROUTER_HPP
