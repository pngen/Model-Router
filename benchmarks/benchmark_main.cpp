// Model Router - routing throughput benchmark suite.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// WHAT THIS MEASURES
//   Completed ROUTING operations only: candidate discovery, hard eligibility
//   filtering, factor construction, deterministic ranking, route-decision
//   commit, pre-dispatch revalidation, explanation generation, snapshots,
//   invariant scans, and durable save/load. Every ops_per_sec value printed
//   below is ROUTING throughput, never inference throughput. This program never
//   runs a model, never sends a request to a backend, and never measures tokens
//   or generations per second.
//
// HOW TIME IS MEASURED
//   The router is driven by model_router::LogicalClock, so freshness, expiry,
//   and currentness decisions are deterministic and never depend on the wall
//   clock. Wall time is measured with std::chrono::steady_clock around the
//   measured section only. There are no timeouts, no sleeps, and no artificial
//   waits anywhere in this program.
//
// CORRECTNESS GUARDS
//   Every measured phase is followed by ModelRouter::check_invariants() and by
//   a decision guard that checks the routed winner against the winner the
//   catalog makes deterministic (the canonically smallest candidate identity,
//   because every candidate carries identical evidence). A failed guard prints
//   the violation and exits non-zero.
//
// EXIT STATUS
//   0  every guard passed and every measured phase behaved as documented
//   1  a benchmark guard failed (invariants, expected winner, rejection counts)
//   2  bad command line
//   3  the measurements completed but the core library violated a contract the
//      benchmark measures; the offending observation is printed above the exit
//      (currently the durable save/load round-trip)
//
//   A non-zero exit is never hidden: the benchmark still finishes every
//   measurement it can so the report is complete.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "model_router/model_router.hpp"

namespace mr = model_router;

namespace {

// ---------------------------------------------------------------------------
// Failure handling. Guards fail loudly with a non-zero exit status.
// ---------------------------------------------------------------------------

struct BenchmarkFailure {
  std::string message;
};

[[noreturn]] void fail(std::string message) { throw BenchmarkFailure{std::move(message)}; }

void require(bool condition, const std::string& message) {
  if (!condition) {
    fail(message);
  }
}

/// Removes the durable state file (and its atomic-rename sibling) on every exit
/// path, including a guard failure that unwinds through an exception.
class StateFileGuard {
 public:
  explicit StateFileGuard(std::string path) : path_(std::move(path)) {}

  ~StateFileGuard() {
    std::remove(path_.c_str());
    std::remove((path_ + ".tmp").c_str());
  }

  StateFileGuard(const StateFileGuard&) = delete;
  StateFileGuard& operator=(const StateFileGuard&) = delete;
  StateFileGuard(StateFileGuard&&) = delete;
  StateFileGuard& operator=(StateFileGuard&&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
};

// ---------------------------------------------------------------------------
// Timing and reporting
// ---------------------------------------------------------------------------

using SteadyClock = std::chrono::steady_clock;

struct Timing {
  std::uint64_t ops{0};
  double ns_per_op{0.0};
  double ops_per_sec{0.0};
};

template <class Operation>
[[nodiscard]] Timing measure_ops(std::uint64_t ops, Operation&& operation) {
  const SteadyClock::time_point start = SteadyClock::now();
  for (std::uint64_t index = 0; index < ops; ++index) {
    (void)operation(index);
  }
  const SteadyClock::time_point finish = SteadyClock::now();
  const double total_ns =
      std::chrono::duration<double, std::nano>(finish - start).count();
  Timing timing;
  timing.ops = ops;
  timing.ns_per_op = ops == 0 ? 0.0 : total_ns / static_cast<double>(ops);
  timing.ops_per_sec = timing.ns_per_op > 0.0 ? 1.0e9 / timing.ns_per_op : 0.0;
  return timing;
}

/// Subtracts one measured phase from a superset phase. The difference is the
/// cost of the work the superset does on top of the subset.
[[nodiscard]] Timing subtract(const Timing& superset, const Timing& subset) {
  Timing timing;
  timing.ops = superset.ops;
  timing.ns_per_op = superset.ns_per_op - subset.ns_per_op;
  timing.ops_per_sec = timing.ns_per_op > 0.0 ? 1.0e9 / timing.ns_per_op : 0.0;
  return timing;
}

struct MetricContext {
  std::uint64_t scale{0};
  std::uint64_t models{0};
  std::uint64_t backends{0};
  std::uint64_t candidates{0};
};

void print_metric(const char* metric, const MetricContext& context, const Timing& timing) {
  std::cout << "metric=" << metric << " scale=" << context.scale << " ops=" << timing.ops
            << std::fixed << std::setprecision(2) << " ns_per_op=" << timing.ns_per_op
            << " ops_per_sec=" << timing.ops_per_sec << " models=" << context.models
            << " backends=" << context.backends << " candidates=" << context.candidates << "\n";
}

void note(const std::string& text) { std::cout << "# " << text << "\n"; }

// ---------------------------------------------------------------------------
// Synthetic catalog built entirely through the public API
// ---------------------------------------------------------------------------

constexpr std::uint32_t kQualityClass = 50;
constexpr std::uint32_t kModelContextTokens = 131072;
constexpr std::uint32_t kModelMaxOutputTokens = 16384;
constexpr mr::UnixMillis kNow = 1700000000000LL;

struct CatalogSpec {
  std::uint32_t models{1};
  std::uint32_t backends{1};
  /// Distinct model bindings per backend; must be <= models.
  std::uint32_t bindings_per_backend{1};

