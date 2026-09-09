// Model Router - deterministic factor construction, ranking, and tie-breaks.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "ranking.hpp"
#include "router_fixture.hpp"

using model_router::AvailabilityState;
using model_router::BackendBootId;
using model_router::BackendDescriptor;
using model_router::BackendGeneration;
using model_router::BackendId;
using model_router::BackendRegistrationGeneration;
using model_router::CandidateKey;
using model_router::CapabilityKey;
using model_router::CapabilityRequirement;
using model_router::CapabilityState;
using model_router::CapacityState;
using model_router::CostEvidence;
using model_router::CostEvidenceId;
using model_router::CostKnownness;
using model_router::EndpointId;
using model_router::FactorValue;
using model_router::HealthState;
using model_router::ModelFamilyId;
using model_router::ModelGeneration;
using model_router::ModelId;
using model_router::ModelLifecycle;
using model_router::Modality;
using model_router::ModalitySet;
using model_router::NamespaceId;
using model_router::OutcomeCode;
using model_router::PolicyGeneration;
using model_router::PolicyId;
using model_router::PolicyVerdict;
using model_router::PriceGeneration;
using model_router::Provenance;
using model_router::ProviderGeneration;
using model_router::ProviderId;
using model_router::RankedCandidate;
using model_router::RankingFactor;
using model_router::RankingScales;
using model_router::RankingWeights;
using model_router::ReadinessState;
using model_router::ResidencyState;
using model_router::RouteOutcome;
using model_router::RouteRejection;
using model_router::RouteRequest;
using model_router::RouteRequestGeneration;
using model_router::RouteRequestId;
using model_router::RouterId;
using model_router::TenantId;
using model_router::TrustDomain;

namespace {

constexpr TenantId kTenant(1);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);

[[nodiscard]] CapabilityRequirement require_code() {
  CapabilityRequirement requirement;
  requirement.key = CapabilityKey(std::string(model_router::capability_keys::code));
  requirement.minimum_state = CapabilityState::DECLARED;
  return requirement;
}

/// A router built from explicit options so several routers can share one
/// canonical identity and therefore be compared byte for byte.
struct RankRig {
  std::shared_ptr<model_router::LogicalClock> clock = std::make_shared<model_router::LogicalClock>();
  std::shared_ptr<mrtest::RecordingDispatcher> dispatcher =
      std::make_shared<mrtest::RecordingDispatcher>();
  model_router::reference::Catalog catalog = model_router::reference::make_catalog();
  mrtest::EvidencePublisher publisher;
  std::unique_ptr<model_router::ModelRouter> router;

  [[nodiscard]] model_router::UnixMillis now() const { return clock->now_unix_millis(); }

  void start(RouterId router_id, const RankingWeights& weights = RankingWeights{}) {
    model_router::ModelRouterOptions options;
    options.clock = clock;
    options.providers.dispatcher = dispatcher;
    options.router_id = router_id;
    options.weights = weights;
    router = std::make_unique<model_router::ModelRouter>(std::move(options));
    MR_CHECK(router->start().accepted());
    for (const model_router::ModelDescriptor& model :
         model_router::reference::make_models(catalog, now())) {
      MR_CHECK(router->register_model(model).accepted());
    }
    model_router::ProviderDescriptor provider;
    provider.provider_id = catalog.provider;
    provider.generation = ProviderGeneration(1);
    provider.display_name = "reference-provider";
    provider.default_trust_domain = TrustDomain::LOCAL;
    provider.third_party = false;
    MR_CHECK(router->register_provider(std::move(provider)).accepted());
    MR_CHECK(mrtest::install_open_policy(*router, kPolicy, kPolicyGeneration).accepted());
  }

  void add_backend(BackendId backend_id, std::uint64_t boot,
                   std::uint32_t dispatch_micros = 1000) {
    BackendDescriptor backend = model_router::reference::make_backend(
        catalog, backend_id, BackendGeneration(1), BackendBootId(boot),
        BackendRegistrationGeneration(1),
        "127.0.0.1:" + std::to_string(7000 + backend_id.value()), Provenance::SYNTHETIC, now());
    MR_CHECK(router->register_backend(std::move(backend)).accepted());
    const BackendBootId boot_id(boot);
    const model_router::UnixMillis current = now();
    MR_CHECK(publisher.health(*router, backend_id, boot_id, HealthState::HEALTHY, current)
                 .accepted());
    MR_CHECK(publisher
                 .availability(*router, backend_id, boot_id, AvailabilityState::AVAILABLE, current)
                 .accepted());
    MR_CHECK(publisher
                 .readiness(*router, backend_id, boot_id, ReadinessState::READY, current)
                 .accepted());
    MR_CHECK(publisher
                 .residency(*router, backend_id, boot_id, ResidencyState::RESIDENT, current)
                 .accepted());
    MR_CHECK(publisher
                 .capacity(*router, backend_id, boot_id, CapacityState::AVAILABLE, 8, 16, current)
                 .accepted());
    MR_CHECK(publisher.latency(*router, backend_id, boot_id, dispatch_micros, 100, current)
                 .accepted());
  }

  void publish_costs(BackendId backend_id, std::int64_t total) {
    const model_router::UnixMillis current = now();
    const ModelId models[] = {catalog.small_model, catalog.general_model, catalog.specialist_model};
    for (const ModelId model_id : models) {
      MR_CHECK(router
                   ->set_cost(model_router::reference::make_cost_evidence(
                       backend_id, model_id, PriceGeneration(1), total, total, total, current))
                   .accepted());
    }
  }
};

/// One identical request, so several routers can be compared exactly.
[[nodiscard]] RouteRequest make_request(const RankRig& rig) {
  RouteRequest request;
  request.request_id = RouteRequestId(1);
  request.request_generation = RouteRequestGeneration(1);
  request.tenant = kTenant;
  request.name_space = NamespaceId(1);
  request.policy_id = kPolicy;
  request.policy_generation = kPolicyGeneration;
  request.created_at_unix_millis = rig.now();
  request.estimated_input_tokens = 1000;
  request.estimated_output_tokens = 500;
  request.requirements.required_input_modalities =
      ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  request.requirements.required_output_modalities =
      ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  return request;
}

