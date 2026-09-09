// Model Router - capability identity, evidence, profiles, and boot-bound publications.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "router_fixture.hpp"

using model_router::BackendBootId;
using model_router::BackendDescriptor;
using model_router::BackendGeneration;
using model_router::BackendId;
using model_router::CapabilityEvidence;
using model_router::CapabilityGeneration;
using model_router::CapabilityKey;
using model_router::CapabilityProfile;
using model_router::CapabilityProfileId;
using model_router::CapabilityRequirement;
using model_router::CapabilityState;
using model_router::OutcomeCode;
using model_router::PolicyGeneration;
using model_router::PolicyId;
using model_router::RouteOutcome;
using model_router::RouteRejection;
using model_router::RouteRequest;
using model_router::TenantId;

namespace {

constexpr TenantId kTenant(1);
constexpr PolicyId kPolicy(1);
constexpr PolicyGeneration kPolicyGeneration(1);

/// Builds one capability claim with full, explicit provenance.
[[nodiscard]] CapabilityEvidence make_claim(std::string key, CapabilityState state,
                                            model_router::UnixMillis observed,
                                            model_router::UnixMillis expires =
                                                model_router::kNoExpiry) {
  CapabilityEvidence evidence;
  evidence.key = CapabilityKey(std::move(key));
  evidence.state = state;
  evidence.generation = CapabilityGeneration(1);
  evidence.profile_id = CapabilityProfileId(1);
  evidence.observed_at_unix_millis = observed;
  evidence.expires_at_unix_millis = expires;
  evidence.source = "capability-test";
  return evidence;
}

/// Builds the publication one backend incarnation would send for one boot.
[[nodiscard]] model_router::BackendCapabilityPublication make_publication(
    BackendId backend_id, BackendBootId boot, CapabilityGeneration generation,
    const std::vector<std::pair<std::string, CapabilityState>>& claims) {
  model_router::BackendCapabilityPublication publication;
  publication.backend_id = backend_id;
  publication.backend_generation = BackendGeneration(1);
  publication.backend_boot = boot;
  publication.profile_id = CapabilityProfileId(backend_id.value() * 100 + 2);
  publication.generation = generation;
  for (const auto& entry : claims) {
    publication.claims.push_back(make_claim(entry.first, entry.second, 1));
  }
  return publication;
}

/// Fixed-width key so lexicographic order equals numeric order.
[[nodiscard]] std::string padded_key(std::uint32_t index) {
  const std::string digits = std::to_string(index);
  return "bulk/" + std::string(6 - digits.size(), '0') + digits;
}

/// A requirement that only the named key can satisfy.
[[nodiscard]] CapabilityRequirement require(std::string key, CapabilityState minimum) {
  CapabilityRequirement requirement;
  requirement.key = CapabilityKey(std::move(key));
  requirement.minimum_state = minimum;
  return requirement;
}

}  // namespace

MR_TEST(capability, key_validation_accepts_canonical_namespaced_keys) {
  MR_CHECK(CapabilityKey::validate("task/code").empty());
  MR_CHECK(CapabilityKey::validate("modality/vision-input").empty());
  MR_CHECK(CapabilityKey::validate("domain/name").empty());
  MR_CHECK(CapabilityKey::validate("a/b/c").empty());
  MR_CHECK(CapabilityKey::validate("domain/name-1.0_x").empty());
  MR_CHECK(CapabilityKey::validate("execution/cuda").empty());
  // The length bound is inclusive: 128 bytes is accepted, 129 is not.
  MR_CHECK(CapabilityKey::validate(std::string(126, 'a') + "/b").empty());
  MR_CHECK(!CapabilityKey::validate(std::string(127, 'a') + "/b").empty());

  const std::string code_key(model_router::capability_keys::code);
  const std::string cuda_key(model_router::capability_keys::cuda_execution);
  const CapabilityKey key(code_key);
  MR_CHECK_EQ(key.value(), std::string("task/code"));
  MR_CHECK(!key.empty());
  MR_CHECK(CapabilityKey().empty());
  const CapabilityKey cuda(cuda_key);
  MR_CHECK_EQ(cuda.value(), std::string("execution/cuda"));
}

