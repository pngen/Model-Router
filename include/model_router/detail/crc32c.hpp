// Model Router - CRC-32C (Castagnoli) checksum. Implementation detail.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_DETAIL_CRC32C_HPP
#define MODEL_ROUTER_DETAIL_CRC32C_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace model_router::detail {

/// Incremental CRC-32C. Used to detect corruption in framed transport messages
/// and in the persistence header and payload.
class Crc32c {
 public:
  Crc32c() = default;

  void update(std::span<const std::uint8_t> bytes) noexcept;
  void update(const void* data, std::size_t size) noexcept;
  [[nodiscard]] std::uint32_t value() const noexcept;

 private:
  std::uint32_t state_{0xFFFFFFFFu};
};

[[nodiscard]] std::uint32_t crc32c(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t size) noexcept;

}  // namespace model_router::detail

#endif  // MODEL_ROUTER_DETAIL_CRC32C_HPP