/// Direct factor-construction input. The baseline candidate has no evidence at
/// all, so every factor is UNKNOWN and contributes exactly zero.
struct FactorCase {
  RouteRequest request{};
  model_router::RouteCandidate candidate{};
  PolicyVerdict policy_verdict{};
  bool has_policy_verdict{false};
  RankingWeights weights{};
  RankingScales scales{};
  model_router::detail::CandidateEvidence evidence{};
  model_router::UnixMillis now{1000};
  BackendId continuity{};
  bool has_continuity{false};
  ProviderId diversity{};
  bool has_diversity{false};
};

[[nodiscard]] FactorCase make_factor_case() {
  FactorCase factor;
  factor.request.request_id = RouteRequestId(1);
  factor.request.request_generation = RouteRequestGeneration(1);
  factor.request.tenant = kTenant;
  factor.request.name_space = NamespaceId(1);
  factor.request.policy_id = kPolicy;
  factor.request.policy_generation = kPolicyGeneration;
  factor.request.created_at_unix_millis = factor.now;
  factor.request.estimated_input_tokens = 100;
  factor.request.estimated_output_tokens = 100;

  factor.candidate.key.model_id = ModelId(1);
  factor.candidate.key.model_generation = ModelGeneration(1);
  factor.candidate.key.artifact_generation = model_router::ArtifactGeneration(1);
  factor.candidate.key.backend_id = BackendId(1);
  factor.candidate.key.backend_generation = BackendGeneration(1);
  factor.candidate.key.backend_boot = BackendBootId(1);
  factor.candidate.key.endpoint_id = EndpointId(1);
  factor.candidate.key.endpoint_generation = model_router::EndpointGeneration(1);

  factor.candidate.model.model_id = ModelId(1);
  factor.candidate.model.model_generation = ModelGeneration(1);
  factor.candidate.model.artifact_generation = model_router::ArtifactGeneration(1);
  factor.candidate.model.family_id = ModelFamilyId(1);
  factor.candidate.model.context_limit_tokens = 100000;
  factor.candidate.model.max_output_tokens = 10000;
  factor.candidate.model.input_modalities =
      ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  factor.candidate.model.output_modalities =
      ModalitySet(static_cast<std::uint32_t>(Modality::TEXT));
  factor.candidate.model.quality_class = model_router::kUnclassifiedQuality;
  factor.candidate.model.lifecycle = ModelLifecycle::CURRENT;

  factor.candidate.backend.backend_id = BackendId(1);
  factor.candidate.backend.backend_generation = BackendGeneration(1);
  factor.candidate.backend.backend_boot = BackendBootId(1);
  factor.candidate.backend.backend_registration_generation = BackendRegistrationGeneration(1);
  factor.candidate.backend.provider_id = ProviderId(1);
  factor.candidate.backend.provider_generation = ProviderGeneration(1);
  factor.candidate.backend.endpoint.endpoint_id = EndpointId(1);
  factor.candidate.backend.endpoint.endpoint_generation = model_router::EndpointGeneration(1);
  factor.candidate.backend.endpoint.backend_id = BackendId(1);
  factor.candidate.backend.endpoint.backend_boot = BackendBootId(1);
  factor.candidate.backend.endpoint.reference = "127.0.0.1:7001";
  factor.candidate.backend.trust_profile_id = model_router::TrustProfileId(1);
  factor.candidate.backend.trust_generation = model_router::TrustGeneration(1);
  factor.candidate.backend.trust_domain = TrustDomain::UNKNOWN;
  factor.candidate.backend.locality = model_router::LocalityKey("local/loopback");
  factor.candidate.backend.offline_capable = true;
  factor.candidate.backend.provenance = Provenance::SYNTHETIC;

  factor.candidate.context_limit_tokens = 100000;
  factor.candidate.quality_class = model_router::kUnclassifiedQuality;
  return factor;
}

[[nodiscard]] std::vector<FactorValue> factors_of(const FactorCase& factor, std::int64_t* score) {
  model_router::detail::RankingContext context;
  context.request = &factor.request;
  context.evidence = &factor.evidence;
  context.policy_verdict = factor.has_policy_verdict ? &factor.policy_verdict : nullptr;
  context.weights = &factor.weights;
  context.scales = &factor.scales;
  context.now = factor.now;
  context.continuity_backend = factor.continuity;
  context.has_continuity_backend = factor.has_continuity;
  context.diversity_reference_provider = factor.diversity;
  context.has_diversity_reference = factor.has_diversity;
  return model_router::detail::build_factors(factor.candidate, context, score);
}

[[nodiscard]] const FactorValue* find_factor(const std::vector<FactorValue>& factors,
                                             RankingFactor factor) {
  const auto iter = std::lower_bound(
      factors.begin(), factors.end(), factor,
      [](const FactorValue& value, RankingFactor key) { return value.factor < key; });
  if (iter == factors.end() || iter->factor != factor) {
    return nullptr;
  }
  return &*iter;
}

void expect_unknown(const FactorCase& factor, RankingFactor which) {
  std::int64_t score = -1;
  const std::vector<FactorValue> factors = factors_of(factor, &score);
  const FactorValue* value = find_factor(factors, which);
  MR_CHECK(value != nullptr);
  if (value == nullptr) {
    return;
  }
  MR_CHECK(!value->known);
  MR_CHECK_EQ(value->normalized, std::int64_t{0});
  MR_CHECK_EQ(value->contribution, std::int64_t{0});
  MR_CHECK_EQ(value->raw, std::int64_t{0});
  MR_CHECK_EQ(value->weight_ppm, factor.weights.weight(which));
}

