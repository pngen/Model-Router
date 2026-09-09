// Model Router - real CUDA backend proof and boot-bound capability evidence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// These tests are registered only when the test target links the optional CUDA
// probe library (tests/CMakeLists.txt defines MODEL_ROUTER_TEST_HAS_CUDA when
// MODEL_ROUTER_CUDA_FOUND is set). Without that library the probe symbols do
// not exist, so the whole file would fail to link; the guard keeps a
// non-CUDA build green without weakening the CUDA build.

#include <cstdio>
#include <string>
#include <utility>

#include "router_fixture.hpp"

#if defined(MODEL_ROUTER_TEST_HAS_CUDA)

#include "model_router/cuda/cuda_reference_probe.hpp"

using model_router::BackendBootId;
using model_router::BackendGeneration;
using model_router::BackendId;
using model_router::CapabilityGeneration;
using model_router::CapabilityProfileId;
using model_router::CapabilityState;
using model_router::OutcomeCode;
using model_router::PolicyGeneration;
using model_router::PolicyId;
using model_router::RouteOutcome;
using model_router::TenantId;

namespace {

constexpr TenantId kTenant(1);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);
/// Elements transferred in both directions by the probe.
constexpr std::uint32_t kProbeElements = 1u << 20;
/// The reference backend starts with capability generation 1 and an empty
/// profile, so real evidence is published at generation 2.
constexpr CapabilityGeneration kCudaCapabilityGeneration(2);

[[nodiscard]] model_router::CapabilityRequirement require_cuda_execution() {
  model_router::CapabilityRequirement requirement;
  requirement.key =
      model_router::CapabilityKey(std::string(model_router::capability_keys::cuda_execution));
  requirement.minimum_state = CapabilityState::VERIFIED;
  return requirement;
}

/// Builds the capability publication a real CUDA-capable backend incarnation
/// would send: execution/cuda at VERIFIED, bound to one specific boot.
[[nodiscard]] model_router::BackendCapabilityPublication make_cuda_publication(
    BackendId backend_id, BackendBootId boot, CapabilityGeneration generation,
    model_router::UnixMillis now, std::string detail) {
  model_router::BackendCapabilityPublication publication;
  publication.backend_id = backend_id;
  publication.backend_generation = BackendGeneration(1);
  publication.backend_boot = boot;
  publication.profile_id = CapabilityProfileId(backend_id.value() * 100 + 2);
  publication.generation = generation;

  model_router::CapabilityEvidence claim;
  claim.key =
      model_router::CapabilityKey(std::string(model_router::capability_keys::cuda_execution));
  claim.state = CapabilityState::VERIFIED;
  claim.observed_at_unix_millis = now;
  claim.expires_at_unix_millis = model_router::kNoExpiry;
  claim.source = "cuda-device-probe";
  claim.detail = std::move(detail);
  publication.claims.push_back(std::move(claim));
  return publication;
}

[[nodiscard]] std::string probe_summary(const model_router::cuda::DeviceProbe& probe) {
  return "device=" + probe.device_name + " compute_capability=" +
         std::to_string(probe.compute_capability_major) + "." +
         std::to_string(probe.compute_capability_minor) +
         " elements=" + std::to_string(probe.element_count) +
         " parity=" + std::string(probe.parity_verified ? "exact" : "mismatch") +
         " memory_restored=" + std::string(probe.memory_restored ? "true" : "false");
}

[[nodiscard]] RouteOutcome route_requiring_cuda(mrtest::Fixture& fixture, BackendId backend_id) {
  model_router::RouteRequest request =
      mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(require_cuda_execution());
  request.requirements.backend_allowlist.push_back(backend_id);
  return fixture.router->route(request);
}

}  // namespace