  [[nodiscard]] std::uint64_t candidate_count() const {
    return static_cast<std::uint64_t>(backends) * static_cast<std::uint64_t>(bindings_per_backend);
  }
};

[[nodiscard]] mr::ModalitySet text_modalities() {
  return mr::ModalitySet(static_cast<std::uint32_t>(mr::Modality::TEXT));
}

[[nodiscard]] mr::CapabilityEvidence make_claim(std::string_view key, mr::CapabilityState state,
                                                mr::CapabilityProfileId profile,
                                                mr::CapabilityGeneration generation) {
  mr::CapabilityEvidence evidence;
  evidence.key = mr::CapabilityKey(std::string(key));
  evidence.state = state;
  evidence.generation = generation;
  evidence.profile_id = profile;
  evidence.observed_at_unix_millis = kNow;
  evidence.expires_at_unix_millis = mr::kNoExpiry;
  evidence.source = "benchmark-synthetic";
  return evidence;
}

[[nodiscard]] mr::ModelDescriptor make_model(std::uint32_t index) {
  mr::ModelDescriptor model;
  model.model_id = mr::ModelId(index);
  model.model_generation = mr::ModelGeneration(1);
  model.model_version_id = mr::ModelVersionId(index);
  model.artifact_generation = mr::ArtifactGeneration(1);
  model.family_id = mr::ModelFamilyId(index);
  model.context_limit_tokens = kModelContextTokens;
  model.max_output_tokens = kModelMaxOutputTokens;
  model.input_modalities = text_modalities();
  model.output_modalities = text_modalities();
  model.quality_class = kQualityClass;
  model.lifecycle = mr::ModelLifecycle::CURRENT;
  model.capability_profile_id = mr::CapabilityProfileId(index);
  model.capability_generation = mr::CapabilityGeneration(1);
  model.provenance = mr::Provenance::SYNTHETIC;
  model.registered_at_unix_millis = kNow;
  model.display_name = "bench-model-" + std::to_string(index);
  model.capabilities =
      mr::CapabilityProfile(model.capability_profile_id, model.capability_generation);
  const auto add = [&model](std::string_view key, mr::CapabilityState state) {
    const bool inserted = model.capabilities.set(
        make_claim(key, state, model.capability_profile_id, model.capability_generation), 256);
    require(inserted, "model capability profile rejected a claim");
  };
  add(mr::capability_keys::text_generation, mr::CapabilityState::VERIFIED);
  add(mr::capability_keys::streaming, mr::CapabilityState::VERIFIED);
  add(mr::capability_keys::structured_output, mr::CapabilityState::VERIFIED);
  return model;
}

[[nodiscard]] mr::BackendDescriptor make_backend(std::uint32_t backend_index,
                                                 const CatalogSpec& spec) {
  const std::uint64_t base = static_cast<std::uint64_t>(backend_index);
  mr::BackendDescriptor backend;
  backend.backend_id = mr::BackendId(base);
  backend.backend_generation = mr::BackendGeneration(1);
  backend.backend_boot = mr::BackendBootId(1);
  backend.backend_registration_generation = mr::BackendRegistrationGeneration(1);
  backend.provider_id = mr::ProviderId(1);
  backend.provider_generation = mr::ProviderGeneration(1);
  backend.endpoint.endpoint_id = mr::EndpointId(base * 1000u + 1u);
  backend.endpoint.endpoint_generation = mr::EndpointGeneration(1);
  backend.endpoint.backend_id = backend.backend_id;
  backend.endpoint.backend_boot = backend.backend_boot;
  backend.endpoint.reference = "127.0.0.1:" + std::to_string(20000u + backend_index);
  backend.endpoint.protocol = "model_router.benchmark";
  backend.endpoint.protocol_version = 1;
  backend.trust_profile_id = mr::TrustProfileId(base);
  backend.trust_generation = mr::TrustGeneration(1);
  backend.trust_domain = mr::TrustDomain::LOCAL;
  backend.locality = mr::LocalityKey("bench/local");
  backend.network_distance = 1;
  backend.capability_profile_id = mr::CapabilityProfileId(base + 1000000u);
  backend.capability_generation = mr::CapabilityGeneration(1);
  backend.compatibility_profile_id = mr::CompatibilityProfileId(base + 2000000u);
  backend.compatibility_generation = mr::CompatibilityGeneration(1);
  backend.offline_capable = true;
  backend.provenance = mr::Provenance::SYNTHETIC;
  backend.registered_at_unix_millis = kNow;
  for (std::uint32_t slot = 0; slot < spec.bindings_per_backend; ++slot) {
    const std::uint64_t model_index =
        ((base - 1u) * static_cast<std::uint64_t>(spec.bindings_per_backend) + slot) %
            static_cast<std::uint64_t>(spec.models) +
        1u;
    mr::ModelBinding binding;
    binding.model_id = mr::ModelId(model_index);
    binding.model_generation = mr::ModelGeneration(1);
    binding.artifact_generation = mr::ArtifactGeneration(1);
    binding.bound_at_unix_millis = kNow;
    backend.model_bindings.push_back(binding);
  }
  backend.canonicalize();
  return backend;
}

void publish_backend_evidence(mr::ModelRouter& router, mr::BackendId backend_id,
                              mr::BackendBootId boot) {
  mr::HealthEvidence health;
  health.backend_id = backend_id;
  health.backend_boot = boot;
  health.generation = mr::HealthGeneration(1);
  health.state = mr::HealthState::HEALTHY;
  health.observed_at_unix_millis = kNow;
  health.source = "benchmark-synthetic";
  require(router.update_health(health).accepted(), "update_health was rejected");

  mr::AvailabilityEvidence availability;
  availability.backend_id = backend_id;
  availability.backend_boot = boot;
  availability.generation = mr::AvailabilityGeneration(1);
  availability.state = mr::AvailabilityState::AVAILABLE;
  availability.observed_at_unix_millis = kNow;
  availability.source = "benchmark-synthetic";
  require(router.update_availability(availability).accepted(), "update_availability was rejected");

  mr::ReadinessEvidence readiness;
  readiness.backend_id = backend_id;
  readiness.backend_boot = boot;
  readiness.generation = mr::ReadinessGeneration(1);
  readiness.state = mr::ReadinessState::READY;
  readiness.observed_at_unix_millis = kNow;
  readiness.source = "benchmark-synthetic";
  require(router.update_readiness(readiness).accepted(), "update_readiness was rejected");

  mr::ResidencyEvidence residency;
  residency.backend_id = backend_id;
  residency.backend_boot = boot;
  residency.generation = mr::ResidencyGeneration(1);
  residency.state = mr::ResidencyState::RESIDENT;
  residency.observed_at_unix_millis = kNow;
  residency.source = "benchmark-synthetic";
  require(router.update_residency(residency).accepted(), "update_residency was rejected");

  mr::CapacityEvidence capacity;
  capacity.backend_id = backend_id;
  capacity.backend_boot = boot;
  capacity.generation = mr::CapacityGeneration(1);
  capacity.state = mr::CapacityState::AVAILABLE;
  capacity.observed_at_unix_millis = kNow;
  capacity.source = "benchmark-synthetic";
  mr::CapacityDetail detail;
  detail.backend_id = backend_id;
  detail.backend_boot = boot;
  detail.available_slots = 8;
  detail.total_slots = 16;
  detail.queue_depth = 0;
  detail.max_queue_depth = 32;
  detail.observed_at_unix_millis = kNow;
  require(router.update_capacity(capacity, detail).accepted(), "update_capacity was rejected");

  mr::LatencyEvidence latency;
  latency.health_generation = mr::HealthGeneration(1);
  latency.backend_id = backend_id;
  latency.backend_boot = boot;
  latency.dispatch_micros = 1000;
  latency.time_to_first_token_micros = 500;
  latency.completed_micros = 2000;
  latency.tail_micros = 3000;
  latency.queue_micros = 100;
  latency.warm_start_penalty_micros = 0;
  latency.cold_start_penalty_micros = 0;
  latency.sample_count = 16;
  latency.observed_at_unix_millis = kNow;
  latency.source = "benchmark-synthetic";
  require(router.update_latency(latency).accepted(), "update_latency was rejected");
}

// ---------------------------------------------------------------------------
// Router construction
// ---------------------------------------------------------------------------

struct BuiltRouter {
  std::shared_ptr<mr::LogicalClock> clock;
  std::unique_ptr<mr::ModelRouter> router;
  mr::PolicyId policy_id{};
  mr::PolicyGeneration policy_generation{};
  mr::TenantId tenant{};
  mr::NamespaceId name_space{};
  double registration_millis{0.0};
};

[[nodiscard]] BuiltRouter build_router(const CatalogSpec& spec, const mr::ResourceLimits& limits) {
  require(spec.bindings_per_backend <= spec.models,
          "a backend cannot bind more models than the catalog declares");
  require(limits.validate().empty(), "resource limits are incoherent: " + limits.validate());

  BuiltRouter built;
  built.clock = std::make_shared<mr::LogicalClock>();
  mr::ModelRouterOptions options;
  options.clock = built.clock;
  options.limits = limits;
  built.router = std::make_unique<mr::ModelRouter>(std::move(options));
  built.policy_id = mr::PolicyId(1);
  built.policy_generation = mr::PolicyGeneration(1);
  built.tenant = mr::TenantId(1);
  built.name_space = mr::NamespaceId(1);

  const SteadyClock::time_point start = SteadyClock::now();
  require(built.router->start().accepted(), "router start was rejected");

  mr::ProviderDescriptor provider;
  provider.provider_id = mr::ProviderId(1);
  provider.generation = mr::ProviderGeneration(1);
  provider.display_name = "bench-provider";
  provider.default_trust_domain = mr::TrustDomain::LOCAL;
  provider.third_party = false;
  require(built.router->register_provider(provider).accepted(), "register_provider was rejected");

  for (std::uint32_t index = 1; index <= spec.models; ++index) {
    require(built.router->register_model(make_model(index)).accepted(),
            "register_model was rejected");
  }
  for (std::uint32_t index = 1; index <= spec.backends; ++index) {
    const mr::BackendDescriptor backend = make_backend(index, spec);
    const mr::BackendBootId boot = backend.backend_boot;
    require(built.router->register_backend(backend).accepted(), "register_backend was rejected");
    publish_backend_evidence(*built.router, mr::BackendId(index), boot);
  }

  mr::PolicyBuilder policy(built.policy_id, built.policy_generation);
  policy.fallback_policy(mr::FallbackPolicy::PERMITTED).max_fallback_depth(4);
  require(built.router->set_policy(policy.build()).accepted(), "set_policy was rejected");

  const SteadyClock::time_point finish = SteadyClock::now();
  built.registration_millis = std::chrono::duration<double, std::milli>(finish - start).count();
  return built;
}

/// The retry policy is the only requirement that must match the configured
/// max_fallback_candidates limit; it is not an input to discovery, eligibility,
/// or factor construction, so variant requests stay comparable.
[[nodiscard]] mr::RouteRequest make_request(const BuiltRouter& built,
                                            std::uint32_t max_fallbacks = 2) {
  mr::RouteRequest request;
  request.request_id = mr::allocate_id<mr::RouteRequestTag>();
  request.request_generation = mr::RouteRequestGeneration(1);
  request.tenant = built.tenant;
  request.name_space = built.name_space;
  request.policy_id = built.policy_id;
  request.policy_generation = built.policy_generation;
  request.created_at_unix_millis = kNow;
  request.estimated_input_tokens = 1000;
  request.estimated_output_tokens = 500;
  request.requirements.retry_policy.max_fallbacks = max_fallbacks;
  request.requirements.required_input_modalities = text_modalities();
  request.requirements.required_output_modalities = text_modalities();
  return request;
}

[[nodiscard]] mr::RouteRequest make_ineligible_request(const BuiltRouter& built) {
  mr::RouteRequest request = make_request(built);
  // No model in this catalog declares an IMAGE input modality, so every
  // discovered candidate is rejected by the modality predicate.
  request.requirements.required_input_modalities =
      mr::ModalitySet(static_cast<std::uint32_t>(mr::Modality::IMAGE));
  return request;
}

[[nodiscard]] mr::RouteRequest make_invalid_request(const BuiltRouter& built) {
  mr::RouteRequest request = make_request(built);
  request.requirements.required_input_modalities = mr::ModalitySet();
  request.requirements.required_output_modalities = mr::ModalitySet();
  return request;
}

[[nodiscard]] std::vector<mr::RouteRequestId> make_request_ids(std::uint64_t count) {
  std::vector<mr::RouteRequestId> ids;
  ids.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index) {
    ids.push_back(mr::allocate_id<mr::RouteRequestTag>());
  }
  return ids;
}

// ---------------------------------------------------------------------------
// Guards
// ---------------------------------------------------------------------------

void guard_invariants(const mr::ModelRouter& router, const std::string& label) {
  const mr::InvariantReport report = router.check_invariants();
  if (!report.ok()) {
    std::cerr << report.to_text();
    fail("invariant guard failed after " + label);
  }
  require(report.checks_run > 0, "invariant scan ran no checks after " + label);
}

/// The catalog gives every candidate identical evidence, so the deterministic
/// tie-break (canonical factor sequence, then canonical candidate identity)
/// must select the canonically smallest (model, backend) pair.
void guard_routed(const mr::RouteOutcome& outcome, std::uint64_t expected_eligible,
                  const std::string& label) {
  require(outcome.code == mr::OutcomeCode::ROUTED, "expected a routed decision in " + label +
                                                       ", got " +
                                                       std::string(mr::to_string(outcome.code)));
  require(outcome.has_decision, "routed outcome carries no decision in " + label);
  const mr::RouteDecision& decision = outcome.decision;
  require(decision.explanation.ranking.size() ==
              static_cast<std::size_t>(expected_eligible),
          "eligible candidate count is not the expected candidate-set size in " + label);
  require(decision.explanation.eligible_candidate_count == expected_eligible,
          "explanation eligible count disagrees in " + label);
  require(!decision.explanation.ranking.empty(), "ranking is empty in " + label);
  require(decision.explanation.ranking.front().key == decision.explanation.winner,
          "winner is not the top-ranked candidate in " + label);
  require(decision.authority.model_id == mr::ModelId(1) &&
              decision.authority.backend_id == mr::BackendId(1),
          "winner is not the canonically smallest candidate identity in " + label);
  require(decision.authority.model_id == decision.explanation.winner.model_id &&
              decision.authority.backend_id == decision.explanation.winner.backend_id,
          "bound authority disagrees with the winner in " + label);
}

/// Routing must be deterministic: the same normalized requirements against the
/// same canonical state must always select the same winner and the same number
/// of eligible candidates.
void guard_determinism(mr::ModelRouter& router, const mr::RouteRequest& request,
                       std::uint64_t expected_eligible, const std::string& label) {
  const std::vector<mr::RouteRequestId> ids = make_request_ids(2);
  mr::RouteRequest first_request = request;
  first_request.request_id = ids[0];
  mr::RouteRequest second_request = request;
  second_request.request_id = ids[1];
  const mr::RouteOutcome first = router.route(first_request);
  const mr::RouteOutcome second = router.route(second_request);
  guard_routed(first, expected_eligible, label + " (first)");
  guard_routed(second, expected_eligible, label + " (second)");
  require(first.decision.explanation.winner == second.decision.explanation.winner,
          "identical requirements selected different winners in " + label);
  require(first.decision.explanation.ranking.size() == second.decision.explanation.ranking.size(),
          "identical requirements produced different eligible sets in " + label);
}

/// Measures the cost of a request whose backend allowlist names every backend.
/// RouteRequirements::canonicalize() sorts that list, but hard eligibility
/// scans it linearly once per candidate, so the overhead should grow with
/// candidates x backends. Baselines are measured immediately before and after
/// the probe so allocator and cache drift cannot masquerade as the overhead.
void measure_allowlist_overhead(mr::ModelRouter& router, const mr::RouteRequest& request_template,
                                std::uint32_t backend_count, std::uint64_t expected_eligible,
                                std::uint64_t ops, const MetricContext& context) {
  // The router must have been built with max_allowlist_entries >= backend_count,
  // otherwise the probe request itself is rejected before any candidate is seen.
  if (backend_count == 0 || ops == 0) {
    return;
  }
  const std::vector<mr::RouteRequestId> ids = make_request_ids(2 * ops + 2);
  mr::RouteRequest probe = request_template;
  probe.requirements.backend_allowlist.reserve(backend_count);
  for (std::uint32_t index = 1; index <= backend_count; ++index) {
    probe.requirements.backend_allowlist.push_back(mr::BackendId(index));
  }

  const auto run_baseline = [&](std::uint64_t offset) {
    return measure_ops(ops, [&](std::uint64_t index) {
      mr::RouteRequest request = request_template;
      request.request_id = ids[static_cast<std::size_t>(index + offset)];
      const mr::RouteOutcome outcome = router.route(request);
      guard_routed(outcome, expected_eligible, "allowlist baseline");
    });
  };
  const auto run_probe = [&]() {
    return measure_ops(ops, [&](std::uint64_t index) {
      mr::RouteRequest request = probe;
      request.request_id = ids[static_cast<std::size_t>(index)];
      const mr::RouteOutcome outcome = router.route(request);
      guard_routed(outcome, expected_eligible, "allowlist probe");
    });
  };

  const Timing before = run_baseline(0);
  const Timing measured = run_probe();
  const Timing after = run_baseline(ops);
  Timing baseline;
  baseline.ops = before.ops;
  baseline.ns_per_op = (before.ns_per_op + after.ns_per_op) / 2.0;
  baseline.ops_per_sec = baseline.ns_per_op > 0.0 ? 1.0e9 / baseline.ns_per_op : 0.0;
  print_metric("allowlist_route_baseline", context, baseline);
  print_metric("allowlist_route_overhead", context, subtract(measured, baseline));

  const Timing digest = measure_ops(std::max<std::uint64_t>(1, ops * 4), [&](std::uint64_t) {
    const std::string value = probe.digest();
    require(!value.empty(), "request digest is empty");
  });
  print_metric("request_requirements_digest", context, digest);
}

void guard_rejected_all(const mr::RouteOutcome& outcome, std::uint64_t expected_rejected,
                        const std::string& label) {
  require(outcome.code == mr::OutcomeCode::NO_ELIGIBLE_CANDIDATE,
          "expected every candidate to be hard-rejected in " + label);
  require(outcome.route.rejected_candidate_count == expected_rejected,
          "rejection count is not the candidate-set size in " + label);
  require(outcome.route.ranking.empty(), "an ineligible candidate was ranked in " + label);
  require(!outcome.has_decision, "a decision was committed without an eligible candidate in " +
                                     label);
}

void guard_limit_stop(const mr::RouteOutcome& outcome, const std::string& label) {
  require(outcome.code == mr::OutcomeCode::REJECT_LIMIT,
          "expected the configured limit to stop the pipeline in " + label);
}

// ---------------------------------------------------------------------------
// Findings collected for the closing summary
// ---------------------------------------------------------------------------

struct Sample {
  std::uint64_t scale{0};
  double value{0.0};
};

std::vector<Sample> g_setup_backends_per_sec;
std::vector<Sample> g_candidate_route_ns_per_op;
std::vector<Sample> g_catalog_route_ns_per_op;
std::vector<Sample> g_revalidate_ns_per_op;
std::vector<Sample> g_invariant_ns_per_op;
std::vector<std::string> g_findings;
std::uint64_t g_core_defects = 0;

void add_finding(const std::string& text) {
  if (std::find(g_findings.begin(), g_findings.end(), text) == g_findings.end()) {
    g_findings.push_back(text);
  }
}

/// Chooses an iteration count that keeps one measurement inside a wall-time
/// budget, so difference-based measurements are not swamped by noise.
[[nodiscard]] std::uint64_t ops_for_budget(double ns_per_op, double budget_ns) {
  if (ns_per_op <= 0.0) {
    return 1;
  }
  const double wanted = budget_ns / ns_per_op;
  if (wanted < 1.0) {
    return 1;
  }
  if (wanted > 5000.0) {
    return 5000;
  }
  return static_cast<std::uint64_t>(wanted);
}

// ---------------------------------------------------------------------------
// Durable state round-trip
// ---------------------------------------------------------------------------

/// Reports the two header fields that decide the header checksum, using the
/// documented layout of PersistenceHeader: header_crc32c at byte 36 and the
/// 32-byte semantic_sha256 at bytes 40..71.
[[nodiscard]] std::string describe_state_header(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return "state file unreadable";
  }
  std::vector<unsigned char> header(mr::persistence_header_bytes, 0);
  input.read(reinterpret_cast<char*>(header.data()),
             static_cast<std::streamsize>(header.size()));
  if (input.gcount() != static_cast<std::streamsize>(header.size())) {
    return "state file shorter than its header";
  }
  const auto u32 = [&header](std::size_t at) {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      value |= static_cast<std::uint32_t>(header[at + static_cast<std::size_t>(shift / 8)])
               << shift;
    }
    return value;
  };
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  out << "header_crc32c_field_at_36=0x" << std::setw(8) << u32(36);
  out << " bytes_64_67=0x" << std::setw(2) << static_cast<unsigned int>(header[64])
      << std::setw(2) << static_cast<unsigned int>(header[65]) << std::setw(2)
      << static_cast<unsigned int>(header[66]) << std::setw(2)
      << static_cast<unsigned int>(header[67]);
  out << std::dec;
  return out.str();
}