/// Asserts the exact normalized value, raw value, weight, contribution, and the
/// complete set of factors that became known.
void expect_known(const FactorCase& factor, RankingFactor which, std::int64_t raw,
                  std::int64_t normalized, std::uint64_t evidence_generation,
                  const std::vector<RankingFactor>& also_known = {}) {
  std::int64_t score = 0;
  const std::vector<FactorValue> factors = factors_of(factor, &score);
  const FactorValue* value = find_factor(factors, which);
  MR_CHECK(value != nullptr);
  if (value == nullptr) {
    return;
  }
  const std::int64_t weight = factor.weights.weight(which);
  MR_CHECK(value->known);
  MR_CHECK_EQ(value->raw, raw);
  MR_CHECK_EQ(value->normalized, normalized);
  MR_CHECK_EQ(value->weight_ppm, weight);
  MR_CHECK_EQ(value->contribution, normalized * weight / FactorValue::kFactorScale);
  if (evidence_generation != 0) {
    MR_CHECK_EQ(value->evidence_generation, evidence_generation);
  }

  std::vector<RankingFactor> expected = also_known;
  expected.push_back(which);
  std::sort(expected.begin(), expected.end());
  std::vector<RankingFactor> known;
  std::int64_t total = 0;
  for (const FactorValue& other : factors) {
    total += other.contribution;
    if (other.known) {
      known.push_back(other.factor);
    }
  }
  MR_CHECK(known == expected);
  MR_CHECK_EQ(score, total);
}

[[nodiscard]] CostEvidence make_cost(ModelId model_id, std::int64_t total) {
  CostEvidence evidence;
  evidence.evidence_id = CostEvidenceId(1000 + model_id.value());
  evidence.price_generation = PriceGeneration(1);
  evidence.backend_id = BackendId(1);
  evidence.model_id = model_id;
  evidence.unit = model_router::reference::reference_cost_unit();
  evidence.input_micros_per_unit = total;
  evidence.output_micros_per_unit = total;
  evidence.estimated_total_micros = total;
  evidence.knownness = CostKnownness::QUOTED;
  evidence.effective_at_unix_millis = 1;
  evidence.source = "ranking-test";
  return evidence;
}

}  // namespace

MR_TEST(ranking, identical_state_ranks_identically_regardless_of_insertion_order) {
  const BackendId forward[] = {BackendId(1), BackendId(2), BackendId(3)};
  const BackendId reversed[] = {BackendId(3), BackendId(2), BackendId(1)};
  const BackendId shuffled[] = {BackendId(2), BackendId(3), BackendId(1)};
  const BackendId* orders[] = {forward, reversed, shuffled};

  RankRig rigs[3];
  for (int index = 0; index < 3; ++index) {
    rigs[index].start(RouterId(1));
    for (int position = 0; position < 3; ++position) {
      const BackendId backend_id = orders[index][position];
      // Distinct prices and latencies so the ranking is not a tie.
      rigs[index].add_backend(backend_id, 1,
                              static_cast<std::uint32_t>(1000 * backend_id.value()));
      rigs[index].publish_costs(backend_id, 100 * backend_id.value());
    }
  }

  RouteOutcome outcomes[3];
  for (int index = 0; index < 3; ++index) {
    RouteRequest request = make_request(rigs[index]);
    outcomes[index] = rigs[index].router->route(request);
  }

  MR_CHECK_EQ(outcomes[0].code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcomes[0].decision.authority.backend_id, BackendId(1));
  // Three backends each bind all three reference models.
  MR_CHECK_EQ(outcomes[0].route.ranking.size(), std::size_t{9});
  MR_CHECK_EQ(outcomes[0].decision.authority.model_id, rigs[0].catalog.specialist_model);
  MR_CHECK_EQ(outcomes[0].route.fallback_order.size(), std::size_t{2});
  MR_CHECK_EQ(outcomes[0].decision.explanation.semantic_digest.size(), std::size_t{64});
  const std::string digest = outcomes[0].decision.explanation.semantic_digest;
  const std::string decision_digest = outcomes[0].decision.semantic_digest();

  for (int index = 1; index < 3; ++index) {
    MR_CHECK_EQ(outcomes[index].code, OutcomeCode::ROUTED);
    MR_CHECK_EQ(outcomes[index].route.eligible_candidate_count,
                outcomes[0].route.eligible_candidate_count);
    MR_CHECK_EQ(outcomes[index].route.rejected_candidate_count,
                outcomes[0].route.rejected_candidate_count);
    MR_CHECK_EQ(outcomes[index].decision.authority.backend_id,
                outcomes[0].decision.authority.backend_id);
    MR_CHECK_EQ(outcomes[index].decision.authority.model_id,
                outcomes[0].decision.authority.model_id);
    MR_CHECK_EQ(outcomes[index].route.winner, outcomes[0].route.winner);
    MR_CHECK_EQ(outcomes[index].route.semantic_digest, digest);
    MR_CHECK_EQ(outcomes[index].decision.semantic_digest(), decision_digest);
    MR_CHECK_EQ(outcomes[index].route.ranking.size(), outcomes[0].route.ranking.size());
    for (std::size_t rank = 0; rank < outcomes[0].route.ranking.size(); ++rank) {
      MR_CHECK_EQ(outcomes[index].route.ranking[rank], outcomes[0].route.ranking[rank]);
    }
    MR_CHECK_EQ(outcomes[index].route.fallback_order.size(),
                outcomes[0].route.fallback_order.size());
    for (std::size_t position = 0; position < outcomes[0].route.fallback_order.size();
         ++position) {
      MR_CHECK_EQ(outcomes[index].route.fallback_order[position],
                  outcomes[0].route.fallback_order[position]);
    }
    // The rendered text intentionally includes the request and decision
    // identity, so the content comparison is on the semantic digest (which
    // covers requirement digest, winner, ranking, rejections, fallback order
    // and bound generations) plus the structured content asserted above.
    MR_CHECK_EQ(outcomes[index].route.semantic_digest, digest);
    MR_CHECK_EQ(outcomes[index].route.rejections, outcomes[0].route.rejections);
  }
}