MR_TEST(cuda, device_probe_reports_real_device_and_exact_parity) {
  const model_router::cuda::DeviceProbe probe = model_router::cuda::run_device_probe(kProbeElements);

  // This build links the CUDA probe library, so compiled_with_cuda() is true.
  MR_CHECK(model_router::cuda::compiled_with_cuda());
  MR_CHECK(!model_router::cuda::runtime_version().empty());

  // The CUDA build runs on a machine with a real device. A missing device is a
  // hard failure here, never a silent skip.
  MR_CHECK(probe.available);

  // Print the observed probe so the real device evidence is visible in the run.
  std::printf("%s", probe.to_text().c_str());

  MR_CHECK_EQ(probe.element_count, kProbeElements);
  MR_CHECK(probe.device_count >= 1u);
  MR_CHECK(!probe.device_name.empty());
  MR_CHECK(probe.compute_capability_major >= 1u);
  MR_CHECK(probe.total_memory_bytes > 0u);
  MR_CHECK(probe.free_memory_before_bytes > 0u);
  MR_CHECK(probe.free_memory_after_bytes > 0u);
  MR_CHECK(probe.parity_verified);
  MR_CHECK(probe.memory_restored);
  MR_CHECK(probe.result_checksum != 0u);
  MR_CHECK(!probe.detail.empty());

  const std::string json = probe.to_json();
  MR_CHECK(json.find("\"parity_verified\":true") != std::string::npos);
  MR_CHECK(json.find("\"memory_restored\":true") != std::string::npos);
  MR_CHECK(json.find("\"device_name\":\"" + probe.device_name + "\"") != std::string::npos);

  // A zero-element probe is a precise rejection, never a crash.
  const model_router::cuda::DeviceProbe empty = model_router::cuda::run_device_probe(0);
  MR_CHECK(!empty.available);
  MR_CHECK(!empty.detail.empty());
}

MR_TEST(cuda, device_probe_is_repeatable_and_deterministic) {
  const model_router::cuda::DeviceProbe first = model_router::cuda::run_device_probe(kProbeElements);
  const model_router::cuda::DeviceProbe second = model_router::cuda::run_device_probe(kProbeElements);
  MR_CHECK(first.available);
  MR_CHECK(second.available);
  MR_CHECK_EQ(first.device_name, second.device_name);
  MR_CHECK_EQ(first.compute_capability_major, second.compute_capability_major);
  MR_CHECK_EQ(first.compute_capability_minor, second.compute_capability_minor);
  MR_CHECK_EQ(first.total_memory_bytes, second.total_memory_bytes);
  MR_CHECK_EQ(first.element_count, second.element_count);
  MR_CHECK_EQ(first.result_checksum, second.result_checksum);
  MR_CHECK(second.parity_verified);
  MR_CHECK(second.memory_restored);

  // A different element range is a different, still deterministic, result.
  const model_router::cuda::DeviceProbe smaller =
      model_router::cuda::run_device_probe(kProbeElements / 4u);
  MR_CHECK(smaller.available);
  MR_CHECK(smaller.parity_verified);
  MR_CHECK(smaller.memory_restored);
  MR_CHECK_EQ(smaller.element_count, kProbeElements / 4u);
  MR_CHECK_NE(smaller.result_checksum, first.result_checksum);
}

MR_TEST(cuda, verified_device_probe_makes_a_backend_eligible) {
  const model_router::cuda::DeviceProbe probe = model_router::cuda::run_device_probe(kProbeElements);
  MR_CHECK(probe.available);
  MR_CHECK(probe.parity_verified);

  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());

  const BackendId backend_id(1);
  const BackendBootId boot(1);
  fixture.add_backend(backend_id, boot.value());

  // Without the CUDA claim the backend cannot satisfy the requirement.
  const RouteOutcome before = route_requiring_cuda(fixture, backend_id);
  MR_CHECK_EQ(before.code, OutcomeCode::NO_ELIGIBLE_CANDIDATE);
  MR_CHECK(!before.route.rejections.empty());
  MR_CHECK_EQ(before.route.rejections.front().code, OutcomeCode::REJECT_CAPABILITY);

  // Publish the real, device-proven capability claim.
  MR_CHECK(fixture.router
               ->publish_capabilities(make_cuda_publication(backend_id, boot,
                                                            kCudaCapabilityGeneration, fixture.now(),
                                                            probe_summary(probe)))
               .accepted());

  model_router::BackendDescriptor descriptor;
  MR_CHECK(fixture.router->find_backend(backend_id, &descriptor));
  const model_router::CapabilityEvidence* evidence = descriptor.capabilities.find(
      model_router::CapabilityKey(std::string(model_router::capability_keys::cuda_execution)));
  MR_CHECK(evidence != nullptr);
  MR_CHECK_EQ(evidence->state, CapabilityState::VERIFIED);
  MR_CHECK_EQ(evidence->backend_boot, boot);

  const RouteOutcome after = route_requiring_cuda(fixture, backend_id);
  MR_CHECK_EQ(after.code, OutcomeCode::ROUTED);
  MR_CHECK(after.has_decision);
  MR_CHECK_EQ(after.decision.authority.backend_id, backend_id);
  MR_CHECK_EQ(after.decision.authority.backend_boot, boot);
  MR_CHECK(!after.route.ranking.empty());
  MR_CHECK(fixture.router->check_invariants().ok());
}