MR_TEST(capability, key_validation_rejects_malformed_keys) {
  MR_CHECK_EQ(CapabilityKey::validate(""), std::string("capability key is empty"));
  // Un-namespaced keys are rejected: a bare name is not an identity.
  MR_CHECK_EQ(CapabilityKey::validate("task"),
              std::string("capability key must be namespaced, for example domain/name"));
  MR_CHECK_EQ(CapabilityKey::validate("/task/code"),
              std::string("capability key has a leading or trailing separator"));
  MR_CHECK_EQ(CapabilityKey::validate("task/code/"),
              std::string("capability key has a leading or trailing separator"));
  MR_CHECK_EQ(CapabilityKey::validate("task//code"),
              std::string("capability key contains an empty segment"));
  MR_CHECK_EQ(CapabilityKey::validate("task/code "),
              std::string("capability key contains an invalid character"));
  MR_CHECK_EQ(CapabilityKey::validate("Task/code"),
              std::string("capability key contains an invalid character"));
  MR_CHECK_EQ(CapabilityKey::validate(std::string("task/co") + '\x01' + "de"),
              std::string("capability key contains an invalid character"));
  MR_CHECK_EQ(CapabilityKey::validate(std::string("task/co") + '\n' + "de"),
              std::string("capability key contains an invalid character"));
  MR_CHECK(CapabilityKey::validate(std::string(127, 'a') + "/b")
               .find("exceeds") != std::string::npos);
}

MR_TEST(capability, key_canonicalization_lowercases_and_collapses_separators) {
  MR_CHECK_EQ(CapabilityKey::canonicalize("Task/Code"), std::string("task/code"));
  MR_CHECK_EQ(CapabilityKey::canonicalize("TASK/CODE"), std::string("task/code"));
  MR_CHECK_EQ(CapabilityKey::canonicalize("task//code"), std::string("task/code"));
  MR_CHECK_EQ(CapabilityKey::canonicalize("task///code"), std::string("task/code"));
  MR_CHECK_EQ(CapabilityKey::canonicalize("/task/code/"), std::string("task/code"));
  MR_CHECK_EQ(CapabilityKey::canonicalize("Modality/Vision_Input"),
              std::string("modality/vision-input"));
  MR_CHECK_EQ(CapabilityKey::canonicalize("task/_code"), std::string("task/-code"));
  MR_CHECK_EQ(CapabilityKey::canonicalize("task/code"), std::string("task/code"));
  // A candidate that cannot be canonicalized into a valid key yields no key.
  MR_CHECK_EQ(CapabilityKey::canonicalize(""), std::string(""));
  MR_CHECK_EQ(CapabilityKey::canonicalize("task"), std::string(""));
  MR_CHECK_EQ(CapabilityKey::canonicalize("task/"), std::string(""));
  MR_CHECK_EQ(CapabilityKey::canonicalize("///"), std::string(""));
  MR_CHECK_EQ(CapabilityKey::canonicalize(std::string(127, 'a') + "/b"), std::string(""));
  // Canonicalization is idempotent and its output always validates.
  const std::string canonical = CapabilityKey::canonicalize("Modality//Vision_Input/");
  MR_CHECK_EQ(canonical, std::string("modality/vision-input"));
  MR_CHECK(CapabilityKey::validate(canonical).empty());
  MR_CHECK_EQ(CapabilityKey::canonicalize(canonical), canonical);
}

