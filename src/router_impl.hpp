// Model Router - internal router state and implementation. Internal.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_SRC_ROUTER_IMPL_HPP
#define MODEL_ROUTER_SRC_ROUTER_IMPL_HPP

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "model_router/router.hpp"
#include "ranking.hpp"

namespace model_router::detail {

/// A retained route record. History is inspectable and is never dispatchable
/// merely because it was once valid.
struct RouteRecord {
  RouteDecision decision{};
  RoutePlan plan{};
  TenantId tenant{};
  NamespaceId name_space{};
};

/// The minimal projection of a retained route that revalidation needs. Copying
/// this instead of the whole record keeps revalidation cost independent of the
/// retained explanation size.
struct RouteLookup {
  RouteDecisionId decision_id{};
  RouteDecisionGeneration decision_generation{};
  RouteRequestId request_id{};
  RouteRequestGeneration request_generation{};
  RouteStatus status{RouteStatus::CURRENT};
  OutcomeCode code{OutcomeCode::ROUTED};
  RouteAuthority authority{};
  TenantId tenant{};
  NamespaceId name_space{};

  [[nodiscard]] static RouteLookup from(const RouteRecord& record);
};

/// Fenced backend incarnation key.
struct FenceKey {
  BackendId backend_id{};
  BackendBootId backend_boot{};

  friend bool operator==(const FenceKey&, const FenceKey&) = default;
  friend bool operator<(const FenceKey& lhs, const FenceKey& rhs) noexcept {
    if (!(lhs.backend_id == rhs.backend_id)) {
      return lhs.backend_id < rhs.backend_id;
    }
    return lhs.backend_boot < rhs.backend_boot;
  }
};

/// Canonical authoritative state. Every container is ordered so that iteration
/// is deterministic and independent of insertion order. No unordered container
/// drives routing semantics.
struct CanonicalState {
  RouterId router_id{};
  RouterGeneration router_generation{};
  RouterEpoch router_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  RouterBootId router_boot{};

  bool running{false};
  bool shutting_down{false};
  bool recovered{false};

  ResourceLimits limits{};
  RankingWeights weights{};
  RankingScales scales{};

  std::map<ModelId, ModelDescriptor> models;
  std::map<ProviderId, ProviderDescriptor> providers;
  std::map<BackendId, BackendDescriptor> backends;

  /// Durable fenced incarnations. A fenced boot may never become authoritative
  /// again, in any process, at any epoch. The record keeps the generation and
  /// fence time so recovery and inspection stay truthful.
  std::map<FenceKey, FencedBootRecord> fenced_boots;

  /// Monotonic generation watermarks.
  std::map<ModelId, ModelGeneration> model_generation_watermark;
  std::map<BackendId, BackendGeneration> backend_generation_watermark;

  PolicySnapshot policy{};
  BudgetSnapshot budget{};
  SloEvidence slo{};
  std::map<BackendId, CompatibilityEvidence> compatibility;
  std::map<BackendId, TrustEvidence> trust;
  std::map<std::pair<BackendId, ModelId>, CostEvidence> costs;
  std::map<BackendId, ReservationEvidence> reservations;

  std::map<RouteDecisionId, RouteRecord> routes;
  std::deque<RouteDecisionId> route_order;
  std::map<RouteDecisionId, DispatchRecord> dispatches;
  std::map<DispatchId, RouteDecisionId> dispatch_index;

  /// Derived indexes. Continuously compared against canonical scans; an index is
  /// never a second source of truth.
  std::map<ModelId, std::set<BackendId>> index_backends_by_model;
  std::map<ProviderId, std::set<BackendId>> index_backends_by_provider;
  std::map<std::uint8_t, std::set<BackendId>> index_backends_by_trust;
  std::set<TenantId> tenants;

  /// Continuity: the backend that most recently won a route for a tenant.
  std::map<TenantId, BackendId> last_winner_by_tenant;

  PersistenceCounters counters{};
  std::uint32_t last_eligible{0};
  std::uint32_t last_rejected{0};

  UnixMillis started_at_unix_millis{0};

  void rebuild_indexes();
  /// Incremental index maintenance. Indexes are derived data and are updated in
  /// the same critical section as the canonical mutation that changed them.
  void index_add_backend(const BackendDescriptor& backend);
  void index_remove_backend(const BackendDescriptor& backend);
  void note_tenant(TenantId tenant);
  /// Marks every retained decision bound to a retired incarnation as STALE.
  void stale_decisions_for_backend(BackendId backend_id, BackendBootId backend_boot);
  /// Marks every retained executable decision whose bound authority no longer
  /// matches current state as STALE. Called after any authority-changing
  /// mutation so that a CURRENT decision is always dispatchable.
  void stale_decisions_for_authority_change(UnixMillis now);
  /// Re-checks one retained decision against current state and marks it STALE
  /// when it is no longer dispatchable. Used at commit time so a mutation that
  /// landed during a routing pass cannot leave a freshly inserted decision
  /// current, without making the commit path scan the whole history.
  void stale_decision_if_not_dispatchable(RouteRecord& record, UnixMillis now);
};

/// Reads the authority currently observable for one candidate identity.
[[nodiscard]] bool build_current_authority(const CanonicalState& state,
                                           const RouteAuthority& bound, RouteAuthority* out);

/// True when a bound route authority is dispatchable right now: every required
/// generation matches, the decision has not expired, and the bound model and
/// backend incarnation are present, current, and not retired, draining, fenced,
/// or replaced. This is the single definition of "still executable".
[[nodiscard]] bool is_authority_dispatchable(const CanonicalState& state,
                                             const RouteAuthority& bound, UnixMillis now);

/// The typed reason a bound route authority is not dispatchable. ACCEPTED means
/// it is dispatchable. Differences are reported as the most specific stale code.
[[nodiscard]] OutcomeCode dispatchability_reason(const CanonicalState& state,
                                                 const RouteAuthority& bound, UnixMillis now);

}  // namespace model_router::detail