void report_core_defect(const std::string& what, const std::string& detail,
                        const std::string& header_dump) {
  ++g_core_defects;
  std::cout << "# CORE DEFECT: " << what << "\n";
  std::cout << "#   observed: " << detail << "\n";
  std::cout << "#   state header: " << header_dump << "\n";
  std::cout << "#   RouterStateStore::encode computes the header CRC over all 72 header bytes "
               "and stores it at byte 64, inside the 32-byte semantic_sha256 field (byte 40), "
               "leaving the documented header_crc32c field at byte 36 zero. decode() recomputes "
               "over the full header and compares against byte 36, so it always rejects a file "
               "this library just wrote.\n";
  std::cout << "#   consequence: save() is one-way; load(), validate_file(), and "
               "load_on_start are unusable.\n";
}

void run_persistence_phase(mr::ModelRouter& saver, const mr::ResourceLimits& limits,
                           const MetricContext& context, std::uint64_t expected_models,
                           std::uint64_t expected_backends) {
  StateFileGuard guard("model_router_bench_state.bin");

  // Production durability: encode, write a sibling temporary file, flush it to
  // the device (_commit/fsync equivalent), and rename it over the target.
  const Timing save = measure_ops(1, [&](std::uint64_t) {
    const mr::PersistenceResult result = saver.save(guard.path());
    require(result.ok(), "durable save failed: " + result.detail);
  });
  print_metric("persistence_save", context, save);

  // Production recovery: read the file back and validate it end to end.
  std::shared_ptr<mr::LogicalClock> loader_clock = std::make_shared<mr::LogicalClock>();
  mr::ModelRouterOptions loader_options;
  loader_options.clock = loader_clock;
  loader_options.limits = limits;
  mr::ModelRouter loader(std::move(loader_options));

  mr::PersistenceResult load_result;
  const Timing load = measure_ops(1, [&](std::uint64_t) {
    load_result = loader.load(guard.path());
  });
  print_metric("persistence_load", context, load);

  if (!load_result.ok()) {
    note("persistence_load above is the rejection path: the header check fails before any "
         "payload or record validation runs, so a full-integrity load cannot be measured until "
         "the defect below is fixed");
    report_core_defect("durable save/load round-trip",
                       "save() was accepted but load() returned " +
                           std::string(mr::to_string(load_result.code)) + " (" +
                           load_result.detail + ")",
                       describe_state_header(guard.path()));
    add_finding("durable round-trip is broken: save() accepted, load() rejected with '" +
                load_result.detail + "'");
    return;
  }

  guard_invariants(loader, "durable load");
  const mr::RouterSummary summary = loader.summary();
  require(summary.backend_count == expected_backends, "loaded backend count disagrees");
  require(summary.model_count == expected_models, "loaded model count disagrees");
}