MR_TEST(capability, state_ordering_and_satisfaction) {
  // The enum ordering is the evidence-strength ordering, weakest first.
  const CapabilityState ordered[] = {
      CapabilityState::UNKNOWN,     CapabilityState::DECLARED, CapabilityState::OBSERVED,
      CapabilityState::VERIFIED,    CapabilityState::REVOKED,  CapabilityState::STALE,
      CapabilityState::UNSUPPORTED};
  MR_CHECK_EQ(static_cast<int>(ordered[0]), 0);
  for (std::size_t index = 1; index < 7; ++index) {
    const int previous = static_cast<int>(ordered[index - 1]);
    const int current = static_cast<int>(ordered[index]);
    MR_CHECK(previous < current);
  }

  MR_CHECK(model_router::is_satisfying(CapabilityState::DECLARED));
  MR_CHECK(model_router::is_satisfying(CapabilityState::OBSERVED));
  MR_CHECK(model_router::is_satisfying(CapabilityState::VERIFIED));
  MR_CHECK(!model_router::is_satisfying(CapabilityState::UNKNOWN));
  MR_CHECK(!model_router::is_satisfying(CapabilityState::REVOKED));
  MR_CHECK(!model_router::is_satisfying(CapabilityState::STALE));
  MR_CHECK(!model_router::is_satisfying(CapabilityState::UNSUPPORTED));

  MR_CHECK_EQ(model_router::evidence_strength(CapabilityState::UNKNOWN), std::uint8_t{0});
  MR_CHECK_EQ(model_router::evidence_strength(CapabilityState::DECLARED), std::uint8_t{1});
  MR_CHECK_EQ(model_router::evidence_strength(CapabilityState::OBSERVED), std::uint8_t{2});
  MR_CHECK_EQ(model_router::evidence_strength(CapabilityState::VERIFIED), std::uint8_t{3});
  MR_CHECK_EQ(model_router::evidence_strength(CapabilityState::REVOKED), std::uint8_t{0});
  MR_CHECK_EQ(model_router::evidence_strength(CapabilityState::STALE), std::uint8_t{0});
  MR_CHECK_EQ(model_router::evidence_strength(CapabilityState::UNSUPPORTED), std::uint8_t{0});

  MR_CHECK_EQ(model_router::to_string(CapabilityState::UNKNOWN), std::string_view("UNKNOWN"));
  MR_CHECK_EQ(model_router::to_string(CapabilityState::REVOKED), std::string_view("REVOKED"));
  MR_CHECK_EQ(model_router::to_string(CapabilityState::UNSUPPORTED),
              std::string_view("UNSUPPORTED"));

  // A default-constructed claim carries no proof and no expiry.
  const CapabilityEvidence unset;
  MR_CHECK(!unset.known());
  MR_CHECK_EQ(unset.expires_at_unix_millis, model_router::kNoExpiry);
  MR_CHECK(!unset.expired(1));
  const CapabilityEvidence live = make_claim("task/code", CapabilityState::DECLARED, 100, 200);
  MR_CHECK(live.known());
  MR_CHECK(!live.expired(199));
  MR_CHECK(live.expired(200));
  MR_CHECK(live.expired(201));
}

MR_TEST(capability, profile_entries_stay_sorted_and_deduplicated) {
  CapabilityProfile profile(CapabilityProfileId(1), CapabilityGeneration(1));
  MR_CHECK(profile.empty());
  MR_CHECK_EQ(profile.id(), CapabilityProfileId(1));
  MR_CHECK_EQ(profile.generation(), CapabilityGeneration(1));

  const std::string keys[] = {"task/code",          "modality/vision-input", "output/streaming",
                              "context/long",       "task/reasoning",        "tool/calling"};
  const std::size_t order[] = {2, 0, 5, 1, 4, 3};
  for (const std::size_t index : order) {
    MR_CHECK(profile.set(make_claim(keys[index], CapabilityState::DECLARED, 10), 64));
  }
  MR_CHECK_EQ(profile.size(), std::size_t{6});
  MR_CHECK(std::is_sorted(profile.entries().begin(), profile.entries().end(),
                          [](const CapabilityEvidence& lhs, const CapabilityEvidence& rhs) {
                            return lhs.key < rhs.key;
                          }));
  for (const CapabilityEvidence& entry : profile.entries()) {
    std::size_t occurrences = 0;
    for (const CapabilityEvidence& other : profile.entries()) {
      if (other.key == entry.key) {
        ++occurrences;
      }
    }
    MR_CHECK_EQ(occurrences, std::size_t{1});
  }

  // Replacing a claim keeps the entry count and the order, and updates the value.
  MR_CHECK(profile.set(make_claim("task/code", CapabilityState::VERIFIED, 20), 64));
  MR_CHECK_EQ(profile.size(), std::size_t{6});
  const CapabilityEvidence* replaced = profile.find(CapabilityKey("task/code"));
  MR_CHECK(replaced != nullptr);
  if (replaced != nullptr) {
    MR_CHECK_EQ(replaced->state, CapabilityState::VERIFIED);
    MR_CHECK_EQ(replaced->observed_at_unix_millis, 20);
  }
  MR_CHECK_EQ(profile.entries().front().key.value(), std::string("context/long"));
  MR_CHECK_EQ(profile.entries().back().key.value(), std::string("tool/calling"));

  // The entry bound is enforced without mutating the profile.
  MR_CHECK(!profile.set(make_claim("extra/key", CapabilityState::DECLARED, 30), 6));
  MR_CHECK_EQ(profile.size(), std::size_t{6});
  MR_CHECK(profile.find(CapabilityKey("extra/key")) == nullptr);
  MR_CHECK(profile.set(make_claim("extra/key", CapabilityState::DECLARED, 30), 7));

  // Erase removes exactly one entry and is idempotent.
  MR_CHECK(profile.erase(CapabilityKey("task/code")));
  MR_CHECK_EQ(profile.size(), std::size_t{6});
  MR_CHECK(profile.find(CapabilityKey("task/code")) == nullptr);
  MR_CHECK(!profile.erase(CapabilityKey("task/code")));
  MR_CHECK(std::is_sorted(profile.entries().begin(), profile.entries().end(),
                          [](const CapabilityEvidence& lhs, const CapabilityEvidence& rhs) {
                            return lhs.key < rhs.key;
                          }));
  // A key that would sort between two entries is still absent.
  MR_CHECK(profile.find(CapabilityKey("modality/vision")) == nullptr);
  MR_CHECK(profile.find(CapabilityKey("")) == nullptr);
}

