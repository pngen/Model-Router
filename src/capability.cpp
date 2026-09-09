// Model Router - capability identity and evidence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/capability.hpp"

#include <algorithm>

namespace model_router {
namespace {

constexpr std::size_t kMaxCapabilityKeyBytes = 128;

[[nodiscard]] bool is_key_character(char ch) noexcept {
  return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' ||
         ch == '_' || ch == '/';
}

}  // namespace

std::string_view to_string(CapabilityState state) noexcept {
  switch (state) {
    case CapabilityState::UNKNOWN:
      return "UNKNOWN";
    case CapabilityState::DECLARED:
      return "DECLARED";
    case CapabilityState::OBSERVED:
      return "OBSERVED";
    case CapabilityState::VERIFIED:
      return "VERIFIED";
    case CapabilityState::REVOKED:
      return "REVOKED";
    case CapabilityState::STALE:
      return "STALE";
    case CapabilityState::UNSUPPORTED:
      return "UNSUPPORTED";
    default:
      return "INVALID";
  }
}

bool is_satisfying(CapabilityState state) noexcept {
  return state == CapabilityState::DECLARED || state == CapabilityState::OBSERVED ||
         state == CapabilityState::VERIFIED;
}

std::uint8_t evidence_strength(CapabilityState state) noexcept {
  switch (state) {
    case CapabilityState::UNKNOWN:
      return 0;
    case CapabilityState::DECLARED:
      return 1;
    case CapabilityState::OBSERVED:
      return 2;
    case CapabilityState::VERIFIED:
      return 3;
    default:
      return 0;
  }
}

CapabilityKey::CapabilityKey(std::string value) : value_(std::move(value)) {}

std::string CapabilityKey::validate(std::string_view value) {
  if (value.empty()) {
    return "capability key is empty";
  }
  if (value.size() > kMaxCapabilityKeyBytes) {
    return "capability key exceeds " + std::to_string(kMaxCapabilityKeyBytes) + " bytes";
  }
  if (value.front() == '/' || value.back() == '/') {
    return "capability key has a leading or trailing separator";
  }
  bool previous_separator = false;
  bool has_separator = false;
  for (const char ch : value) {
    if (!is_key_character(ch)) {
      return std::string("capability key contains an invalid character");
    }
    if (ch == '/') {
      if (previous_separator) {
        return "capability key contains an empty segment";
      }
      previous_separator = true;
      has_separator = true;
    } else {
      previous_separator = false;
    }
  }
  if (!has_separator) {
    return "capability key must be namespaced, for example domain/name";
  }
  return {};
}

std::string CapabilityKey::canonicalize(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char raw : value) {
    char ch = raw;
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (ch == '_' || ch == '.') {
      ch = '-';
    }
    if (ch == ' ' || ch == '\t') {
      ch = '-';
    }
    out.push_back(ch);
  }
  // Collapse repeated separators and trim them.
  std::string collapsed;
  collapsed.reserve(out.size());
  bool previous_separator = false;
  for (const char ch : out) {
    if (ch == '/') {
      if (collapsed.empty() || previous_separator) {
        continue;
      }
      previous_separator = true;
    } else {
      previous_separator = false;
    }
    collapsed.push_back(ch);
  }
  while (!collapsed.empty() && collapsed.back() == '/') {
    collapsed.pop_back();
  }
  if (!validate(collapsed).empty()) {
    return {};
  }
  return collapsed;
}

bool CapabilityProfile::set(CapabilityEvidence evidence, std::uint32_t max_entries) {
  const auto existing = std::lower_bound(
      entries_.begin(), entries_.end(), evidence.key,
      [](const CapabilityEvidence& entry, const CapabilityKey& key) { return entry.key < key; });
  if (existing != entries_.end() && existing->key == evidence.key) {
    *existing = std::move(evidence);
    return true;
  }
  if (entries_.size() >= max_entries) {
    return false;
  }
  entries_.insert(existing, std::move(evidence));
  return true;
}

bool CapabilityProfile::erase(const CapabilityKey& key) {
  const auto existing = std::lower_bound(
      entries_.begin(), entries_.end(), key,
      [](const CapabilityEvidence& entry, const CapabilityKey& value) { return entry.key < value; });
  if (existing == entries_.end() || !(existing->key == key)) {
    return false;
  }
  entries_.erase(existing);
  return true;
}

const CapabilityEvidence* CapabilityProfile::find(const CapabilityKey& key) const noexcept {
  const auto existing = std::lower_bound(
      entries_.begin(), entries_.end(), key,
      [](const CapabilityEvidence& entry, const CapabilityKey& value) { return entry.key < value; });
  if (existing == entries_.end() || !(existing->key == key)) {
    return nullptr;
  }
  return &*existing;
}

void CapabilityProfile::sort_entries() {
  std::stable_sort(entries_.begin(), entries_.end(),
                   [](const CapabilityEvidence& lhs, const CapabilityEvidence& rhs) {
                     return lhs.key < rhs.key;
                   });
}

std::string CapabilityProfile::evaluate(const CapabilityRequirement& requirement,
                                        UnixMillis now) const {
  if (requirement.key.empty()) {
    return "required capability key is empty";
  }
  const CapabilityEvidence* evidence = find(requirement.key);
  if (evidence == nullptr) {
    return "capability " + requirement.key.value() + " has no evidence";
  }
  if (evidence->state == CapabilityState::UNKNOWN) {
    return "capability " + requirement.key.value() + " is UNKNOWN";
  }
  if (!is_satisfying(evidence->state)) {
    return "capability " + requirement.key.value() + " is " +
           std::string(to_string(evidence->state));
  }
  if (evidence->expired(now)) {
    return "capability " + requirement.key.value() + " evidence is stale";
  }
  if (evidence_strength(evidence->state) < evidence_strength(requirement.minimum_state)) {
    return "capability " + requirement.key.value() + " evidence is " +
           std::string(to_string(evidence->state)) + " but " +
           std::string(to_string(requirement.minimum_state)) + " is required";
  }
  return {};
}

}  // namespace model_router