namespace model_router {

class ModelRouter::Impl {
 public:
  explicit Impl(ModelRouterOptions options);
  ~Impl();

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  ModelRouterOptions options;
  std::string options_error;
  std::shared_ptr<Clock> clock;
  mutable std::shared_mutex mutex;
  detail::CanonicalState state;

  // --- helpers (all take the lock themselves) -----------------------------
  [[nodiscard]] UnixMillis now() const;
  [[nodiscard]] DiscoveryContext make_context() const;
  [[nodiscard]] MutationResult reject(OutcomeCode code, const char* subject,
                                      const std::string& message) const;
  /// Caller must hold the canonical lock.
  [[nodiscard]] bool is_fenced(BackendId backend_id, BackendBootId boot) const;

  // --- lifecycle ----------------------------------------------------------
  [[nodiscard]] MutationResult start();
  [[nodiscard]] MutationResult shutdown();

  // --- catalog ------------------------------------------------------------
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

  // --- routing ------------------------------------------------------------
  [[nodiscard]] RouteOutcome route(RouteRequest request);
  [[nodiscard]] MutationResult revalidate(RouteDecisionId decision_id, TenantId caller_tenant,
                                          NamespaceId caller_namespace);
  [[nodiscard]] DispatchRecord dispatch(RouteDecisionId decision_id, TenantId caller_tenant,
                                        NamespaceId caller_namespace);
  [[nodiscard]] MutationResult record_completion(const CompletionRecord& completion);
  [[nodiscard]] RouteOutcome reroute(RouteDecisionId decision_id, FailureClass failure,
                                     RouteRequest request);

  // --- inspection ---------------------------------------------------------
  [[nodiscard]] RouterSnapshot snapshot() const;
  [[nodiscard]] RouterSummary summary() const;
  [[nodiscard]] InvariantReport check_invariants() const;
  [[nodiscard]] std::string explain_route(RouteDecisionId decision_id) const;

  // --- persistence --------------------------------------------------------
  [[nodiscard]] PersistenceResult save(const std::string& path);
  [[nodiscard]] PersistenceResult load(const std::string& path);
  [[nodiscard]] RouterPersistentState export_persistent_state() const;
  [[nodiscard]] MutationResult import_persistent_state(const RouterPersistentState& persisted);

  /// One consistent capture of the evidence maps a routing pass needs. Taking
  /// it once per pass removes the per-candidate lock acquisition that otherwise
  /// dominates large candidate sets.
  struct EvidenceSnapshot {
    std::map<BackendId, CompatibilityEvidence> compatibility;
    std::map<BackendId, TrustEvidence> trust;
    std::map<BackendId, ReservationEvidence> reservations;
    SloEvidence slo{};

    [[nodiscard]] static EvidenceSnapshot capture(const detail::CanonicalState& state);
  };

  // --- pipeline internals (called with no canonical lock held) ------------

  /// Reads the authority currently observable for one candidate identity.
  [[nodiscard]] static bool current_authority(const Impl& impl, const RouteAuthority& bound,
                                              RouteAuthority* out);
  /// Builds a decision for exactly one freshly validated fallback candidate.
  [[nodiscard]] static RouteOutcome commit_single_candidate(
      Impl& impl, const RouteRequest& request, const DiscoveryContext& context,
      RouteCandidate candidate, detail::CandidateEvidence evidence, const PolicySnapshot& policy,
      const BudgetSnapshot& budget, const SloEvidence& slo, const RankingWeights& weights,
      const RankingScales& scales, RouterEpoch epoch, CoordinatorEpoch coordinator_epoch,
      const std::vector<CandidateKey>& fallbacks, std::uint32_t permitted_fallbacks,
      BackendId continuity, bool has_continuity);

  [[nodiscard]] std::vector<RouteCandidate> discover(const RouteRequest& request,
                                                     const DiscoveryContext& context);
  void resolve_candidate(const RouteRequest& request, const DiscoveryContext& context,
                         const EvidenceSnapshot& snapshot, RouteCandidate* candidate,
                         detail::CandidateEvidence* evidence) const;
  [[nodiscard]] bool build_candidate_from_catalog(const RouteRequest& request, ModelId model_id,
                                                  BackendId backend_id,
                                                  RouteCandidate* out) const;

  // --- retained-history access -------------------------------------------
  [[nodiscard]] bool find_model(ModelId model_id, ModelDescriptor* out) const;
  [[nodiscard]] bool find_backend(BackendId backend_id, BackendDescriptor* out) const;
  [[nodiscard]] bool find_decision(RouteDecisionId decision_id, RouteDecision* out) const;
  [[nodiscard]] std::vector<RouteDecision> route_history() const;
  [[nodiscard]] std::vector<FencedBootRecord> fenced_boots() const;
};

}  // namespace model_router

#endif  // MODEL_ROUTER_SRC_ROUTER_IMPL_HPP