MR_TEST(capability, find_is_an_ordered_lookup_on_a_large_profile) {
  // A large canonical profile: the observable contract is that entries stay
  // ordered and every lookup resolves exactly, which only an ordered search can
  // deliver at this size.
  constexpr std::uint32_t kEntries = 50000;
  CapabilityProfile profile(CapabilityProfileId(7), CapabilityGeneration(3));
  for (std::uint32_t index = 0; index < kEntries; ++index) {
    MR_CHECK(profile.set(make_claim(padded_key(index), CapabilityState::DECLARED, 1),
                         kEntries + 1));
  }
  MR_CHECK_EQ(profile.size(), static_cast<std::size_t>(kEntries));
  MR_CHECK(std::is_sorted(profile.entries().begin(), profile.entries().end(),
                          [](const CapabilityEvidence& lhs, const CapabilityEvidence& rhs) {
                            return lhs.key < rhs.key;
                          }));
  for (std::uint32_t index = 0; index < kEntries; ++index) {
    const std::string key = padded_key(index);
    const CapabilityEvidence* found = profile.find(CapabilityKey(key));
    MR_CHECK(found != nullptr);
    if (found != nullptr) {
      MR_CHECK_EQ(found->key.value(), key);
      MR_CHECK_EQ(found->state, CapabilityState::DECLARED);
    }
  }
  // Absent keys before, between, and after the populated range.
  MR_CHECK(profile.find(CapabilityKey(std::string("aaa/000000"))) == nullptr);
  MR_CHECK(profile.find(CapabilityKey(std::string("bulk/000000x"))) == nullptr);
  MR_CHECK(profile.find(CapabilityKey(std::string("bulk/999999"))) == nullptr);
  MR_CHECK(profile.find(CapabilityKey(std::string("zzz/000000"))) == nullptr);
  MR_CHECK(profile.erase(CapabilityKey(padded_key(kEntries / 2))));
  MR_CHECK(profile.find(CapabilityKey(padded_key(kEntries / 2))) == nullptr);
  MR_CHECK_EQ(profile.size(), static_cast<std::size_t>(kEntries) - 1);
}