MR_TEST(ranking, hard_invalid_candidates_never_appear_in_the_ranked_list) {
  RankRig rig;
  rig.start(RouterId(1));
  rig.add_backend(BackendId(1), 1);
  rig.add_backend(BackendId(2), 1);
  rig.publish_costs(BackendId(1), 500);
  rig.publish_costs(BackendId(2), 100);
  // Backend 2 is cheaper but unhealthy: it must never be ranked.
  MR_CHECK(rig.publisher
               .health(*rig.router, BackendId(2), BackendBootId(1), HealthState::UNHEALTHY, rig.now())
               .accepted());

  RouteRequest request = make_request(rig);
  request.requirements.required_capabilities.push_back(require_code());
  const RouteOutcome outcome = rig.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, BackendId(1));
  MR_CHECK_EQ(outcome.route.ranking.size(), std::size_t{1});
  // Two models lack the capability on the healthy backend; the whole unhealthy
  // backend is rejected before capability is even considered.
  MR_CHECK_EQ(outcome.route.rejections.size(), std::size_t{5});
  MR_CHECK_EQ(outcome.route.eligible_candidate_count, std::uint32_t{1});
  MR_CHECK_EQ(outcome.route.rejected_candidate_count, std::uint32_t{5});
  std::size_t unhealthy = 0;
  for (const RouteRejection& rejection : outcome.route.rejections) {
    if (rejection.code == OutcomeCode::REJECT_UNHEALTHY) {
      ++unhealthy;
    }
  }
  MR_CHECK_EQ(unhealthy, std::size_t{3});
  for (const RankedCandidate& ranked : outcome.route.ranking) {
    MR_CHECK_NE(ranked.key.backend_id, BackendId(2));
    for (const RouteRejection& rejection : outcome.route.rejections) {
      const bool same_candidate = rejection.model_id == ranked.key.model_id.value() &&
                                  rejection.backend_id == ranked.key.backend_id.value();
      MR_CHECK(!same_candidate);
    }
  }
}

MR_TEST(ranking, unknown_factors_contribute_exactly_zero) {
  RankRig unpriced;
  unpriced.start(RouterId(1));
  unpriced.add_backend(BackendId(1), 1);
  RouteRequest request = make_request(unpriced);
  request.requirements.required_capabilities.push_back(require_code());
  const RouteOutcome unknown = unpriced.router->route(request);
  MR_CHECK_EQ(unknown.code, OutcomeCode::ROUTED);
  const RankedCandidate& winner = unknown.route.ranking.front();
  const FactorValue* cost = winner.find(RankingFactor::COST_TOTAL);
  MR_CHECK(cost != nullptr);
  if (cost != nullptr) {
    MR_CHECK(!cost->known);
    MR_CHECK_EQ(cost->normalized, std::int64_t{0});
    MR_CHECK_EQ(cost->contribution, std::int64_t{0});
    MR_CHECK_EQ(cost->raw, std::int64_t{0});
    const RankingWeights defaults;
    MR_CHECK_EQ(cost->weight_ppm, defaults.cost_total_ppm);
    MR_CHECK_EQ(cost->evidence_generation, std::uint64_t{0});
  }
  MR_CHECK(!winner.fully_known());

  // Publishing a price adds exactly the cost factor's contribution and nothing
  // else, which proves an unknown factor was contributing exactly zero.
  RankRig priced;
  priced.start(RouterId(1));
  priced.add_backend(BackendId(1), 1);
  priced.publish_costs(BackendId(1), 400000);
  const RouteOutcome known = priced.router->route(request);
  MR_CHECK_EQ(known.code, OutcomeCode::ROUTED);
  const FactorValue* priced_cost = known.route.ranking.front().find(RankingFactor::COST_TOTAL);
  MR_CHECK(priced_cost != nullptr);
  if (priced_cost != nullptr) {
    MR_CHECK(priced_cost->known);
    MR_CHECK_EQ(known.route.ranking.front().score - winner.score, priced_cost->contribution);
    MR_CHECK_EQ(priced_cost->normalized, std::int64_t{600000});
  }
}

MR_TEST(ranking, a_cheaper_candidate_never_defeats_a_capability_floor) {
  RankRig rig;
  rig.start(RouterId(1));
  rig.add_backend(BackendId(1), 1);
  rig.add_backend(BackendId(2), 1);
  // The specialist on backend 1 is free; on backend 2 it is expensive.
  rig.publish_costs(BackendId(1), 1);
  rig.publish_costs(BackendId(2), 1000000);

  RouteRequest request = make_request(rig);
  request.requirements.required_capabilities.push_back(require_code());
  const RouteOutcome outcome = rig.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.decision.authority.model_id, rig.catalog.specialist_model);
  MR_CHECK_EQ(outcome.decision.authority.backend_id, BackendId(1));
  MR_CHECK_EQ(outcome.route.ranking.size(), std::size_t{2});
  MR_CHECK(outcome.route.ranking.front().score > outcome.route.ranking.back().score);
  for (const RankedCandidate& ranked : outcome.route.ranking) {
    MR_CHECK_EQ(ranked.key.model_id, rig.catalog.specialist_model);
  }
  // The two cheaper models are hard-rejected on both backends, never ranked.
  MR_CHECK_EQ(outcome.route.rejections.size(), std::size_t{4});
  for (const RouteRejection& rejection : outcome.route.rejections) {
    MR_CHECK_EQ(rejection.code, OutcomeCode::REJECT_CAPABILITY);
    MR_CHECK_NE(rejection.model_id, rig.catalog.specialist_model.value());
  }
}