// ---------------------------------------------------------------------------
// Section 1: candidate-set scaling for a single request
// ---------------------------------------------------------------------------

[[nodiscard]] std::uint64_t candidate_scale_ops(std::uint64_t candidates) {
  if (candidates <= 10) {
    return 2000;
  }
  if (candidates <= 100) {
    return 500;
  }
  if (candidates <= 1000) {
    return 50;
  }
  return 2;
}

void run_candidate_scale(std::uint64_t target_candidates) {
  const std::uint32_t models =
      static_cast<std::uint32_t>(std::min<std::uint64_t>(10, target_candidates));
  const std::uint32_t backends = static_cast<std::uint32_t>(target_candidates / models);
  CatalogSpec spec;
  spec.models = models;
  spec.backends = backends;
  spec.bindings_per_backend = models;
  require(spec.candidate_count() == target_candidates, "candidate-set size was not realized");

  // Full pipeline limits. max_ranked_candidates is raised above its 4096 default
  // so that the largest candidate set can actually be ranked and committed.
  mr::ResourceLimits full_limits;
  full_limits.max_candidates_per_request = 100000;
  full_limits.max_ranked_candidates = 100000;
  full_limits.max_rejection_records = 100000;
  // Raised so the allowlist probe can name every backend without the request
  // being rejected by the allowlist bound.
  full_limits.max_allowlist_entries = 100000;
  full_limits.max_route_history = 8;

  // Stops route() immediately after candidate discovery.
  mr::ResourceLimits discovery_limits = full_limits;
  discovery_limits.max_candidates_per_request = 1;
  discovery_limits.max_ranked_candidates = 1;
  discovery_limits.max_fallback_candidates = 1;
  discovery_limits.max_rejection_records = 1;

  // Stops route() immediately after factor construction, before ranking.
  mr::ResourceLimits factor_limits = full_limits;
  factor_limits.max_ranked_candidates = 1;
  factor_limits.max_fallback_candidates = 1;

  const BuiltRouter full = build_router(spec, full_limits);
  const BuiltRouter discovery = build_router(spec, discovery_limits);
  const BuiltRouter factors = build_router(spec, factor_limits);

  MetricContext context;
  context.scale = target_candidates;
  context.models = models;
  context.backends = backends;
  context.candidates = target_candidates;

  note("candidate-set scale " + std::to_string(target_candidates) + ": models=" +
       std::to_string(models) + " backends=" + std::to_string(backends) +
       " bindings_per_backend=" + std::to_string(models) + " setup_ms=" +
       std::to_string(full.registration_millis));

  Timing setup;
  setup.ops = 1;
  setup.ns_per_op = full.registration_millis * 1.0e6;
  setup.ops_per_sec = full.registration_millis > 0.0 ? 1000.0 / full.registration_millis : 0.0;
  print_metric("catalog_registration_setup", context, setup);

  const std::uint64_t ops = candidate_scale_ops(target_candidates);
  const std::vector<mr::RouteRequestId> ids = make_request_ids(ops + 8);
  const mr::RouteRequest eligible_request = make_request(full);
  const mr::RouteRequest ineligible_request = make_ineligible_request(full);
  const mr::RouteRequest invalid_request = make_invalid_request(full);
  // The early-stop variants cap max_fallback_candidates at 1, so their requests
  // must not ask for more fallbacks than the limit allows.
  const mr::RouteRequest discovery_request = make_request(discovery, 1);
  const mr::RouteRequest factor_request = make_request(factors, 1);

  // Admission baseline: rejected before discovery, so it measures only the
  // request copy, canonicalization, validation, and state snapshot.
  const Timing admission = measure_ops(ops, [&](std::uint64_t index) {
    mr::RouteRequest request = invalid_request;
    request.request_id = ids[index];
    const mr::RouteOutcome outcome = full.router->route(request);
    require(outcome.code == mr::OutcomeCode::REJECT_INVALID,
            "invalid request was not rejected during admission");
  });
  print_metric("route_admission_baseline", context, admission);

  // Candidate discovery only.
  const Timing until_discovery = measure_ops(ops, [&](std::uint64_t index) {
    mr::RouteRequest request = discovery_request;
    request.request_id = ids[index];
    const mr::RouteOutcome outcome = discovery.router->route(request);
    if (target_candidates > 1) {
      guard_limit_stop(outcome, "discovery-only variant");
    }
  });
  print_metric("route_until_discovery", context, until_discovery);

  // Hard eligibility filtering: identical discovery, every candidate rejected.
  const Timing until_filter = measure_ops(ops, [&](std::uint64_t index) {
    mr::RouteRequest request = ineligible_request;
    request.request_id = ids[index];
    const mr::RouteOutcome outcome = full.router->route(request);
    guard_rejected_all(outcome, target_candidates, "hard-eligibility phase");
  });
  print_metric("route_until_hard_filter", context, until_filter);

  // Factor construction only.
  const Timing until_factors = measure_ops(ops, [&](std::uint64_t index) {
    mr::RouteRequest request = factor_request;
    request.request_id = ids[index];
    const mr::RouteOutcome outcome = factors.router->route(request);
    if (target_candidates > 1) {
      guard_limit_stop(outcome, "factor-construction variant");
    }
  });
  print_metric("route_until_factor_construction", context, until_factors);

  // Complete pipeline: discovery, filtering, factor construction, deterministic
  // ranking, route-plan construction, and the route-decision commit.
  mr::RouteOutcome last_outcome;
  const Timing completed = measure_ops(ops, [&](std::uint64_t index) {
    mr::RouteRequest request = eligible_request;
    request.request_id = ids[index];
    mr::RouteOutcome outcome = full.router->route(request);
    guard_routed(outcome, target_candidates, "completed route");
    last_outcome = std::move(outcome);
  });
  print_metric("route_pipeline_completed", context, completed);

  if (target_candidates > 1) {
    const Timing discovery_net = subtract(until_discovery, admission);
    const Timing filter_net = subtract(until_filter, until_discovery);
    const Timing factor_net = subtract(until_factors, until_filter);
    const Timing ranking_commit_net = subtract(completed, until_factors);
    print_metric("candidate_discovery_net", context, discovery_net);
    print_metric("hard_eligibility_filter_net", context, filter_net);
    print_metric("factor_construction_net", context, factor_net);
    print_metric("ranking_and_commit_net", context, ranking_commit_net);
    if (factor_net.ns_per_op < 0.0) {
      note("factor_construction_net is negative at this scale: the all-ineligible baseline "
           "builds rejection records and copies the explanation into the outcome, which costs "
           "more than the factor vectors it replaces, so the difference is not a lower bound "
           "for tiny candidate sets");
    }
  } else {
    note("phase decomposition needs at least two candidates; scale 1 reports the total only");
  }

  require(last_outcome.has_decision, "completed route produced no decision");
  const mr::RouteDecisionId decision_id = last_outcome.decision.decision_id;

  // Pre-dispatch revalidation.
  const std::uint64_t revalidate_ops = std::max<std::uint64_t>(1, ops / 2);
  const Timing revalidate = measure_ops(revalidate_ops, [&](std::uint64_t) {
    const mr::MutationResult result = full.router->revalidate(decision_id);
    require(result.accepted(), "pre-dispatch revalidation rejected a current decision");
  });
  print_metric("pre_dispatch_revalidation", context, revalidate);
  g_revalidate_ns_per_op.push_back(Sample{target_candidates, revalidate.ns_per_op});

  // Explanation generation.
  const Timing explain = measure_ops(ops, [&](std::uint64_t) {
    const std::string text = full.router->explain_route(decision_id);
    require(!text.empty(), "explain_route produced no text");
  });
  print_metric("explanation_explain_route", context, explain);

  const mr::RouteExplanation explanation = last_outcome.decision.explanation;
  const Timing to_text = measure_ops(ops, [&](std::uint64_t) {
    const std::string text = explanation.to_text();
    require(!text.empty(), "RouteExplanation::to_text produced no text");
  });
  print_metric("explanation_to_text", context, to_text);

  const Timing digest = measure_ops(ops, [&](std::uint64_t) {
    const std::string value = last_outcome.decision.semantic_digest();
    require(!value.empty(), "decision semantic digest is empty");
  });
  print_metric("decision_semantic_digest", context, digest);

  // Snapshot.
  const std::uint64_t snapshot_ops = std::max<std::uint64_t>(1, ops / 4);
  const Timing snapshot = measure_ops(snapshot_ops, [&](std::uint64_t) {
    const mr::RouterSnapshot value = full.router->snapshot();
    require(value.backend_count == backends, "snapshot backend count disagrees");
  });
  print_metric("snapshot", context, snapshot);

  // Invariant scan over a bounded, populated route history.
  for (std::uint64_t index = 0; index < 8; ++index) {
    mr::RouteRequest request = eligible_request;
    request.request_id = ids[static_cast<std::size_t>(index)];
    (void)full.router->route(request);
  }
  const Timing invariants = measure_ops(1, [&](std::uint64_t) {
    const mr::InvariantReport report = full.router->check_invariants();
    require(report.ok(), "invariant scan reported a violation");
  });
  print_metric("invariant_scan", context, invariants);
  g_invariant_ns_per_op.push_back(Sample{target_candidates, invariants.ns_per_op});

  // Durable save and load. save() writes a sibling temporary file, flushes it to
  // the device, and renames it over the target; load() reads the file back and
  // performs the full integrity validation.
  run_persistence_phase(*full.router, full_limits, context, models, backends);

  // Linear scans over canonical allowlists. The request allowlist is sorted by
  // canonicalize(), but eligibility scans it linearly for every candidate.
  if (target_candidates >= 1000) {
    measure_allowlist_overhead(*full.router, eligible_request, backends, target_candidates,
                               ops_for_budget(completed.ns_per_op, 5.0e8), context);
  }

  guard_determinism(*full.router, eligible_request, target_candidates, "candidate-scale route");
  guard_invariants(*full.router, "candidate-scale measurement");

  g_candidate_route_ns_per_op.push_back(Sample{target_candidates, completed.ns_per_op});
}

