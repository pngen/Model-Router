// Model Router - identity allocation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/ids.hpp"

#include <atomic>

namespace model_router {
namespace {

constexpr std::uint64_t kCeiling = 0xFFFFFFFFFFFFFFFFull;
std::atomic<std::uint64_t> g_highest_issued{0};

}  // namespace

std::uint64_t IdAllocator::next() noexcept {
  std::uint64_t observed = g_highest_issued.load(std::memory_order_relaxed);
  for (;;) {
    if (observed >= kCeiling) {
      // The identity space is exhausted. Returning 0 would alias the invalid
      // identity, so the ceiling value is returned and never wraps.
      return kCeiling;
    }
    const std::uint64_t desired = observed + 1;
    if (g_highest_issued.compare_exchange_weak(observed, desired, std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
      return desired;
    }
  }
}

void IdAllocator::observe(std::uint64_t highest_seen) noexcept {
  if (highest_seen > kCeiling) {
    return;
  }
  std::uint64_t observed = g_highest_issued.load(std::memory_order_relaxed);
  while (observed < highest_seen) {
    if (g_highest_issued.compare_exchange_weak(observed, highest_seen, std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
      return;
    }
  }
}

std::uint64_t IdAllocator::highest_issued() noexcept {
  return g_highest_issued.load(std::memory_order_relaxed);
}

}  // namespace model_router
