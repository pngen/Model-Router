// Model Router - typed outcomes and deterministic explanations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/outcome.hpp"

#include <algorithm>
#include <array>

namespace model_router {
namespace {

constexpr std::array<std::string_view, static_cast<std::size_t>(OutcomeCode::kCount)>
    kOutcomeNames = {
        "ACCEPTED",
        "NO_CHANGE",
        "ROUTED",
        "DEFERRED",
        "REVALIDATION_REQUIRED",
        "REROUTE_REQUIRED",
        "NO_ELIGIBLE_CANDIDATE",
        "REJECT_STALE_ROUTER_EPOCH",
        "REJECT_STALE_COORDINATOR_EPOCH",
        "REJECT_STALE_REQUEST",
        "REJECT_STALE_MODEL",
        "REJECT_STALE_BACKEND",
        "REJECT_STALE_BACKEND_BOOT",
        "REJECT_STALE_ENDPOINT",
        "REJECT_STALE_CAPABILITY",
        "REJECT_STALE_POLICY",
        "REJECT_STALE_BUDGET",
        "REJECT_STALE_PRICE",
        "REJECT_STALE_SLO",
        "REJECT_STALE_HEALTH",
        "REJECT_STALE_AVAILABILITY",
        "REJECT_STALE_READINESS",
        "REJECT_STALE_RESIDENCY",
        "REJECT_STALE_CAPACITY",
        "REJECT_STALE_COMPATIBILITY",
        "REJECT_STALE_TRUST",
        "REJECT_STALE_RESERVATION",
        "REJECT_STALE_ARTIFACT",
        "REJECT_CAPABILITY",
        "REJECT_CONTEXT_LIMIT",
        "REJECT_MODALITY",
        "REJECT_STRUCTURED_OUTPUT",
        "REJECT_TOOL_CALLING",
        "REJECT_STREAMING",
        "REJECT_QUALITY",
        "REJECT_POLICY",
        "REJECT_BUDGET",
        "REJECT_COST",
        "REJECT_COST_UNKNOWN",
        "REJECT_SLO",
        "REJECT_TRUST",
        "REJECT_LOCALITY",
        "REJECT_RESIDENCY",
        "REJECT_COMPATIBILITY",
        "REJECT_UNAVAILABLE",
        "REJECT_NOT_READY",
        "REJECT_UNHEALTHY",
        "REJECT_DRAINING",
        "REJECT_RETIRED",
        "REJECT_CAPACITY",
        "REJECT_RESERVATION",
        "REJECT_AFFINITY",
        "REJECT_INVALID",
        "REJECT_CONFLICT",
        "REJECT_UNKNOWN_EVIDENCE",
        "REJECT_LIMIT",
        "REJECT_SPOOFED",
        "FENCED",
        "SUPERSEDED",
        "DISPATCHED",
        "COMPLETED",
        "FAILED",
        "CANCELLED",
        "SHUTTING_DOWN",
        "INTERNAL_ERROR",
};

[[nodiscard]] std::string escape_json(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char raw : text) {
    const auto ch = static_cast<unsigned char>(raw);
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (ch < 0x20u) {
          static constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[ch >> 4]);
          out.push_back(kHex[ch & 0x0Fu]);
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  return out;
}

}  // namespace

std::string_view to_string(OutcomeCode code) noexcept {
  const auto index = static_cast<std::size_t>(code);
  if (index >= kOutcomeNames.size()) {
    return "UNKNOWN_OUTCOME";
  }
  return kOutcomeNames[index];
}

bool is_acceptance(OutcomeCode code) noexcept {
  switch (code) {
    case OutcomeCode::ACCEPTED:
    case OutcomeCode::NO_CHANGE:
    case OutcomeCode::ROUTED:
    case OutcomeCode::DISPATCHED:
    case OutcomeCode::COMPLETED:
      return true;
    default:
      return false;
  }
}

bool is_rejection(OutcomeCode code) noexcept {
  switch (code) {
    case OutcomeCode::ACCEPTED:
    case OutcomeCode::NO_CHANGE:
    case OutcomeCode::ROUTED:
    case OutcomeCode::DISPATCHED:
    case OutcomeCode::COMPLETED:
    case OutcomeCode::SUPERSEDED:
      return false;
    default:
      return true;
  }
}

bool requires_revalidation(OutcomeCode code) noexcept {
  switch (code) {
    case OutcomeCode::REVALIDATION_REQUIRED:
    case OutcomeCode::REROUTE_REQUIRED:
    case OutcomeCode::REJECT_STALE_ROUTER_EPOCH:
    case OutcomeCode::REJECT_STALE_COORDINATOR_EPOCH:
    case OutcomeCode::REJECT_STALE_REQUEST:
    case OutcomeCode::REJECT_STALE_MODEL:
    case OutcomeCode::REJECT_STALE_BACKEND:
    case OutcomeCode::REJECT_STALE_BACKEND_BOOT:
    case OutcomeCode::REJECT_STALE_ENDPOINT:
    case OutcomeCode::REJECT_STALE_CAPABILITY:
    case OutcomeCode::REJECT_STALE_POLICY:
    case OutcomeCode::REJECT_STALE_BUDGET:
    case OutcomeCode::REJECT_STALE_PRICE:
    case OutcomeCode::REJECT_STALE_SLO:
    case OutcomeCode::REJECT_STALE_HEALTH:
    case OutcomeCode::REJECT_STALE_AVAILABILITY:
    case OutcomeCode::REJECT_STALE_READINESS:
    case OutcomeCode::REJECT_STALE_RESIDENCY:
    case OutcomeCode::REJECT_STALE_CAPACITY:
    case OutcomeCode::REJECT_STALE_COMPATIBILITY:
    case OutcomeCode::REJECT_STALE_TRUST:
    case OutcomeCode::REJECT_STALE_RESERVATION:
    case OutcomeCode::REJECT_STALE_ARTIFACT:
    case OutcomeCode::REJECT_UNKNOWN_EVIDENCE:
      return true;
    default:
      return false;
  }
}

bool is_hard_rejection(OutcomeCode code) noexcept {
  return is_rejection(code) && !requires_revalidation(code) &&
         code != OutcomeCode::SHUTTING_DOWN && code != OutcomeCode::INTERNAL_ERROR;
}

bool permits_fallback(OutcomeCode code) noexcept {
  switch (code) {
    case OutcomeCode::REJECT_STALE_BACKEND_BOOT:
    case OutcomeCode::REJECT_STALE_ENDPOINT:
    case OutcomeCode::REJECT_STALE_HEALTH:
    case OutcomeCode::REJECT_STALE_AVAILABILITY:
    case OutcomeCode::REJECT_STALE_READINESS:
    case OutcomeCode::REJECT_STALE_CAPACITY:
    case OutcomeCode::REJECT_UNAVAILABLE:
    case OutcomeCode::REJECT_NOT_READY:
    case OutcomeCode::REJECT_UNHEALTHY:
    case OutcomeCode::REJECT_CAPACITY:
    case OutcomeCode::REVALIDATION_REQUIRED:
    case OutcomeCode::REROUTE_REQUIRED:
      return true;
    default:
      return false;
  }
}

const std::string* Explanation::find(std::string_view key) const noexcept {
  const auto iter = std::lower_bound(
      factors_.begin(), factors_.end(), key,
      [](const ExplanationFactor& factor, std::string_view value) { return factor.key < value; });
  if (iter == factors_.end() || iter->key != key) {
    return nullptr;
  }
  return &iter->value;
}

void Explanation::add_factor(std::string key, std::string value) {
  factors_.push_back(ExplanationFactor{std::move(key), std::move(value)});
}

void Explanation::canonicalize() {
  std::stable_sort(factors_.begin(), factors_.end(),
                   [](const ExplanationFactor& lhs, const ExplanationFactor& rhs) {
                     return lhs.key < rhs.key;
                   });
}

std::string Explanation::to_text() const {
  std::string out;
  out += std::string(to_string(code_));
  if (!subject_.empty()) {
    out.push_back(' ');
    out += subject_;
  }
  out += " {";
  bool first = true;
  for (const ExplanationFactor& factor : factors_) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    out += factor.key;
    out.push_back('=');
    out += factor.value;
  }
  out += "}";
  if (!message_.empty()) {
    out.push_back(' ');
    out += message_;
  }
  return out;
}