void run_candidate_scale_section(bool quick) {
  std::cout << "\n";
  note("SECTION 1 - candidate-set scaling for ONE request "
       "(scale = candidates discovered for that request)");
  std::vector<std::uint64_t> targets{1, 10, 100, 1000};
  if (!quick) {
    targets.push_back(10000);
  }
  for (const std::uint64_t target : targets) {
    run_candidate_scale(target);
  }
}

// ---------------------------------------------------------------------------
// Section 2: catalog scaling (models and backends)
// ---------------------------------------------------------------------------

struct CatalogCase {
  std::uint32_t models;
  std::uint32_t backends;
};

void run_catalog_case(const CatalogCase& test_case) {
  CatalogSpec spec;
  spec.models = test_case.models;
  spec.backends = test_case.backends;
  spec.bindings_per_backend = 1;

  mr::ResourceLimits limits;
  limits.max_candidates_per_request = 100000;
  limits.max_ranked_candidates = 100000;
  limits.max_rejection_records = 100000;
  limits.max_allowlist_entries = 100000;
  limits.max_route_history = 4;

  const BuiltRouter built = build_router(spec, limits);

  MetricContext context;
  context.scale = test_case.backends;
  context.models = test_case.models;
  context.backends = test_case.backends;
  context.candidates = spec.candidate_count();

  note("catalog scale models=" + std::to_string(test_case.models) +
       " backends=" + std::to_string(test_case.backends) +
       " candidates_per_request=" + std::to_string(spec.candidate_count()) +
       " setup_ms=" + std::to_string(built.registration_millis));

  Timing setup;
  setup.ops = 1;
  setup.ns_per_op = built.registration_millis * 1.0e6;
  setup.ops_per_sec = built.registration_millis > 0.0 ? 1000.0 / built.registration_millis : 0.0;
  print_metric("catalog_registration_setup", context, setup);
  g_setup_backends_per_sec.push_back(
      Sample{test_case.backends,
             built.registration_millis > 0.0
                 ? static_cast<double>(test_case.backends) * 1000.0 / built.registration_millis
                 : 0.0});

  const std::uint64_t ops = test_case.backends <= 100 ? 5 : (test_case.backends <= 1000 ? 3 : 1);
  const std::vector<mr::RouteRequestId> ids = make_request_ids(ops + 8);
  const mr::RouteRequest eligible_request = make_request(built);
  const mr::RouteRequest ineligible_request = make_ineligible_request(built);

  const Timing completed = measure_ops(ops, [&](std::uint64_t index) {
    mr::RouteRequest request = eligible_request;
    request.request_id = ids[index];
    const mr::RouteOutcome outcome = built.router->route(request);
    guard_routed(outcome, spec.candidate_count(), "catalog-scale completed route");
  });
  print_metric("route_pipeline_completed", context, completed);
  g_catalog_route_ns_per_op.push_back(Sample{spec.candidate_count(), completed.ns_per_op});

  const Timing filtered = measure_ops(ops, [&](std::uint64_t index) {
    mr::RouteRequest request = ineligible_request;
    request.request_id = ids[index];
    const mr::RouteOutcome outcome = built.router->route(request);
    guard_rejected_all(outcome, spec.candidate_count(), "catalog-scale hard filter");
  });
  print_metric("route_until_hard_filter", context, filtered);

  // Backend lookup: a canonical map lookup that must stay logarithmic.
  const Timing lookup = measure_ops(200000, [&](std::uint64_t index) {
    const std::uint64_t backend_index = index % test_case.backends + 1;
    mr::BackendDescriptor descriptor;
    const bool found = built.router->find_backend(mr::BackendId(backend_index), &descriptor);
    require(found, "find_backend did not find a registered backend");
    require(descriptor.backend_id == mr::BackendId(backend_index),
            "find_backend returned the wrong backend");
  });
  print_metric("backend_lookup", context, lookup);

  const Timing snapshot = measure_ops(1, [&](std::uint64_t) {
    const mr::RouterSnapshot value = built.router->snapshot();
    require(value.backend_count == test_case.backends, "snapshot backend count disagrees");
    require(value.model_count == test_case.models, "snapshot model count disagrees");
  });
  print_metric("snapshot", context, snapshot);

  for (std::uint64_t index = 0; index < 4; ++index) {
    mr::RouteRequest request = eligible_request;
    request.request_id = ids[static_cast<std::size_t>(index)];
    (void)built.router->route(request);
  }
  const Timing invariants = measure_ops(1, [&](std::uint64_t) {
    const mr::InvariantReport report = built.router->check_invariants();
    require(report.ok(), "invariant scan reported a violation");
  });
  print_metric("invariant_scan", context, invariants);

  measure_allowlist_overhead(*built.router, eligible_request, test_case.backends,
                             spec.candidate_count(), ops_for_budget(completed.ns_per_op, 5.0e8),
                             context);

  guard_determinism(*built.router, eligible_request, spec.candidate_count(),
                    "catalog-scale route");

  run_persistence_phase(*built.router, limits, context, test_case.models, test_case.backends);

  guard_invariants(*built.router, "catalog-scale measurement");
}

