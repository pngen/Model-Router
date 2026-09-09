// Model Router - SHA-256 semantic digest. Implementation detail.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_DETAIL_SHA256_HPP
#define MODEL_ROUTER_DETAIL_SHA256_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace model_router::detail {

/// SHA-256. Used for semantic digests of persisted state, route decisions, and
/// explanations. It is a content fingerprint, not a security primitive for
/// authentication.
class Sha256 {
 public:
  Sha256() = default;

  void update(std::span<const std::uint8_t> bytes) noexcept;
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept;
  /// Finalizes the digest. The object must not be updated afterwards.
  [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept;

 private:
  void transform(const std::uint8_t* block) noexcept;

  std::uint32_t state_[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  std::uint64_t total_bytes_{0};
  std::uint8_t buffer_[64]{};
  std::size_t buffer_size_{0};
};

[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::string_view text) noexcept;
/// Lower-case hexadecimal rendering of a digest.
[[nodiscard]] std::string to_hex(const std::array<std::uint8_t, 32>& digest);

}  // namespace model_router::detail

#endif  // MODEL_ROUTER_DETAIL_SHA256_HPP
