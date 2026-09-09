// Model Router - declarative policy evaluation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_POLICY_HPP
#define MODEL_ROUTER_POLICY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "model_router/candidate.hpp"
#include "model_router/evidence.hpp"
#include "model_router/request.hpp"

namespace model_router {

/// Result of evaluating policy for one candidate or one request. The policy
/// engine is typed and declarative: it never evaluates expressions or executes
/// caller-supplied code.
struct PolicyVerdict {
  OutcomeCode code{OutcomeCode::ACCEPTED};
  std::string detail;
  /// Preference contribution in [0, kFactorScale]; 0 when policy expresses no
  /// preference. Preference never overrides a hard policy decision.
  std::int64_t preference{0};

  static constexpr std::int64_t kPreferenceScale = FactorValue::kFactorScale;

  [[nodiscard]] bool allowed() const noexcept { return code == OutcomeCode::ACCEPTED; }
  friend bool operator==(const PolicyVerdict&, const PolicyVerdict&) = default;
};

/// Deterministic, side-effect-free policy evaluation.
class PolicyEvaluator {
 public:
  /// Evaluates tenant/namespace binding and request-level policy constraints.
  [[nodiscard]] static PolicyVerdict evaluate_request(const PolicySnapshot& policy,
                                                      const RouteRequest& request,
                                                      UnixMillis now) noexcept;

  /// Evaluates one candidate against policy. Hard denials return a rejection
  /// code; otherwise ACCEPTED with an optional preference contribution.
  [[nodiscard]] static PolicyVerdict evaluate_candidate(const PolicySnapshot& policy,
                                                        const RouteRequest& request,
                                                        const RouteCandidate& candidate,
                                                        UnixMillis now) noexcept;

  /// Decides whether an ordered fallback may be attempted after p trigger.
  [[nodiscard]] static bool fallback_permitted(const PolicySnapshot& policy,
                                               const RouteRequirements& requirements,
                                               OutcomeCode trigger) noexcept;
};

/// Convenience constructor for a coherent policy snapshot.
class PolicyBuilder {
 public:
  explicit PolicyBuilder(PolicyId policy_id, PolicyGeneration generation);

  PolicyBuilder& deny_model(ModelId model_id);
  PolicyBuilder& allow_model(ModelId model_id);
  PolicyBuilder& deny_provider(ProviderId provider_id);
  PolicyBuilder& allow_provider(ProviderId provider_id);
  PolicyBuilder& deny_backend(BackendId backend_id);
  PolicyBuilder& allow_backend(BackendId backend_id);
  PolicyBuilder& deny_family(ModelFamilyId family_id);
  PolicyBuilder& prefer_family(ModelFamilyId family_id);
  PolicyBuilder& allow_locality(LocalityKey locality);
  PolicyBuilder& deny_locality(LocalityKey locality);
  PolicyBuilder& bind_tenant(TenantId tenant);
  PolicyBuilder& minimum_trust(TrustDomain domain);
  PolicyBuilder& minimum_capability_evidence(CapabilityState state);
  PolicyBuilder& quality_floor(QualityClass quality);
  PolicyBuilder& maximum_request_cost(Money cost);
  PolicyBuilder& maximum_latency(std::uint32_t micros);
  PolicyBuilder& local_only(bool value);
  PolicyBuilder& deny_public_remote(bool value);
  PolicyBuilder& fallback_policy(FallbackPolicy value);
  PolicyBuilder& permit_fallback_outcome(OutcomeCode code);
  PolicyBuilder& max_fallback_depth(std::uint32_t depth);
  PolicyBuilder& max_provider_concentration_ppm(std::uint32_t ppm);
  PolicyBuilder& allow_unlisted_models(bool value);
  PolicyBuilder& allow_unlisted_providers(bool value);
  PolicyBuilder& allow_unlisted_backends(bool value);
  PolicyBuilder& expires_at(UnixMillis unix_millis);

  [[nodiscard]] PolicySnapshot build() const;

 private:
  PolicySnapshot policy_;
};

}  // namespace model_router

#endif  // MODEL_ROUTER_POLICY_HPP