void run_catalog_scale_section(bool quick) {
  std::cout << "\n";
  note("SECTION 2 - catalog scaling (scale = registered backends, one model binding each)");
  std::vector<CatalogCase> cases{{10, 100}, {100, 1000}};
  if (quick) {
    note("--quick: skipping the 1000 models / 10000 backends catalog scale");
  } else {
    cases.push_back(CatalogCase{1000, 10000});
  }
  for (const CatalogCase& test_case : cases) {
    run_catalog_case(test_case);
  }
}

// ---------------------------------------------------------------------------
// Section 3: routing throughput over completed route decisions
// ---------------------------------------------------------------------------

void run_throughput_section(bool quick) {
  std::cout << "\n";
  note("SECTION 3 - routing throughput: completed route decisions per second "
       "(scale = decisions completed)");

  CatalogSpec spec;
  spec.models = 3;
  spec.backends = 4;
  spec.bindings_per_backend = 3;

  mr::ResourceLimits limits;
  limits.max_candidates_per_request = 100000;
  limits.max_ranked_candidates = 100000;
  limits.max_rejection_records = 100000;
  limits.max_route_history = 64;

  const BuiltRouter built = build_router(spec, limits);
  note("throughput catalog: models=" + std::to_string(spec.models) + " backends=" +
       std::to_string(spec.backends) + " candidates_per_request=" +
       std::to_string(spec.candidate_count()) + " setup_ms=" +
       std::to_string(built.registration_millis));

  std::vector<std::uint64_t> scales{1000, 10000};
  if (!quick) {
    scales.push_back(100000);
  }
  for (const std::uint64_t scale : scales) {
    const std::vector<mr::RouteRequestId> ids = make_request_ids(scale);
    const mr::RouteRequest request_template = make_request(built);
    std::uint64_t routed = 0;
    const Timing timing = measure_ops(scale, [&](std::uint64_t index) {
      mr::RouteRequest request = request_template;
      request.request_id = ids[static_cast<std::size_t>(index)];
      const mr::RouteOutcome outcome = built.router->route(request);
      require(outcome.code == mr::OutcomeCode::ROUTED && outcome.has_decision,
              "routing throughput run produced a non-routed outcome");
      require(outcome.decision.authority.model_id == mr::ModelId(1) &&
                  outcome.decision.authority.backend_id == mr::BackendId(1),
              "routing throughput run produced an unexpected winner");
      ++routed;
    });
    require(routed == scale, "completed route-decision count does not match the scale");

    MetricContext context;
    context.scale = scale;
    context.models = spec.models;
    context.backends = spec.backends;
    context.candidates = spec.candidate_count();
    print_metric("routing_throughput_completed_decisions", context, timing);
    std::cout << "routing_throughput_completed_decisions_per_sec=" << std::fixed
              << std::setprecision(2) << timing.ops_per_sec << " scale=" << scale
              << " candidates=" << spec.candidate_count() << "\n";
  }
  guard_invariants(*built.router, "routing throughput");
}

