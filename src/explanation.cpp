// Model Router - deterministic route explanations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <charconv>
#include <string>

#include "model_router/decision.hpp"
#include "model_router/detail/sha256.hpp"

namespace model_router {
namespace {

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

void append_key(std::string* out, const CandidateKey& key) {
  *out += key.to_string();
}

}  // namespace

namespace {

/// Appends an integer as fixed-width little-endian bytes. The digest is built
/// on every routing commit, so it avoids decimal conversion entirely; the
/// encoding is explicit so the value is identical on every host.
void append_u64(std::string* out, std::uint64_t value) {
  char buffer[8];
  for (int shift = 0; shift < 64; shift += 8) {
    buffer[shift / 8] = static_cast<char>((value >> shift) & 0xFFu);
  }
  out->append(buffer, sizeof(buffer));
}

}  // namespace

std::string RouteExplanation::compute_semantic_digest() const {
  std::string canonical;
  canonical.reserve(512 + ranking.size() * 96 + rejections.size() * 48);
  canonical += requirement_digest;
  canonical.push_back('|');
  canonical += winner.to_string();
  canonical.push_back('|');
  // The digest identifies the routing outcome: which candidates are eligible,
  // in what order, with what integer score, and under which bound authority.
  // The per-factor detail that produced each score stays in the explanation
  // itself; folding every factor value into the digest would multiply the
  // hashed volume by roughly thirty for no additional identity.
  for (const RankedCandidate& candidate : ranking) {
    canonical += candidate.key.to_string();
    canonical.push_back(':');
    append_u64(&canonical, static_cast<std::uint64_t>(candidate.score));
    canonical.push_back(':');
    append_u64(&canonical, candidate.rank);
    canonical.push_back(':');
    for (const FactorValue& factor : candidate.factors) {
      canonical.push_back(static_cast<char>(factor.factor));
      canonical.push_back(factor.known ? '1' : '0');
    }
    canonical.push_back('|');
  }
  for (const RouteRejection& rejection : rejections) {
    append_u64(&canonical, rejection.model_id);
    canonical.push_back('/');
    append_u64(&canonical, rejection.model_generation.value());
    canonical.push_back('/');
    append_u64(&canonical, rejection.backend_id);
    canonical.push_back('/');
    append_u64(&canonical, rejection.backend_boot.value());
    canonical.push_back('/');
    canonical.push_back(static_cast<char>(rejection.code));
    canonical.push_back(';');
  }
  canonical.push_back('|');
  for (const CandidateKey& key : fallback_order) {
    canonical += key.to_string();
    canonical.push_back(';');
  }
  canonical.push_back('|');
  const auto append = [&canonical](std::uint64_t value) {
    append_u64(&canonical, value);
    canonical.push_back(',');
  };
  append(authority.router_epoch.value());
  append(authority.coordinator_epoch.value());
  append(authority.model_generation.value());
  append(authority.artifact_generation.value());
  append(authority.backend_generation.value());
  append(authority.backend_boot.value());
  append(authority.endpoint_generation.value());
  append(authority.capability_generation.value());
  append(authority.policy_generation.value());
  append(authority.budget_generation.value());
  append(authority.price_generation.value());
  append(authority.slo_generation.value());
  append(authority.compatibility_generation.value());
  append(authority.trust_generation.value());
  append(authority.health_generation.value());
  append(authority.availability_generation.value());
  append(authority.readiness_generation.value());
  append(authority.residency_generation.value());
  append(authority.capacity_generation.value());
  append(authority.reservation_generation.value());
  return detail::to_hex(detail::sha256(canonical));
}

void RouteExplanation::canonicalize() {
  std::sort(rejections.begin(), rejections.end(),
            [](const RouteRejection& lhs, const RouteRejection& rhs) {
              return lhs.canonical_less(rhs);
            });
  std::stable_sort(ranking.begin(), ranking.end(),
                   [](const RankedCandidate& lhs, const RankedCandidate& rhs) {
                     return candidate_ranks_before(lhs, rhs);
                   });
  for (std::size_t index = 0; index < ranking.size(); ++index) {
    ranking[index].rank = static_cast<std::uint32_t>(index + 1);
  }
  for (RankedCandidate& candidate : ranking) {
    std::sort(candidate.factors.begin(), candidate.factors.end(),
              [](const FactorValue& lhs, const FactorValue& rhs) {
                return lhs.factor < rhs.factor;
              });
  }
  // fallback_order is semantically ordered (rank order) and is never sorted:
  // it is an ordered set of alternative singular routes, not a set.
}

std::string RouteExplanation::to_text() const {
  std::string out;
  out += "request=";
  out += std::to_string(request_id.value());
  out.push_back('/');
  out += std::to_string(request_generation.value());
  out += "\nwinner=";
  append_key(&out, winner);
  out += "\neligible=";
  out += std::to_string(eligible_candidate_count);
  out += " rejected=";
  out += std::to_string(rejected_candidate_count);
  out += "\npolic=";
  out += std::string(to_string(policy_result));
  out += " currentness=";
  out += std::string(to_string(currentness));
  out += " revalidation_required=";
  out += revalidation_required ? "true" : "false";
  out += "\ntie_break=";
  out += tie_break_reason;
  out += "\nauthority_digest=";
  out += authority_digest(authority);
  out += "\nranking:";
  if (!detail_retained) {
    out += " (factor detail was not persisted; winner and bound authority are exact)";
  }
  for (const RankedCandidate& candidate : ranking) {
    out += "\n  #";
    out += std::to_string(candidate.rank);
    out += " score=";
    out += std::to_string(candidate.score);
    out.push_back(' ');
    append_key(&out, candidate.key);
    out += " factors=";
    bool first = true;
    for (const FactorValue& factor : candidate.factors) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      out += std::string(to_string(factor.factor));
      out.push_back(':');
      out += factor.known ? std::to_string(factor.normalized) : std::string("UNKNOWN");
    }
  }
  if (!rejections.empty() || !detail_retained) {
    out += "\nrejections:";
    if (!detail_retained && rejections.empty()) {
      out += " (not persisted)";
    }
    for (const RouteRejection& rejection : rejections) {
      out += "\n  model=";
      out += std::to_string(rejection.model_id);
      out.push_back('@');
      out += std::to_string(rejection.model_generation.value());
      out += " backend=";
      out += std::to_string(rejection.backend_id);
      out.push_back('@');
      out += std::to_string(rejection.backend_generation.value());
      out += ".boot=";
      out += std::to_string(rejection.backend_boot.value());
      out += " code=";
      out += std::string(to_string(rejection.code));
      if (!rejection.detail.empty()) {
        out.push_back(' ');
        out += rejection.detail;
      }
    }
  }
  if (!fallback_order.empty()) {
    out += "\nfallbacks:";
    for (const CandidateKey& key : fallback_order) {
      out += "\n  ";
      append_key(&out, key);
    }
  }
  out += "\ncost=";
  out += cost.unit.currency;
  out.push_back('/');
  out += cost.unit.basis;
  out.push_back('/');
  out += std::to_string(cost.estimated_total_micros);
  out += " knownness=";
  out += std::string(to_string(cost.knownness));
  out += " price_generation=";
  out += std::to_string(cost.price_generation.value());
  out += "\nslo=";
  out += std::to_string(slo.slo_id.value());
  out.push_back('/');
  out += std::to_string(slo.generation.value());
  out.push_back('/');
  out += std::string(to_string(slo.verdict));
  out += "\nbudget=";
  out += std::to_string(budget.budget_id.value());
  out.push_back('/');
  out += std::to_string(budget.generation.value());
  out.push_back('/');
  out += std::string(to_string(budget.verdict));
  out += "\npolicy=";
  out += std::to_string(policy.policy_id.value());
  out.push_back('/');
  out += std::to_string(policy.generation.value());
  out += "\nrequirement_digest=";
  out += requirement_digest;
  out += "\nsemantic_digest=";
  out += semantic_digest;
  return out;
}