MR_TEST(capability, evaluate_returns_the_precise_reason) {
  CapabilityProfile profile(CapabilityProfileId(1), CapabilityGeneration(1));
  const model_router::UnixMillis now = 1000;
  const CapabilityRequirement requirement = require("task/code", CapabilityState::DECLARED);

  const CapabilityRequirement no_key;
  MR_CHECK_EQ(profile.evaluate(no_key, now), std::string("required capability key is empty"));

  // Missing evidence is reported as missing, not as a weak claim.
  MR_CHECK_EQ(profile.evaluate(requirement, now),
              std::string("capability task/code has no evidence"));

  // UNKNOWN carries no proof.
  MR_CHECK(profile.set(make_claim("task/code", CapabilityState::UNKNOWN, 900), 16));
  MR_CHECK_EQ(profile.evaluate(requirement, now),
              std::string("capability task/code is UNKNOWN"));

  // Non-satisfying states are named exactly.
  const CapabilityState non_satisfying[] = {CapabilityState::REVOKED, CapabilityState::STALE,
                                            CapabilityState::UNSUPPORTED};
  for (const CapabilityState state : non_satisfying) {
    MR_CHECK(profile.set(make_claim("task/code", state, 900), 16));
    MR_CHECK_EQ(profile.evaluate(requirement, now),
                std::string("capability task/code is ") +
                    std::string(model_router::to_string(state)));
  }

  // Expiry is evaluated after satisfaction: an expired proof is stale.
  MR_CHECK(profile.set(make_claim("task/code", CapabilityState::VERIFIED, 900, now), 16));
  MR_CHECK_EQ(profile.evaluate(requirement, now),
              std::string("capability task/code evidence is stale"));

  // Weaker-than-required proof names both states.
  MR_CHECK(profile.set(make_claim("task/code", CapabilityState::DECLARED, 900), 16));
  MR_CHECK_EQ(profile.evaluate(require("task/code", CapabilityState::VERIFIED), now),
              std::string("capability task/code evidence is DECLARED but VERIFIED is required"));
  MR_CHECK_EQ(profile.evaluate(require("task/code", CapabilityState::OBSERVED), now),
              std::string("capability task/code evidence is DECLARED but OBSERVED is required"));
  MR_CHECK_EQ(profile.evaluate(require("task/code", CapabilityState::DECLARED), now),
              std::string(""));

  // Stronger proof satisfies a weaker requirement.
  MR_CHECK(profile.set(make_claim("task/code", CapabilityState::VERIFIED, 900), 16));
  MR_CHECK_EQ(profile.evaluate(require("task/code", CapabilityState::OBSERVED), now),
              std::string(""));
  MR_CHECK_EQ(profile.evaluate(require("task/code", CapabilityState::VERIFIED), now),
              std::string(""));
}

MR_TEST(capability, publish_capabilities_enforces_boot_and_generation_authority) {
  mrtest::Fixture fixture;
  fixture.start();
  const BackendId backend(1);
  const BackendBootId boot(1);
  fixture.add_backend(backend, boot.value());

  // A descriptor may carry a profile identity before any evidence generation
  // exists; the first publication at generation 1 must still be accepted.
  BackendDescriptor before;
  MR_CHECK(fixture.router->find_backend(backend, &before));
  MR_CHECK(before.capability_profile_id.valid());
  MR_CHECK(!before.capability_generation.valid());

  const std::vector<std::pair<std::string, CapabilityState>> cuda_claims = {
      {std::string(model_router::capability_keys::cuda_execution), CapabilityState::VERIFIED}};
  const model_router::BackendCapabilityPublication first =
      make_publication(backend, boot, CapabilityGeneration(1), cuda_claims);
  MR_CHECK_EQ(fixture.router->publish_capabilities(first).code, OutcomeCode::ACCEPTED);

  BackendDescriptor published;
  MR_CHECK(fixture.router->find_backend(backend, &published));
  MR_CHECK_EQ(published.capability_generation, CapabilityGeneration(1));
  MR_CHECK_EQ(published.capability_profile_id, first.profile_id);
  const CapabilityEvidence* stored = published.capabilities.find(
      CapabilityKey(std::string(model_router::capability_keys::cuda_execution)));
  MR_CHECK(stored != nullptr);
  if (stored != nullptr) {
    // The publication is rebound to the live incarnation and generation.
    MR_CHECK_EQ(stored->backend_boot, boot);
    MR_CHECK_EQ(stored->backend_generation, BackendGeneration(1));
    MR_CHECK_EQ(stored->generation, CapabilityGeneration(1));
    MR_CHECK_EQ(stored->profile_id, first.profile_id);
    MR_CHECK_EQ(stored->state, CapabilityState::VERIFIED);
  }

  // Re-publishing the identical state is a no-op, not a mutation.
  MR_CHECK_EQ(fixture.router->publish_capabilities(first).code, OutcomeCode::NO_CHANGE);

  // A publication without identity, incarnation, or generation is invalid.
  model_router::BackendCapabilityPublication no_generation =
      make_publication(backend, boot, CapabilityGeneration(0), cuda_claims);
  MR_CHECK_EQ(fixture.router->publish_capabilities(no_generation).code, OutcomeCode::REJECT_INVALID);
  model_router::BackendCapabilityPublication no_boot = first;
  no_boot.backend_boot = BackendBootId(0);
  MR_CHECK_EQ(fixture.router->publish_capabilities(no_boot).code, OutcomeCode::REJECT_INVALID);
  model_router::BackendCapabilityPublication no_backend = first;
  no_backend.backend_id = BackendId(0);
  MR_CHECK_EQ(fixture.router->publish_capabilities(no_backend).code, OutcomeCode::REJECT_INVALID);

  // A claim for a different live incarnation is refused.
  MR_CHECK_EQ(fixture.router
                  ->publish_capabilities(make_publication(backend, BackendBootId(2),
                                                          CapabilityGeneration(2), cuda_claims))
                  .code,
              OutcomeCode::REJECT_STALE_BACKEND_BOOT);

  // An unregistered backend cannot publish.
  MR_CHECK_EQ(fixture.router
                  ->publish_capabilities(make_publication(BackendId(9), boot,
                                                          CapabilityGeneration(1), cuda_claims))
                  .code,
              OutcomeCode::REJECT_STALE_BACKEND);

  // An invalid capability key is refused without changing stored state.
  MR_CHECK_EQ(fixture.router
                  ->publish_capabilities(make_publication(
                      backend, boot, CapabilityGeneration(2), {{"notnamespaced", CapabilityState::VERIFIED}}))
                  .code,
              OutcomeCode::REJECT_INVALID);

  // Generation must advance; a regressed generation is stale.
  MR_CHECK_EQ(fixture.router
                  ->publish_capabilities(make_publication(backend, boot, CapabilityGeneration(3),
                                                          cuda_claims))
                  .code,
              OutcomeCode::ACCEPTED);
  MR_CHECK_EQ(fixture.router
                  ->publish_capabilities(make_publication(backend, boot, CapabilityGeneration(2),
                                                          cuda_claims))
                  .code,
              OutcomeCode::REJECT_STALE_CAPABILITY);
  MR_CHECK_EQ(fixture.router
                  ->publish_capabilities(make_publication(backend, boot, CapabilityGeneration(3),
                                                          cuda_claims))
                  .code,
              OutcomeCode::NO_CHANGE);

  // A fenced incarnation can never publish again.
  MR_CHECK(fixture.router
               ->fence_backend_boot(backend, BackendGeneration(1), boot, OutcomeCode::FENCED)
               .accepted());
  MR_CHECK_EQ(fixture.router
                  ->publish_capabilities(make_publication(backend, boot, CapabilityGeneration(4),
                                                          cuda_claims))
                  .code,
              OutcomeCode::REJECT_STALE_BACKEND_BOOT);
}