MR_TEST(ranking, ties_break_by_canonical_candidate_identity) {
  RankRig first;
  first.start(RouterId(1));
  first.add_backend(BackendId(1), 1);
  first.add_backend(BackendId(2), 1);
  first.publish_costs(BackendId(1), 500);
  first.publish_costs(BackendId(2), 500);
  RouteRequest request = make_request(first);
  request.requirements.required_capabilities.push_back(require_code());
  const RouteOutcome outcome = first.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.route.ranking.size(), std::size_t{2});
  MR_CHECK_EQ(outcome.route.ranking[0].score, outcome.route.ranking[1].score);
  MR_CHECK_EQ(outcome.route.ranking[0].key.backend_id, BackendId(1));
  MR_CHECK_EQ(outcome.route.ranking[1].key.backend_id, BackendId(2));
  MR_CHECK(model_router::candidate_ranks_before(outcome.route.ranking[0],
                                                 outcome.route.ranking[1]));
  MR_CHECK(!model_router::candidate_ranks_before(outcome.route.ranking[1],
                                                  outcome.route.ranking[0]));
  MR_CHECK_EQ(outcome.route.tie_break_reason,
              std::string("canonical factor sequence then canonical candidate identity"));

  // Registering the identical state in the opposite order picks the same winner.
  RankRig second;
  second.start(RouterId(1));
  second.add_backend(BackendId(2), 1);
  second.add_backend(BackendId(1), 1);
  second.publish_costs(BackendId(2), 500);
  second.publish_costs(BackendId(1), 500);
  const RouteOutcome reversed = second.router->route(request);
  MR_CHECK_EQ(reversed.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(reversed.route.ranking[0].key, outcome.route.ranking[0].key);
  MR_CHECK_EQ(reversed.route.ranking[1].key, outcome.route.ranking[1].key);
  MR_CHECK_EQ(reversed.decision.authority.backend_id, BackendId(1));
  MR_CHECK_EQ(reversed.route.ranking[0].score, reversed.route.ranking[1].score);
}

MR_TEST(ranking, higher_score_wins_and_custom_weights_change_the_winner) {
  RankRig rig;
  rig.start(RouterId(1));
  // Backend 1 is cheap but slow; backend 2 is fast but expensive.
  rig.add_backend(BackendId(1), 1, 200000);
  rig.add_backend(BackendId(2), 1, 1000);
  rig.publish_costs(BackendId(1), 1);
  rig.publish_costs(BackendId(2), 1000000);

  RouteRequest request = make_request(rig);
  request.requirements.required_capabilities.push_back(require_code());
  const RouteOutcome default_weights = rig.router->route(request);
  MR_CHECK_EQ(default_weights.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(default_weights.decision.authority.backend_id, BackendId(1));
  MR_CHECK(default_weights.route.ranking.front().score >
            default_weights.route.ranking.back().score);

  // Weighting latency alone must pick the fast backend instead.
  RankingWeights latency_only;
  latency_only.capability_fit_ppm = 0;
  latency_only.preferred_capability_coverage_ppm = 0;
  latency_only.quality_class_ppm = 0;
  latency_only.cost_total_ppm = 0;
  latency_only.latency_ppm = 1000000;
  latency_only.tail_latency_ppm = 0;
  latency_only.queue_delay_ppm = 0;
  latency_only.warmth_ppm = 0;
  latency_only.residency_ppm = 0;
  latency_only.locality_ppm = 0;
  latency_only.network_distance_ppm = 0;
  latency_only.provider_availability_ppm = 0;
  latency_only.backend_health_ppm = 0;
  latency_only.backend_readiness_ppm = 0;
  latency_only.capacity_headroom_ppm = 0;
  latency_only.reservation_confidence_ppm = 0;
  latency_only.slo_headroom_ppm = 0;
  latency_only.context_headroom_ppm = 0;
  latency_only.trust_preference_ppm = 0;
  latency_only.policy_preference_ppm = 0;
  latency_only.model_affinity_ppm = 0;
  latency_only.cache_affinity_ppm = 0;
  latency_only.historical_reliability_ppm = 0;
  latency_only.failure_domain_diversity_ppm = 0;
  latency_only.data_movement_cost_ppm = 0;
  latency_only.backend_startup_cost_ppm = 0;
  latency_only.route_switch_penalty_ppm = 0;
  latency_only.continuity_stickiness_ppm = 0;
  latency_only.caller_preference_ppm = 0;
  MR_CHECK(latency_only.validate().empty());

  RankRig weighted;
  weighted.start(RouterId(1), latency_only);
  weighted.add_backend(BackendId(1), 1, 200000);
  weighted.add_backend(BackendId(2), 1, 1000);
  weighted.publish_costs(BackendId(1), 1);
  weighted.publish_costs(BackendId(2), 1000000);
  const RouteOutcome latency_first = weighted.router->route(request);
  MR_CHECK_EQ(latency_first.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(latency_first.decision.authority.backend_id, BackendId(2));
  MR_CHECK(latency_first.decision.authority.backend_id !=
           default_weights.decision.authority.backend_id);
  MR_CHECK(latency_first.route.ranking.front().score >
            latency_first.route.ranking.back().score);
  // The cost factor is still evaluated, it simply carries no weight.
  const FactorValue* cost = latency_first.route.ranking.front().find(RankingFactor::COST_TOTAL);
  MR_CHECK(cost != nullptr);
  if (cost != nullptr) {
    MR_CHECK(cost->known);
    MR_CHECK_EQ(cost->contribution, std::int64_t{0});
  }
}

MR_TEST(ranking, a_candidate_with_no_evidence_has_only_unknown_factors) {
  const FactorCase factor = make_factor_case();
  std::int64_t score = -1;
  const std::vector<FactorValue> factors = factors_of(factor, &score);
  MR_CHECK_EQ(factors.size(), static_cast<std::size_t>(RankingFactor::kCount));
  MR_CHECK_EQ(score, std::int64_t{0});
  const std::vector<RankingFactor> canonical = model_router::canonical_factor_order();
  MR_CHECK_EQ(canonical.size(), factors.size());
  for (std::size_t index = 0; index < factors.size(); ++index) {
    MR_CHECK_EQ(factors[index].factor, canonical[index]);
    MR_CHECK(!factors[index].known);
    MR_CHECK_EQ(factors[index].normalized, std::int64_t{0});
    MR_CHECK_EQ(factors[index].contribution, std::int64_t{0});
    MR_CHECK_EQ(factors[index].raw, std::int64_t{0});
    MR_CHECK_EQ(factors[index].weight_ppm, factor.weights.weight(factors[index].factor));
  }
  MR_CHECK_EQ(model_router::to_string(RankingFactor::CAPABILITY_FIT),
              std::string_view("capability_fit"));
  MR_CHECK_EQ(model_router::to_string(RankingFactor::CALLER_PREFERENCE),
              std::string_view("caller_preference"));
  MR_CHECK_EQ(model_router::to_string(RankingFactor::kCount), std::string_view("unknown"));
}

MR_TEST(ranking, capability_fit_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::CAPABILITY_FIT);
  FactorCase factor = make_factor_case();
  factor.candidate.required_capabilities_total = 4;
  factor.candidate.required_capabilities_satisfied = 3;
  expect_known(factor, RankingFactor::CAPABILITY_FIT, 3, 750000, 0);
}

MR_TEST(ranking, preferred_capability_coverage_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::PREFERRED_CAPABILITY_COVERAGE);
  FactorCase factor = make_factor_case();
  factor.candidate.preferred_capabilities_total = 3;
  factor.candidate.preferred_capabilities_satisfied = 1;
  expect_known(factor, RankingFactor::PREFERRED_CAPABILITY_COVERAGE, 1, 333333, 0);
}