std::string RouteExplanation::to_json() const {
  std::string out = "{\"request\":\"";
  out += std::to_string(request_id.value());
  out.push_back('/');
  out += std::to_string(request_generation.value());
  out += "\",\"winner\":\"";
  out += escape_json(winner.to_string());
  out += "\",\"eligible\":";
  out += std::to_string(eligible_candidate_count);
  out += ",\"rejected\":";
  out += std::to_string(rejected_candidate_count);
  out += ",\"policy_result\":\"";
  out += std::string(to_string(policy_result));
  out += "\",\"currentness\":\"";
  out += std::string(to_string(currentness));
  out += "\",\"revalidation_required\":";
  out += revalidation_required ? "true" : "false";
  out += ",\"tie_break\":\"";
  out += escape_json(tie_break_reason);
  out += "\",\"authority_digest\":\"";
  out += authority_digest(authority);
  out += "\",\"detail_retained\":";
  out += detail_retained ? "true" : "false";
  out += ",\"ranking\":[";
  bool first_candidate = true;
  for (const RankedCandidate& candidate : ranking) {
    if (!first_candidate) {
      out.push_back(',');
    }
    first_candidate = false;
    out += "{\"rank\":";
    out += std::to_string(candidate.rank);
    out += ",\"score\":";
    out += std::to_string(candidate.score);
    out += ",\"candidate\":\"";
    out += escape_json(candidate.key.to_string());
    out += "\",\"factors\":{";
    bool first_factor = true;
    for (const FactorValue& factor : candidate.factors) {
      if (!first_factor) {
        out.push_back(',');
      }
      first_factor = false;
      out.push_back('"');
      out += std::string(to_string(factor.factor));
      out += "\":{";
      out += "\"known\":";
      out += factor.known ? "true" : "false";
      out += ",\"value\":";
      out += std::to_string(factor.normalized);
      out += ",\"raw\":";
      out += std::to_string(factor.raw);
      out += ",\"weight\":";
      out += std::to_string(factor.weight_ppm);
      out += ",\"contribution\":";
      out += std::to_string(factor.contribution);
      out += ",\"generation\":";
      out += std::to_string(factor.evidence_generation);
      out += ",\"source\":\"";
      out += escape_json(factor.source);
      out += "\"}";
    }
    out += "}}";
  }
  out += "],\"rejections\":[";
  bool first_rejection = true;
  for (const RouteRejection& rejection : rejections) {
    if (!first_rejection) {
      out.push_back(',');
    }
    first_rejection = false;
    out += "{\"model\":";
    out += std::to_string(rejection.model_id);
    out += ",\"model_generation\":";
    out += std::to_string(rejection.model_generation.value());
    out += ",\"backend\":";
    out += std::to_string(rejection.backend_id);
    out += ",\"backend_generation\":";
    out += std::to_string(rejection.backend_generation.value());
    out += ",\"backend_boot\":";
    out += std::to_string(rejection.backend_boot.value());
    out += ",\"endpoint\":";
    out += std::to_string(rejection.endpoint_id);
    out += ",\"code\":\"";
    out += std::string(to_string(rejection.code));
    out += "\",\"detail\":\"";
    out += escape_json(rejection.detail);
    out += "\"}";
  }
  out += "],\"fallbacks\":[";
  bool first_fallback = true;
  for (const CandidateKey& key : fallback_order) {
    if (!first_fallback) {
      out.push_back(',');
    }
    first_fallback = false;
    out.push_back('"');
    out += escape_json(key.to_string());
    out.push_back('"');
  }
  out += "],\"cost\":{\"currency\":\"";
  out += escape_json(cost.unit.currency);
  out += "\",\"basis\":\"";
  out += escape_json(cost.unit.basis);
  out += "\",\"estimated_total_micros\":";
  out += std::to_string(cost.estimated_total_micros);
  out += ",\"knownness\":\"";
  out += std::string(to_string(cost.knownness));
  out += "\",\"price_generation\":";
  out += std::to_string(cost.price_generation.value());
  out += "},\"slo\":{\"id\":";
  out += std::to_string(slo.slo_id.value());
  out += ",\"generation\":";
  out += std::to_string(slo.generation.value());
  out += ",\"verdict\":\"";
  out += std::string(to_string(slo.verdict));
  out += "\"},\"budget\":{\"id\":";
  out += std::to_string(budget.budget_id.value());
  out += ",\"generation\":";
  out += std::to_string(budget.generation.value());
  out += ",\"verdict\":\"";
  out += std::string(to_string(budget.verdict));
  out += "\"},\"policy\":{\"id\":";
  out += std::to_string(policy.policy_id.value());
  out += ",\"generation\":";
  out += std::to_string(policy.generation.value());
  out += "},\"requirement_digest\":\"";
  out += requirement_digest;
  out += "\",\"semantic_digest\":\"";
  out += semantic_digest;
  out += "\"}";
  return out;
}

}  // namespace model_router
