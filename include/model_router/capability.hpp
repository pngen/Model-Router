// Model Router - namespaced capability identity and evidence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_CAPABILITY_HPP
#define MODEL_ROUTER_CAPABILITY_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "model_router/clock.hpp"
#include "model_router/ids.hpp"

namespace model_router {

/// Evidence state of a capability claim. The ordering below is the evidence
/// strength ordering: UNKNOWN carries no proof, DECLARED is an operator claim,
/// OBSERVED is runtime observation, VERIFIED is externally validated proof.
/// REVOKED, STALE, and UNSUPPORTED never satisfy a requirement.
enum class CapabilityState : std::uint8_t {
  UNKNOWN = 0,
  DECLARED = 1,
  OBSERVED = 2,
  VERIFIED = 3,
  REVOKED = 4,
  STALE = 5,
  UNSUPPORTED = 6,
  kCount
};

[[nodiscard]] std::string_view to_string(CapabilityState state) noexcept;
/// True when the state can satisfy a requirement at the given minimum state.
[[nodiscard]] bool is_satisfying(CapabilityState state) noexcept;
/// Numeric strength used only for ordering; never a routing score.
[[nodiscard]] std::uint8_t evidence_strength(CapabilityState state) noexcept;

/// A canonical, namespaced capability key such as "text/generation" or
/// "modality/vision-input". Keys are validated: lower-case ASCII, bounded
/// length, no empty segments, no control characters.
class CapabilityKey {
 public:
  CapabilityKey() = default;
  explicit CapabilityKey(std::string value);

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

  /// Returns an empty string when the key is canonical, otherwise the reason.
  [[nodiscard]] static std::string validate(std::string_view value);
  /// Canonicalizes a candidate key (lower-cases, trims separators). Returns an
  /// empty string when the result would be invalid.
  [[nodiscard]] static std::string canonicalize(std::string_view value);

  friend bool operator==(const CapabilityKey&, const CapabilityKey&) = default;
  friend std::strong_ordering operator<=>(const CapabilityKey&, const CapabilityKey&) = default;

 private:
  std::string value_;
};

/// Well-known capability keys. These are names, not quality claims.
namespace capability_keys {
inline constexpr std::string_view text_generation = "text/generation";
inline constexpr std::string_view vision_input = "modality/vision-input";
inline constexpr std::string_view image_output = "modality/image-output";
inline constexpr std::string_view audio_input = "modality/audio-input";
inline constexpr std::string_view audio_output = "modality/audio-output";
inline constexpr std::string_view tool_calling = "tool/calling";
inline constexpr std::string_view structured_output = "output/structured";
inline constexpr std::string_view json_schema = "output/json-schema";
inline constexpr std::string_view reasoning = "task/reasoning";
inline constexpr std::string_view code = "task/code";
inline constexpr std::string_view embedding = "task/embedding";
inline constexpr std::string_view reranking = "task/reranking";
inline constexpr std::string_view long_context = "context/long";
inline constexpr std::string_view streaming = "output/streaming";
inline constexpr std::string_view batch = "output/batch";
inline constexpr std::string_view logprobs = "output/logprobs";
inline constexpr std::string_view multimodal = "modality/multimodal";
inline constexpr std::string_view deterministic_seed = "behavior/deterministic-seed";
inline constexpr std::string_view local_execution = "execution/local";
inline constexpr std::string_view offline_execution = "execution/offline";
inline constexpr std::string_view cuda_execution = "execution/cuda";
}  // namespace capability_keys

/// A single capability claim with full provenance. Evidence from an old backend
/// incarnation never authorizes a fresh backend.
struct CapabilityEvidence {
  CapabilityKey key;
  CapabilityState state{CapabilityState::UNKNOWN};
  CapabilityGeneration generation{};
  CapabilityProfileId profile_id{};
  ModelGeneration model_generation{};
  BackendGeneration backend_generation{};
  BackendBootId backend_boot{};
  UnixMillis observed_at_unix_millis{0};
  /// kNoExpiry means the evidence does not expire.
  UnixMillis expires_at_unix_millis{kNoExpiry};
  std::string source;
  std::string detail;

  [[nodiscard]] bool expired(UnixMillis now) const noexcept {
    return expires_at_unix_millis != kNoExpiry && now >= expires_at_unix_millis;
  }
  [[nodiscard]] bool known() const noexcept { return state != CapabilityState::UNKNOWN; }

  friend bool operator==(const CapabilityEvidence&, const CapabilityEvidence&) = default;
};

/// A required capability with the minimum evidence state that satisfies it.
/// UNKNOWN evidence never satisfies any requirement.
struct CapabilityRequirement {
  CapabilityKey key;
  CapabilityState minimum_state{CapabilityState::DECLARED};

  friend bool operator==(const CapabilityRequirement&, const CapabilityRequirement&) = default;
};

/// A model's or backend's capability profile: a canonical, ordered set of
/// capability claims. Entries are kept sorted by key so that iteration is
/// deterministic and independent of insertion order.
class CapabilityProfile {
 public:
  CapabilityProfile() = default;
  explicit CapabilityProfile(CapabilityProfileId id, CapabilityGeneration generation)
      : id_(id), generation_(generation) {}

  [[nodiscard]] CapabilityProfileId id() const noexcept { return id_; }
  [[nodiscard]] CapabilityGeneration generation() const noexcept { return generation_; }

  /// Inserts or replaces the claim for its key. Returns false when the entry
  /// count would exceed the bound.
  bool set(CapabilityEvidence evidence, std::uint32_t max_entries);
  /// Removes the claim for the key. Returns true when an entry was removed.
  bool erase(const CapabilityKey& key);

  [[nodiscard]] const CapabilityEvidence* find(const CapabilityKey& key) const noexcept;
  [[nodiscard]] const std::vector<CapabilityEvidence>& entries() const noexcept { return entries_; }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

  /// Deterministic evaluation of a requirement against this profile. Returns an
  /// empty string when satisfied, otherwise the rejection reason. UNKNOWN and
  /// expired evidence never satisfy a requirement.
  [[nodiscard]] std::string evaluate(const CapabilityRequirement& requirement,
                                     UnixMillis now) const;

 private:
  void sort_entries();

  CapabilityProfileId id_{};
  CapabilityGeneration generation_{};
  std::vector<CapabilityEvidence> entries_;
};

/// Deterministic, boot-bound set of capability claims published by one backend
/// incarnation. Claims from a different BackendBootId are never authoritative.
struct BackendCapabilityPublication {
  BackendId backend_id{};
  BackendGeneration backend_generation{};
  BackendBootId backend_boot{};
  CapabilityProfileId profile_id{};
  CapabilityGeneration generation{};
  std::vector<CapabilityEvidence> claims;

  friend bool operator==(const BackendCapabilityPublication&,
                         const BackendCapabilityPublication&) = default;
};

}  // namespace model_router

#endif  // MODEL_ROUTER_CAPABILITY_HPP