MR_TEST(ranking, quality_class_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::QUALITY_CLASS);
  FactorCase factor = make_factor_case();
  factor.candidate.quality_class = 250;
  expect_known(factor, RankingFactor::QUALITY_CLASS, 250, 250000, 0);
}

MR_TEST(ranking, cost_total_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::COST_TOTAL);
  FactorCase factor = make_factor_case();
  factor.evidence.cost = make_cost(ModelId(1), 400000);
  expect_known(factor, RankingFactor::COST_TOTAL, 400000, 600000, 1);
}

MR_TEST(ranking, latency_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::LATENCY);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.latency.dispatch_micros = 1000000;
  factor.candidate.backend.latency.health_generation = model_router::HealthGeneration(7);
  expect_known(factor, RankingFactor::LATENCY, 1000000, 800000, 7);
}

MR_TEST(ranking, tail_latency_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::TAIL_LATENCY);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.latency.tail_micros = 5000000;
  expect_known(factor, RankingFactor::TAIL_LATENCY, 5000000, 750000, 0);
}

MR_TEST(ranking, queue_delay_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::QUEUE_DELAY);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.latency.queue_micros = 250000;
  expect_known(factor, RankingFactor::QUEUE_DELAY, 250000, 750000, 0);
}

MR_TEST(ranking, warmth_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::WARMTH);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.readiness.generation = model_router::ReadinessGeneration(3);
  factor.candidate.backend.readiness.state = ReadinessState::READY;
  expect_known(factor, RankingFactor::WARMTH, 1, 1000000, 3,
               {RankingFactor::BACKEND_READINESS});

  // A WARMING backend is worth half, and an observed cold start is subtracted.
  FactorCase warming = make_factor_case();
  warming.candidate.backend.readiness.generation = model_router::ReadinessGeneration(3);
  warming.candidate.backend.readiness.state = ReadinessState::WARMING;
  warming.candidate.backend.latency.cold_start_penalty_micros = 5000000;
  expect_known(warming, RankingFactor::WARMTH, 2, 0, 3,
               {RankingFactor::BACKEND_READINESS, RankingFactor::BACKEND_STARTUP_COST});
}

MR_TEST(ranking, residency_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::RESIDENCY);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.residency.generation = model_router::ResidencyGeneration(4);
  factor.candidate.backend.residency.state = ResidencyState::RESIDENT;
  expect_known(factor, RankingFactor::RESIDENCY, 1, 1000000, 4);

  FactorCase loading = make_factor_case();
  loading.candidate.backend.residency.generation = model_router::ResidencyGeneration(4);
  loading.candidate.backend.residency.state = ResidencyState::LOADING;
  expect_known(loading, RankingFactor::RESIDENCY, 2, 500000, 4);
}

MR_TEST(ranking, locality_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::LOCALITY);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.trust_domain = TrustDomain::LOCAL;
  expect_known(factor, RankingFactor::LOCALITY, 4, 1000000, 0,
               {RankingFactor::TRUST_PREFERENCE, RankingFactor::DATA_MOVEMENT_COST});
}

MR_TEST(ranking, network_distance_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::NETWORK_DISTANCE);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.network_distance = 25000;
  expect_known(factor, RankingFactor::NETWORK_DISTANCE, 25000, 750000, 0,
               {RankingFactor::DATA_MOVEMENT_COST});
}

MR_TEST(ranking, provider_availability_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::PROVIDER_AVAILABILITY);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.availability.generation = model_router::AvailabilityGeneration(5);
  factor.candidate.backend.availability.state = AvailabilityState::AVAILABLE;
  expect_known(factor, RankingFactor::PROVIDER_AVAILABILITY, 1, 1000000, 5);
}

MR_TEST(ranking, backend_health_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::BACKEND_HEALTH);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.health.generation = model_router::HealthGeneration(6);
  factor.candidate.backend.health.state = HealthState::HEALTHY;
  expect_known(factor, RankingFactor::BACKEND_HEALTH, 1, 1000000, 6);

  FactorCase degraded = make_factor_case();
  degraded.candidate.backend.health.generation = model_router::HealthGeneration(6);
  degraded.candidate.backend.health.state = HealthState::DEGRADED;
  expect_known(degraded, RankingFactor::BACKEND_HEALTH, 2, 500000, 6);
}

MR_TEST(ranking, backend_readiness_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::BACKEND_READINESS);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.readiness.generation = model_router::ReadinessGeneration(3);
  factor.candidate.backend.readiness.state = ReadinessState::READY;
  expect_known(factor, RankingFactor::BACKEND_READINESS, 1, 1000000, 3,
               {RankingFactor::WARMTH});
}

MR_TEST(ranking, capacity_headroom_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::CAPACITY_HEADROOM);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.capacity.generation = model_router::CapacityGeneration(9);
  factor.candidate.backend.capacity_detail.available_slots = 6;
  factor.candidate.backend.capacity_detail.total_slots = 8;
  expect_known(factor, RankingFactor::CAPACITY_HEADROOM, 6, 750000, 9);
}

MR_TEST(ranking, reservation_confidence_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::RESERVATION_CONFIDENCE);
  FactorCase factor = make_factor_case();
  factor.evidence.reservation_present = true;
  factor.evidence.reservation.reservation_id = model_router::ReservationId(1);
  factor.evidence.reservation.generation = model_router::ReservationGeneration(1);
  factor.evidence.reservation.backend_id = BackendId(1);
  factor.evidence.reservation.backend_boot = BackendBootId(1);
  expect_known(factor, RankingFactor::RESERVATION_CONFIDENCE, 1, 1000000, 0);
}