std::string Explanation::to_json() const {
  std::string out = "{\"code\":\"";
  out += std::string(to_string(code_));
  out += "\",\"subject\":\"";
  out += escape_json(subject_);
  out += "\",\"message\":\"";
  out += escape_json(message_);
  out += "\",\"factors\":{";
  bool first = true;
  for (const ExplanationFactor& factor : factors_) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    out.push_back('"');
    out += escape_json(factor.key);
    out += "\":\"";
    out += escape_json(factor.value);
    out.push_back('"');
  }
  out += "}}";
  return out;
}

Explanation ExplanationBuilder::build() const {
  Explanation explanation(code_, subject_);
  explanation.set_message(message_);
  for (const ExplanationFactor& factor : factors_) {
    explanation.add_factor(factor.key, factor.value);
  }
  explanation.canonicalize();
  return explanation;
}

MutationResult MutationResult::make(OutcomeCode code_value, std::string subject,
                                    std::string message) {
  ExplanationBuilder builder(code_value, std::move(subject));
  builder.set_message(std::move(message));
  return MutationResult(code_value, builder.build());
}

bool RouteRejection::canonical_less(const RouteRejection& other) const noexcept {
  if (model_id != other.model_id) {
    return model_id < other.model_id;
  }
  if (model_generation != other.model_generation) {
    return model_generation < other.model_generation;
  }
  if (backend_id != other.backend_id) {
    return backend_id < other.backend_id;
  }
  if (backend_boot != other.backend_boot) {
    return backend_boot < other.backend_boot;
  }
  if (backend_generation != other.backend_generation) {
    return backend_generation < other.backend_generation;
  }
  if (endpoint_id != other.endpoint_id) {
    return endpoint_id < other.endpoint_id;
  }
  if (code != other.code) {
    return static_cast<std::uint16_t>(code) < static_cast<std::uint16_t>(other.code);
  }
  return detail < other.detail;
}

}  // namespace model_router
