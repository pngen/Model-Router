// Model Router - injectable time source.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_CLOCK_HPP
#define MODEL_ROUTER_CLOCK_HPP

#include <cstdint>
#include <memory>

namespace model_router {

/// Wall-clock timestamp in milliseconds since the Unix epoch. This is the only
/// time representation that may be persisted: it keeps a well-defined meaning
/// after a restart. Process-local monotonic values are never persisted as if
/// they were still current.
using UnixMillis = std::int64_t;
/// Process-local monotonic milliseconds. Meaningless across processes.
using MonotonicMillis = std::uint64_t;

/// Sentinel meaning "no expiry / no bound".
inline constexpr UnixMillis kNoExpiry = 0;

/// Injected time source. Freshness, expiry, and currentness semantics must never
/// depend on the real passage of time: tests supply a logical clock and move it
/// explicitly instead of sleeping.
class Clock {
 public:
  virtual ~Clock() = default;

  [[nodiscard]] virtual UnixMillis now_unix_millis() const = 0;
  [[nodiscard]] virtual MonotonicMillis now_monotonic_millis() const = 0;
};

/// Production clock backed by the host steady clock and system clock.
class SystemClock final : public Clock {
 public:
  [[nodiscard]] UnixMillis now_unix_millis() const override;
  [[nodiscard]] MonotonicMillis now_monotonic_millis() const override;
};

/// Deterministic logical clock. Time only moves when the caller moves it.
class LogicalClock final : public Clock {
 public:
  explicit LogicalClock(UnixMillis start_unix_millis = 1700000000000LL,
                        MonotonicMillis start_monotonic_millis = 1000) noexcept
      : unix_millis_(start_unix_millis), monotonic_millis_(start_monotonic_millis) {}

  [[nodiscard]] UnixMillis now_unix_millis() const override { return unix_millis_; }
  [[nodiscard]] MonotonicMillis now_monotonic_millis() const override { return monotonic_millis_; }

  void advance(std::uint64_t millis) noexcept {
    unix_millis_ += static_cast<UnixMillis>(millis);
    monotonic_millis_ += millis;
  }
  void set_unix_millis(UnixMillis value) noexcept { unix_millis_ = value; }
  void set_monotonic_millis(MonotonicMillis value) noexcept { monotonic_millis_ = value; }

 private:
  UnixMillis unix_millis_;
  MonotonicMillis monotonic_millis_;
};

/// True when a timestamped observation with the given expiry is still current.
[[nodiscard]] bool is_current(UnixMillis now, UnixMillis expires_at) noexcept;

}  // namespace model_router

#endif  // MODEL_ROUTER_CLOCK_HPP