MR_TEST(ranking, slo_headroom_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::SLO_HEADROOM);
  FactorCase factor = make_factor_case();
  factor.evidence.slo.latency_target_micros = 2000000;
  factor.candidate.backend.latency.dispatch_micros = 1000000;
  expect_known(factor, RankingFactor::SLO_HEADROOM, 1000000, 500000, 0,
               {RankingFactor::LATENCY});
}

MR_TEST(ranking, context_headroom_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::CONTEXT_HEADROOM);
  FactorCase factor = make_factor_case();
  factor.request.requirements.min_context_tokens = 1000;
  factor.candidate.context_limit_tokens = 201000;
  expect_known(factor, RankingFactor::CONTEXT_HEADROOM, 200000, 200000, 0);
}

MR_TEST(ranking, trust_preference_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::TRUST_PREFERENCE);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.trust_domain = TrustDomain::TRUSTED_REMOTE;
  expect_known(factor, RankingFactor::TRUST_PREFERENCE, 2, 500000, 1,
               {RankingFactor::LOCALITY});
}

MR_TEST(ranking, policy_preference_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::POLICY_PREFERENCE);
  FactorCase factor = make_factor_case();
  factor.has_policy_verdict = true;
  factor.policy_verdict.preference = 250000;
  expect_known(factor, RankingFactor::POLICY_PREFERENCE, 250000, 250000, 0);
}

MR_TEST(ranking, model_affinity_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::MODEL_AFFINITY);
  FactorCase factor = make_factor_case();
  factor.request.requirements.affinity_model = ModelId(1);
  expect_known(factor, RankingFactor::MODEL_AFFINITY, 1, 1000000, 0,
               {RankingFactor::CALLER_PREFERENCE});

  FactorCase mismatch = make_factor_case();
  mismatch.request.requirements.affinity_model = ModelId(9);
  expect_known(mismatch, RankingFactor::MODEL_AFFINITY, 0, 0, 0,
               {RankingFactor::CALLER_PREFERENCE});
}

MR_TEST(ranking, cache_affinity_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::CACHE_AFFINITY);
  FactorCase factor = make_factor_case();
  factor.has_continuity = true;
  factor.continuity = BackendId(1);
  expect_known(factor, RankingFactor::CACHE_AFFINITY, 1, 1000000, 0,
               {RankingFactor::ROUTE_SWITCH_PENALTY});

  FactorCase switched = make_factor_case();
  switched.has_continuity = true;
  switched.continuity = BackendId(2);
  expect_known(switched, RankingFactor::CACHE_AFFINITY, 0, 0, 0,
               {RankingFactor::ROUTE_SWITCH_PENALTY});
}

MR_TEST(ranking, historical_reliability_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::HISTORICAL_RELIABILITY);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.reliability_ppm = 9500;
  expect_known(factor, RankingFactor::HISTORICAL_RELIABILITY, 9500, 950000, 0);
}

MR_TEST(ranking, failure_domain_diversity_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::FAILURE_DOMAIN_DIVERSITY);
  FactorCase factor = make_factor_case();
  factor.has_diversity = true;
  factor.diversity = ProviderId(2);
  expect_known(factor, RankingFactor::FAILURE_DOMAIN_DIVERSITY, 1, 1000000, 0);

  FactorCase same_provider = make_factor_case();
  same_provider.has_diversity = true;
  same_provider.diversity = ProviderId(1);
  expect_known(same_provider, RankingFactor::FAILURE_DOMAIN_DIVERSITY, 0, 0, 0);
}

MR_TEST(ranking, data_movement_cost_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::DATA_MOVEMENT_COST);
  // A remote backend pays for moving data; a local one pays nothing but is
  // still KNOWN, which is not the same as absent evidence.
  FactorCase remote = make_factor_case();
  remote.candidate.backend.trust_domain = TrustDomain::TRUSTED_REMOTE;
  remote.candidate.backend.network_distance = 50000;
  expect_known(remote, RankingFactor::DATA_MOVEMENT_COST, 50000, 500000, 0,
               {RankingFactor::NETWORK_DISTANCE, RankingFactor::LOCALITY,
                RankingFactor::TRUST_PREFERENCE});

  FactorCase local = make_factor_case();
  local.candidate.backend.trust_domain = TrustDomain::LOCAL;
  expect_known(local, RankingFactor::DATA_MOVEMENT_COST, 0, 1000000, 0,
               {RankingFactor::LOCALITY, RankingFactor::TRUST_PREFERENCE});
}

MR_TEST(ranking, backend_startup_cost_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::BACKEND_STARTUP_COST);
  FactorCase factor = make_factor_case();
  factor.candidate.backend.latency.cold_start_penalty_micros = 2000000;
  expect_known(factor, RankingFactor::BACKEND_STARTUP_COST, 2000000, 800000, 0);
}

MR_TEST(ranking, route_switch_penalty_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::ROUTE_SWITCH_PENALTY);
  FactorCase same = make_factor_case();
  same.has_continuity = true;
  same.continuity = BackendId(1);
  expect_known(same, RankingFactor::ROUTE_SWITCH_PENALTY, 0, 1000000, 0,
               {RankingFactor::CACHE_AFFINITY});

  FactorCase different = make_factor_case();
  different.has_continuity = true;
  different.continuity = BackendId(2);
  expect_known(different, RankingFactor::ROUTE_SWITCH_PENALTY, 1, 0, 0,
               {RankingFactor::CACHE_AFFINITY});
}

