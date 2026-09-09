// Model Router - narrow replaceable adapters for external evidence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_PROVIDERS_HPP
#define MODEL_ROUTER_PROVIDERS_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "model_router/candidate.hpp"
#include "model_router/evidence.hpp"
#include "model_router/limits.hpp"
#include "model_router/request.hpp"

namespace model_router {

/// Result of asking an adapter for evidence. A missing or stale answer is an
/// explicit typed outcome, never an empty value that reads as "fine".
template <class T>
struct EvidenceResult {
  OutcomeCode code{OutcomeCode::INTERNAL_ERROR};
  T value{};
  bool has_value{false};
  std::string detail;

  [[nodiscard]] bool ok() const noexcept {
    return has_value && (code == OutcomeCode::ACCEPTED || code == OutcomeCode::NO_CHANGE);
  }
  [[nodiscard]] static EvidenceResult accepted(T value_in) {
    EvidenceResult result;
    result.code = OutcomeCode::ACCEPTED;
    result.value = std::move(value_in);
    result.has_value = true;
    return result;
  }
  [[nodiscard]] static EvidenceResult unavailable(OutcomeCode code_in, std::string detail_in) {
    EvidenceResult result;
    result.code = code_in;
    result.has_value = false;
    result.detail = std::move(detail_in);
    return result;
  }
};

/// Context handed to every adapter call.
struct DiscoveryContext {
  RouterId router_id{};
  RouterEpoch router_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  TenantId tenant{};
  NamespaceId name_space{};
  UnixMillis now_unix_millis{0};
  const ResourceLimits* limits{nullptr};

  friend bool operator==(const DiscoveryContext&, const DiscoveryContext&) = default;
};

/// Candidate discovery. Discovery is deliberately separate from ranking: a
/// provider may drop obviously impossible identities, but it must not silently
/// pre-rank candidates or hide alternatives. The returned set is a semantic set
/// whose ordering is irrelevant; Model Router canonicalizes it.
class CandidateProvider {
 public:
  virtual ~CandidateProvider() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual EvidenceResult<std::vector<RouteCandidate>> discover(
      const RouteRequest& request, const DiscoveryContext& context) = 0;
};

/// Policy evidence boundary. The policy is generation-bound; a provider that
/// cannot prove the requested generation must say so rather than return a
/// different generation's policy.
class PolicyProvider {
 public:
  virtual ~PolicyProvider() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual EvidenceResult<PolicySnapshot> fetch(PolicyId policy_id,
                                                             PolicyGeneration generation,
                                                             const DiscoveryContext& context) = 0;
};

/// Budget feasibility boundary.
class BudgetProvider {
 public:
  virtual ~BudgetProvider() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual EvidenceResult<BudgetSnapshot> fetch(BudgetId budget_id,
                                                             BudgetGeneration generation,
                                                             const RouteRequest& request,
                                                             const DiscoveryContext& context) = 0;
};

/// Cost evidence boundary. Cost is a routing factor and/or a hard constraint;
/// Model Router never becomes the accounting engine.
class CostProvider {
 public:
  virtual ~CostProvider() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  /// Returns the price evidence for one candidate. UNKNOWN price must be
  /// reported as UNKNOWN, never as zero.
  [[nodiscard]] virtual EvidenceResult<CostEvidence> fetch(const RouteCandidate& candidate,
                                                           const RouteRequest& request,
                                                           const DiscoveryContext& context) = 0;
};

/// SLO evidence boundary.
class SloProvider {
 public:
  virtual ~SloProvider() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual EvidenceResult<SloEvidence> fetch(SLOId slo_id, SLOGeneration generation,
                                                          const RouteRequest& request,
                                                          const DiscoveryContext& context) = 0;
  /// Per-candidate feasibility under the request SLO. Returning
  /// UNKNOWN is legal and is treated as "not proven feasible".
  [[nodiscard]] virtual EvidenceResult<SloVerdict> feasibility(const RouteCandidate& candidate,
                                                               const SloEvidence& slo,
                                                               const RouteRequest& request,
                                                               const DiscoveryContext& context) = 0;
};

/// Compatibility evidence boundary.
class CompatibilityProvider {
 public:
  virtual ~CompatibilityProvider() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual EvidenceResult<CompatibilityEvidence> fetch(
      const RouteCandidate& candidate, const RouteRequest& request,
      const DiscoveryContext& context) = 0;
};

/// Trust evidence boundary. Trust is never inferred from a hostname.
class TrustProvider {
 public:
  virtual ~TrustProvider() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual EvidenceResult<TrustEvidence> fetch(const RouteCandidate& candidate,
                                                            const DiscoveryContext& context) = 0;
};

/// Optional reservation boundary. A route that requires a reservation is not
/// dispatchable without a current lease.
class ReservationProvider {
 public:
  virtual ~ReservationProvider() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual EvidenceResult<ReservationEvidence> reserve(
      const RouteCandidate& candidate, const RouteRequest& request,
      const DiscoveryContext& context) = 0;
};

/// Optional dispatch boundary. Model Router owns the route decision and the
/// route authority; a dispatcher performs the handoff and reports the outcome.
class DispatchHandler {
 public:
  virtual ~DispatchHandler() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  /// Performs the handoff. The caller has already revalidated authority.
  [[nodiscard]] virtual DispatchRecord handoff(const RouteDecision& decision,
                                               const RoutePlan& plan,
                                               const DiscoveryContext& context) = 0;
};

/// The set of adapters a router instance uses. Any adapter left null is treated
/// as "evidence unavailable", which makes dependent hard requirements fail.
struct ProviderSet {
  std::shared_ptr<CandidateProvider> candidates;
  std::shared_ptr<PolicyProvider> policy;
  std::shared_ptr<BudgetProvider> budget;
  std::shared_ptr<CostProvider> cost;
  std::shared_ptr<SloProvider> slo;
  std::shared_ptr<CompatibilityProvider> compatibility;
  std::shared_ptr<TrustProvider> trust;
  std::shared_ptr<ReservationProvider> reservation;
  std::shared_ptr<DispatchHandler> dispatcher;
};

}  // namespace model_router

#endif  // MODEL_ROUTER_PROVIDERS_HPP