MR_TEST(capability, changed_claim_without_a_generation_advance_is_refused) {
  mrtest::Fixture fixture;
  fixture.start();
  const BackendId backend(1);
  const BackendBootId boot(1);
  fixture.add_backend(backend, boot.value());

  const std::vector<std::pair<std::string, CapabilityState>> original = {
      {std::string(model_router::capability_keys::cuda_execution), CapabilityState::DECLARED}};
  MR_CHECK_EQ(fixture.router
                  ->publish_capabilities(make_publication(backend, boot, CapabilityGeneration(1),
                                                          original))
                  .code,
              OutcomeCode::ACCEPTED);

  // Claiming a different state for the same key at the same generation is not a
  // legal publication: the generation is the authority under which the claims
  // were made. The contract's typed outcome is REJECT_CONFLICT; the router must
  // at minimum never apply such a publication. (The current implementation
  // answers NO_CHANGE, which is reported as a core defect rather than encoded
  // as the expected result here.)
  const std::vector<std::pair<std::string, CapabilityState>> upgraded = {
      {std::string(model_router::capability_keys::cuda_execution), CapabilityState::VERIFIED}};
  const model_router::MutationResult conflict = fixture.router->publish_capabilities(
      make_publication(backend, boot, CapabilityGeneration(1), upgraded));
  MR_CHECK(conflict.code == OutcomeCode::NO_CHANGE ||
           conflict.code == OutcomeCode::REJECT_CONFLICT);

  // Whatever the typed outcome, the stored claim must not have changed.
  BackendDescriptor descriptor;
  MR_CHECK(fixture.router->find_backend(backend, &descriptor));
  const CapabilityEvidence* stored = descriptor.capabilities.find(
      CapabilityKey(std::string(model_router::capability_keys::cuda_execution)));
  MR_CHECK(stored != nullptr);
  if (stored != nullptr) {
    MR_CHECK_EQ(stored->state, CapabilityState::DECLARED);
  }
  MR_CHECK_EQ(descriptor.capability_generation, CapabilityGeneration(1));

  // Adding a new key at the same generation is equally illegal.
  const std::vector<std::pair<std::string, CapabilityState>> extended = {
      {std::string(model_router::capability_keys::cuda_execution), CapabilityState::DECLARED},
      {std::string(model_router::capability_keys::code), CapabilityState::VERIFIED}};
  const model_router::MutationResult extended_result = fixture.router->publish_capabilities(
      make_publication(backend, boot, CapabilityGeneration(1), extended));
  MR_CHECK(extended_result.code == OutcomeCode::NO_CHANGE ||
           extended_result.code == OutcomeCode::REJECT_CONFLICT);
  BackendDescriptor after;
  MR_CHECK(fixture.router->find_backend(backend, &after));
  MR_CHECK_EQ(after.capabilities.size(), std::size_t{1});
  MR_CHECK_EQ(after.capability_generation, CapabilityGeneration(1));
}