MR_TEST(cuda, fenced_incarnation_must_republish_device_evidence) {
  const model_router::cuda::DeviceProbe probe = model_router::cuda::run_device_probe(kProbeElements);
  MR_CHECK(probe.available);
  MR_CHECK(probe.parity_verified);

  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());

  const BackendId backend_id(1);
  const BackendBootId first_boot(1);
  const BackendBootId second_boot(2);
  fixture.add_backend(backend_id, first_boot.value());
  MR_CHECK(fixture.router
               ->publish_capabilities(make_cuda_publication(backend_id, first_boot,
                                                            kCudaCapabilityGeneration, fixture.now(),
                                                            probe_summary(probe)))
               .accepted());

  const RouteOutcome first_route = route_requiring_cuda(fixture, backend_id);
  MR_CHECK_EQ(first_route.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(first_route.decision.authority.backend_boot, first_boot);

  // Fencing the incarnation retires every claim that incarnation made.
  MR_CHECK(fixture.router
               ->fence_backend_boot(backend_id, BackendGeneration(1), first_boot,
                                    OutcomeCode::REJECT_STALE_BACKEND_BOOT)
               .accepted());
  MR_CHECK(!fixture.router->find_backend(backend_id, nullptr));

  // The old incarnation can never publish evidence again.
  MR_CHECK_EQ(fixture.router
                  ->publish_capabilities(make_cuda_publication(
                      backend_id, first_boot, model_router::CapabilityGeneration(3), fixture.now(),
                      probe_summary(probe)))
                  .code,
              OutcomeCode::REJECT_STALE_BACKEND_BOOT);

  // A fresh incarnation registers and publishes its live evidence, but the
  // device capability claim belongs to the previous boot and is gone.
  model_router::BackendDescriptor backend = model_router::reference::make_backend(
      fixture.catalog, backend_id, BackendGeneration(1), second_boot,
      model_router::BackendRegistrationGeneration(2), "127.0.0.1:7001",
      model_router::Provenance::SYNTHETIC, fixture.now());
  MR_CHECK(fixture.router->register_backend(std::move(backend)).accepted());
  fixture.publish_current(backend_id, second_boot);

  const RouteOutcome unrepaired = route_requiring_cuda(fixture, backend_id);
  MR_CHECK_EQ(unrepaired.code, OutcomeCode::NO_ELIGIBLE_CANDIDATE);
  MR_CHECK(!unrepaired.route.rejections.empty());
  MR_CHECK_EQ(unrepaired.route.rejections.front().code, OutcomeCode::REJECT_CAPABILITY);

  // The fresh incarnation must republish device evidence in its own name.
  MR_CHECK(fixture.router
               ->publish_capabilities(make_cuda_publication(backend_id, second_boot,
                                                            kCudaCapabilityGeneration, fixture.now(),
                                                            probe_summary(probe)))
               .accepted());

  const RouteOutcome repaired = route_requiring_cuda(fixture, backend_id);
  MR_CHECK_EQ(repaired.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(repaired.decision.authority.backend_boot, second_boot);

  // The retained decision of the fenced incarnation can never be dispatched
  // again: revalidation rejects it and retires it.
  MR_CHECK_EQ(fixture.router->revalidate(first_route.decision.decision_id).code,
              OutcomeCode::REJECT_STALE_BACKEND_BOOT);
  MR_CHECK(fixture.router->check_invariants().ok());
}

#endif  // MODEL_ROUTER_TEST_HAS_CUDA