// ---------------------------------------------------------------------------
// Closing summary
// ---------------------------------------------------------------------------

void print_findings() {
  std::cout << "\n";
  note("SECTION 4 - asymptotic observations (core library inspected, never modified)");
  for (const std::string& finding : g_findings) {
    note(finding);
  }
  const auto report_scaling = [](const char* label, const std::vector<Sample>& samples) {
    for (std::size_t index = 1; index < samples.size(); ++index) {
      const Sample& previous = samples[index - 1];
      const Sample& current = samples[index];
      if (previous.scale == 0 || previous.value <= 0.0) {
        continue;
      }
      const double ratio = current.value / previous.value;
      const double growth =
          static_cast<double>(current.scale) / static_cast<double>(previous.scale);
      note(std::string(label) + " cost grew x" + std::to_string(ratio) +
           " when the candidate set grew x" + std::to_string(growth) + " (" +
           std::to_string(previous.scale) + " -> " + std::to_string(current.scale) +
           " candidates)");
    }
  };
  report_scaling("completed route (candidate-set series)", g_candidate_route_ns_per_op);
  report_scaling("completed route (catalog series)", g_catalog_route_ns_per_op);
  const auto report_series = [](const char* label, const std::vector<Sample>& samples) {
    for (const Sample& sample : samples) {
      note(std::string(label) + ": candidates=" + std::to_string(sample.scale) +
           " ns_per_op=" + std::to_string(sample.value));
    }
  };
  report_series("pre-dispatch revalidation", g_revalidate_ns_per_op);
  report_series("invariant scan", g_invariant_ns_per_op);
  for (std::size_t index = 1; index < g_setup_backends_per_sec.size(); ++index) {
    const Sample& previous = g_setup_backends_per_sec[index - 1];
    const Sample& current = g_setup_backends_per_sec[index];
    note("catalog registration throughput " + std::to_string(previous.value) + " -> " +
         std::to_string(current.value) + " backend registrations/sec (" +
         std::to_string(previous.scale) + " -> " + std::to_string(current.scale) + " backends)");
  }
  note("core defects observed: " + std::to_string(g_core_defects));
}