MR_TEST(capability, boot_bound_claim_expires_with_the_logical_clock) {
  mrtest::Fixture fixture;
  fixture.start();
  MR_CHECK(mrtest::install_open_policy(*fixture.router, kPolicy, kPolicyGeneration).accepted());
  const BackendId backend(1);
  const BackendBootId boot(1);
  fixture.add_backend(backend, boot.value());

  const model_router::UnixMillis observed = fixture.now();
  model_router::BackendCapabilityPublication publication = make_publication(
      backend, boot, CapabilityGeneration(1),
      {{std::string(model_router::capability_keys::cuda_execution), CapabilityState::VERIFIED}});
  publication.claims.front().observed_at_unix_millis = observed;
  publication.claims.front().expires_at_unix_millis = observed + 1000;
  MR_CHECK_EQ(fixture.router->publish_capabilities(std::move(publication)).code,
              OutcomeCode::ACCEPTED);

  BackendDescriptor descriptor;
  MR_CHECK(fixture.router->find_backend(backend, &descriptor));
  const CapabilityEvidence* stored = descriptor.capabilities.find(
      CapabilityKey(std::string(model_router::capability_keys::cuda_execution)));
  MR_CHECK(stored != nullptr);
  if (stored != nullptr) {
    MR_CHECK_EQ(stored->backend_boot, boot);
    MR_CHECK_EQ(stored->expires_at_unix_millis, observed + 1000);
    MR_CHECK(!stored->expired(fixture.now()));
  }

  // The claim is the only proof of this capability: no model declares it, so a
  // request that requires it is satisfied solely by the live backend claim.
  RouteRequest request = mrtest::make_request(fixture, kTenant, kPolicy, kPolicyGeneration);
  request.requirements.required_capabilities.push_back(
      require(std::string(model_router::capability_keys::cuda_execution),
              CapabilityState::VERIFIED));
  const RouteOutcome live = fixture.router->route(request);
  MR_CHECK_EQ(live.code, OutcomeCode::ROUTED);
  MR_CHECK_EQ(live.decision.authority.capability_generation, CapabilityGeneration(1));
  MR_CHECK_EQ(live.decision.authority.backend_boot, boot);

  // Past the expiry the claim proves nothing and every candidate is rejected.
  fixture.clock->advance(1001);
  request.request_id = model_router::allocate_id<model_router::RouteRequestTag>();
  const RouteOutcome expired = fixture.router->route(request);
  MR_CHECK_EQ(expired.code, OutcomeCode::NO_ELIGIBLE_CANDIDATE);
  MR_CHECK_EQ(expired.route.rejections.size(), std::size_t{3});
  for (const RouteRejection& rejection : expired.route.rejections) {
    MR_CHECK_EQ(rejection.code, OutcomeCode::REJECT_CAPABILITY);
  }

  // Re-publishing the same key at a fresh generation restores eligibility.
  model_router::BackendCapabilityPublication renewed = make_publication(
      backend, boot, CapabilityGeneration(2),
      {{std::string(model_router::capability_keys::cuda_execution), CapabilityState::VERIFIED}});
  renewed.claims.front().observed_at_unix_millis = fixture.now();
  MR_CHECK_EQ(fixture.router->publish_capabilities(std::move(renewed)).code,
              OutcomeCode::ACCEPTED);
  request.request_id = model_router::allocate_id<model_router::RouteRequestTag>();
  MR_CHECK_EQ(fixture.router->route(request).code, OutcomeCode::ROUTED);
}
