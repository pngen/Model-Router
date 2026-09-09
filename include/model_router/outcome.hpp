// Model Router - typed outcomes and deterministic explanations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_OUTCOME_HPP
#define MODEL_ROUTER_OUTCOME_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "model_router/ids.hpp"

namespace model_router {

/// Deterministic outcome of a routing mutation or routing operation. No
/// meaningful operation returns a bare boolean: every outcome names exactly why
/// it did or did not take effect.
enum class OutcomeCode : std::uint16_t {
  ACCEPTED = 0,
  NO_CHANGE,
  ROUTED,
  DEFERRED,
  REVALIDATION_REQUIRED,
  REROUTE_REQUIRED,
  NO_ELIGIBLE_CANDIDATE,

  // Stale-authority rejections: the operation was refused because a bound
  // generation is no longer current.
  REJECT_STALE_ROUTER_EPOCH,
  REJECT_STALE_COORDINATOR_EPOCH,
  REJECT_STALE_REQUEST,
  REJECT_STALE_MODEL,
  REJECT_STALE_BACKEND,
  REJECT_STALE_BACKEND_BOOT,
  REJECT_STALE_ENDPOINT,
  REJECT_STALE_CAPABILITY,
  REJECT_STALE_POLICY,
  REJECT_STALE_BUDGET,
  REJECT_STALE_PRICE,
  REJECT_STALE_SLO,
  REJECT_STALE_HEALTH,
  REJECT_STALE_AVAILABILITY,
  REJECT_STALE_READINESS,
  REJECT_STALE_RESIDENCY,
  REJECT_STALE_CAPACITY,
  REJECT_STALE_COMPATIBILITY,
  REJECT_STALE_TRUST,
  REJECT_STALE_RESERVATION,
  REJECT_STALE_ARTIFACT,

  // Hard-eligibility rejections.
  REJECT_CAPABILITY,
  REJECT_CONTEXT_LIMIT,
  REJECT_MODALITY,
  REJECT_STRUCTURED_OUTPUT,
  REJECT_TOOL_CALLING,
  REJECT_STREAMING,
  REJECT_QUALITY,
  REJECT_POLICY,
  REJECT_BUDGET,
  REJECT_COST,
  REJECT_COST_UNKNOWN,
  REJECT_SLO,
  REJECT_TRUST,
  REJECT_LOCALITY,
  REJECT_RESIDENCY,
  REJECT_COMPATIBILITY,
  REJECT_UNAVAILABLE,
  REJECT_NOT_READY,
  REJECT_UNHEALTHY,
  REJECT_DRAINING,
  REJECT_RETIRED,
  REJECT_CAPACITY,
  REJECT_RESERVATION,
  REJECT_AFFINITY,
  REJECT_INVALID,
  REJECT_CONFLICT,
  REJECT_UNKNOWN_EVIDENCE,
  REJECT_LIMIT,
  REJECT_SPOOFED,

  // Lifecycle.
  FENCED,
  SUPERSEDED,
  DISPATCHED,
  COMPLETED,
  FAILED,
  CANCELLED,
  SHUTTING_DOWN,
  INTERNAL_ERROR,

  kCount
};

[[nodiscard]] std::string_view to_string(OutcomeCode code) noexcept;

/// True when the outcome means the requested effect took place (or the state was
/// already exactly as requested).
[[nodiscard]] bool is_acceptance(OutcomeCode code) noexcept;
/// True when the outcome rejected the request without changing canonical state.
[[nodiscard]] bool is_rejection(OutcomeCode code) noexcept;
/// True when the caller must obtain fresh authority before retrying.
[[nodiscard]] bool requires_revalidation(OutcomeCode code) noexcept;
/// True when the outcome indicates a hard-invalid candidate rather than a
/// transient condition.
[[nodiscard]] bool is_hard_rejection(OutcomeCode code) noexcept;
/// True when the outcome permits a policy-bound fallback attempt.
[[nodiscard]] bool permits_fallback(OutcomeCode code) noexcept;

/// One deterministic explanation factor. Factors are always emitted in
/// canonical key-ascending order so that explanations are byte-stable.
struct ExplanationFactor {
  std::string key;
  std::string value;