void print_header(bool quick) {
  std::cout << "# Model Router benchmark suite\n";
  std::cout << "# ROUTING THROUGHPUT ONLY. No model inference is performed, no request is "
               "submitted, and no tokens are measured.\n";
  std::cout << "# mode=" << (quick ? "quick" : "full") << "\n";
  std::cout << "# clock=model_router::LogicalClock wall=std::chrono::steady_clock "
               "timeouts=0 sleeps=0\n";
  const mr::ResourceLimits limits;
  std::cout << "# default_limits max_candidates_per_request=" << limits.max_candidates_per_request
            << " max_ranked_candidates=" << limits.max_ranked_candidates
            << " max_route_history=" << limits.max_route_history
            << " max_backends=" << limits.max_backends << "\n";
}

void print_usage() {
  std::cout << "usage: model_router_benchmarks [--quick] [--help]\n"
               "  (no arguments)  run every scale, including 1000 models / 10000 backends\n"
               "  --quick         run only the fast scales\n"
               "  --help          print this message\n";
}

}  // namespace

int main(int argc, char** argv) {
  bool quick = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--quick") {
      quick = true;
    } else if (argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    } else {
      std::cerr << "unknown argument: " << argument << "\n";
      print_usage();
      return 2;
    }
  }

  try {
    print_header(quick);
    run_candidate_scale_section(quick);
    run_catalog_scale_section(quick);
    run_throughput_section(quick);
    print_findings();
    if (g_core_defects != 0) {
      std::cout << "\n# benchmark finished, but " << g_core_defects
                << " core-library defect(s) were observed above. Exiting non-zero.\n";
      return 3;
    }
    std::cout << "\n# benchmark complete: every invariant and decision guard passed\n";
    return 0;
  } catch (const BenchmarkFailure& failure) {
    std::cerr << "\nGUARD FAILURE: " << failure.message << "\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "\nUNEXPECTED FAILURE: " << error.what() << "\n";
    return 1;
  }
}