MR_TEST(ranking, continuity_stickiness_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::CONTINUITY_STICKINESS);
  FactorCase factor = make_factor_case();
  factor.request.requirements.stickiness.prefer_same_backend = true;
  factor.request.requirements.stickiness.sticky_backend = BackendId(1);
  expect_known(factor, RankingFactor::CONTINUITY_STICKINESS, 1, 1000000, 0);

  // Two continuity preferences, only one of which matches.
  FactorCase partial = make_factor_case();
  partial.request.requirements.stickiness.prefer_same_backend = true;
  partial.request.requirements.stickiness.sticky_backend = BackendId(1);
  partial.request.requirements.stickiness.prefer_same_provider = true;
  partial.request.requirements.stickiness.sticky_provider = ProviderId(9);
  expect_known(partial, RankingFactor::CONTINUITY_STICKINESS, 1, 500000, 0);
}

MR_TEST(ranking, caller_preference_matches_its_formula) {
  expect_unknown(make_factor_case(), RankingFactor::CALLER_PREFERENCE);
  FactorCase factor = make_factor_case();
  factor.request.requirements.affinity_provider = ProviderId(1);
  expect_known(factor, RankingFactor::CALLER_PREFERENCE, 1, 1000000, 0);

  FactorCase mismatch = make_factor_case();
  mismatch.request.requirements.affinity_provider = ProviderId(9);
  mismatch.request.requirements.affinity_backend = BackendId(9);
  expect_known(mismatch, RankingFactor::CALLER_PREFERENCE, 0, 0, 0);
}

MR_TEST(ranking, factor_order_is_canonical_in_every_ranked_candidate) {
  RankRig rig;
  rig.start(RouterId(1));
  rig.add_backend(BackendId(1), 1);
  rig.add_backend(BackendId(2), 1);
  rig.publish_costs(BackendId(1), 500);
  rig.publish_costs(BackendId(2), 900);
  RouteRequest request = make_request(rig);
  request.requirements.required_capabilities.push_back(require_code());
  request.requirements.preferred_capabilities.push_back(
      CapabilityKey(std::string(model_router::capability_keys::reasoning)));
  request.requirements.affinity_backend = BackendId(1);
  const RouteOutcome outcome = rig.router->route(request);
  MR_CHECK_EQ(outcome.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(outcome.route.ranking.size(), std::size_t{2});
  const std::vector<RankingFactor> canonical = model_router::canonical_factor_order();
  for (const RankedCandidate& ranked : outcome.route.ranking) {
    MR_CHECK_EQ(ranked.factors.size(), canonical.size());
    for (std::size_t index = 0; index < canonical.size(); ++index) {
      MR_CHECK_EQ(ranked.factors[index].factor, canonical[index]);
    }
    // find() uses the canonical order, so it must resolve every factor.
    for (const RankingFactor factor : canonical) {
      MR_CHECK(ranked.find(factor) != nullptr);
    }
  }
}

MR_TEST(ranking, ranking_is_stable_across_repeated_identical_routes) {
  RankRig rig;
  rig.start(RouterId(1));
  rig.add_backend(BackendId(1), 1, 1000);
  rig.add_backend(BackendId(2), 1, 50000);
  rig.publish_costs(BackendId(1), 100);
  rig.publish_costs(BackendId(2), 900);
  RouteRequest request = make_request(rig);
  request.requirements.required_capabilities.push_back(require_code());

  const RouteOutcome first = rig.router->route(request);
  const RouteOutcome second = rig.router->route(request);
  MR_CHECK_EQ(first.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(second.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(first.decision.authority.backend_id, BackendId(1));
  MR_CHECK_EQ(second.decision.authority.backend_id, BackendId(1));
  MR_CHECK_EQ(second.route.ranking.size(), first.route.ranking.size());
  for (std::size_t rank = 0; rank < first.route.ranking.size(); ++rank) {
    MR_CHECK_EQ(second.route.ranking[rank].key, first.route.ranking[rank].key);
    MR_CHECK_EQ(second.route.ranking[rank].rank, first.route.ranking[rank].rank);
  }
  // The second route now has a continuity reference, so exactly the two
  // continuity factors become known; nothing else may move.
  for (std::size_t rank = 0; rank < first.route.ranking.size(); ++rank) {
    const RankedCandidate& before = first.route.ranking[rank];
    const RankedCandidate& after = second.route.ranking[rank];
    MR_CHECK_EQ(after.factors.size(), before.factors.size());
    for (std::size_t index = 0; index < before.factors.size(); ++index) {
      const FactorValue& left = before.factors[index];
      const FactorValue& right = after.factors[index];
      MR_CHECK_EQ(right.factor, left.factor);
      const bool continuity = left.factor == RankingFactor::CACHE_AFFINITY ||
                              left.factor == RankingFactor::ROUTE_SWITCH_PENALTY;
      if (!continuity) {
        MR_CHECK_EQ(right.known, left.known);
        MR_CHECK_EQ(right.normalized, left.normalized);
        MR_CHECK_EQ(right.contribution, left.contribution);
        MR_CHECK_EQ(right.raw, left.raw);
      } else {
        MR_CHECK(!left.known);
        MR_CHECK(right.known);
      }
    }
  }

  // Two independent routers replaying the same sequence agree byte for byte.
  RankRig replay;
  replay.start(RouterId(1));
  replay.add_backend(BackendId(1), 1, 1000);
  replay.add_backend(BackendId(2), 1, 50000);
  replay.publish_costs(BackendId(1), 100);
  replay.publish_costs(BackendId(2), 900);
  const RouteOutcome replay_first = replay.router->route(request);
  const RouteOutcome replay_second = replay.router->route(request);
  MR_CHECK_EQ(replay_first.route.ranking, first.route.ranking);
  MR_CHECK_EQ(replay_second.route.ranking, second.route.ranking);
  // The decision identity differs between the two routers, so the replay
  // agreement is asserted on the content-derived digests and structured
  // content rather than on the identity-bearing rendering.
  MR_CHECK_EQ(replay_first.decision.semantic_digest(), first.decision.semantic_digest());
  MR_CHECK_EQ(replay_second.decision.semantic_digest(), second.decision.semantic_digest());
  MR_CHECK_EQ(replay_second.route.semantic_digest, second.route.semantic_digest);
  MR_CHECK_EQ(replay_second.route.rejections, second.route.rejections);
  MR_CHECK_EQ(replay_second.route.fallback_order, second.route.fallback_order);
}