  friend bool operator==(const ExplanationFactor&, const ExplanationFactor&) = default;
};

/// A deterministic, canonically ordered explanation.
class Explanation {
 public:
  Explanation() = default;
  Explanation(OutcomeCode code, std::string subject)
      : code_(code), subject_(std::move(subject)) {}

  [[nodiscard]] OutcomeCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& subject() const noexcept { return subject_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const std::vector<ExplanationFactor>& factors() const noexcept { return factors_; }

  [[nodiscard]] const std::string* find(std::string_view key) const noexcept;

  /// Canonical single-line rendering: "code subject {k=v,...} message".
  [[nodiscard]] std::string to_text() const;
  /// Canonical JSON object rendering with factors in key order.
  [[nodiscard]] std::string to_json() const;

  void set_message(std::string message) { message_ = std::move(message); }
  void add_factor(std::string key, std::string value);
  /// Restores canonical key order after factors were appended out of order.
  void canonicalize();

 private:
  OutcomeCode code_{OutcomeCode::INTERNAL_ERROR};
  std::string subject_;
  std::string message_;
  std::vector<ExplanationFactor> factors_;
};

/// Builder used by the implementation and by callers to assemble explanations.
class ExplanationBuilder {
 public:
  ExplanationBuilder(OutcomeCode code, std::string subject)
      : code_(code), subject_(std::move(subject)) {}

  ExplanationBuilder& add(std::string key, std::string value) {
    factors_.emplace_back(std::move(key), std::move(value));
    return *this;
  }
  ExplanationBuilder& add_u64(std::string key, std::uint64_t value) {
    return add(std::move(key), std::to_string(value));
  }
  ExplanationBuilder& add_i64(std::string key, std::int64_t value) {
    return add(std::move(key), std::to_string(value));
  }
  ExplanationBuilder& add_bool(std::string key, bool value) {
    return add(std::move(key), value ? "true" : "false");
  }
  template <class Tag>
  ExplanationBuilder& add_id(std::string key, EntityId<Tag> id) {
    return add(std::move(key), id.valid() ? std::to_string(id.value()) : "none");
  }
  template <class Tag>
  ExplanationBuilder& add_generation(std::string key, Generation<Tag> generation) {
    return add(std::move(key), generation.valid() ? std::to_string(generation.value()) : "none");
  }
  ExplanationBuilder& set_message(std::string message) {
    message_ = std::move(message);
    return *this;
  }

  [[nodiscard]] Explanation build() const;

 private:
  OutcomeCode code_;
  std::string subject_;
  std::string message_;
  std::vector<ExplanationFactor> factors_;
};

/// Result of a routing mutation or routing operation.
struct MutationResult {
  OutcomeCode code{OutcomeCode::INTERNAL_ERROR};
  Explanation explanation;

  MutationResult() = default;
  MutationResult(OutcomeCode code_value, Explanation explanation_value)
      : code(code_value), explanation(std::move(explanation_value)) {}

  [[nodiscard]] bool accepted() const noexcept { return is_acceptance(code); }
  [[nodiscard]] bool rejected() const noexcept { return is_rejection(code); }
  [[nodiscard]] bool needs_revalidation() const noexcept { return requires_revalidation(code); }
  [[nodiscard]] bool hard_rejected() const noexcept { return is_hard_rejection(code); }

  [[nodiscard]] std::string to_text() const { return explanation.to_text(); }

  [[nodiscard]] static MutationResult make(OutcomeCode code_value, std::string subject,
                                           std::string message);
};

/// One rejected candidate with its typed hard-rejection reason.
struct RouteRejection {
  std::uint64_t model_id{0};
  ModelGeneration model_generation{};
  std::uint64_t backend_id{0};
  BackendGeneration backend_generation{};
  BackendBootId backend_boot{};
  std::uint64_t endpoint_id{0};
  OutcomeCode code{OutcomeCode::INTERNAL_ERROR};
  std::string detail;

  /// Canonical ordering key, so rejection lists never depend on insertion order.
  [[nodiscard]] bool canonical_less(const RouteRejection& other) const noexcept;
  friend bool operator==(const RouteRejection&, const RouteRejection&) = default;
};

}  // namespace model_router

#endif  // MODEL_ROUTER_OUTCOME_HPP
