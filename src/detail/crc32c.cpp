// Model Router - CRC-32C (Castagnoli) checksum.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/detail/crc32c.hpp"

#include <array>

namespace model_router::detail {
namespace {

/// Reflected CRC-32C polynomial 0x1EDC6F41, reversed: 0x82F63B78.
constexpr std::uint32_t kPolynomial = 0x82F63B78u;

struct Table {
  std::array<std::uint32_t, 256> entries{};

  constexpr Table() noexcept {
    for (std::uint32_t index = 0; index < 256; ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) != 0u ? (value >> 1) ^ kPolynomial : (value >> 1);
      }
      entries[index] = value;
    }
  }
};

constexpr Table kTable{};

}  // namespace

void Crc32c::update(std::span<const std::uint8_t> bytes) noexcept {
  std::uint32_t state = state_;
  for (const std::uint8_t byte : bytes) {
    state = kTable.entries[(state ^ byte) & 0xFFu] ^ (state >> 8);
  }
  state_ = state;
}

void Crc32c::update(const void* data, std::size_t size) noexcept {
  if (data == nullptr || size == 0) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  update(std::span<const std::uint8_t>(bytes, size));
}

std::uint32_t Crc32c::value() const noexcept { return state_ ^ 0xFFFFFFFFu; }

std::uint32_t crc32c(std::span<const std::uint8_t> bytes) noexcept {
  Crc32c crc;
  crc.update(bytes);
  return crc.value();
}

std::uint32_t crc32c(const void* data, std::size_t size) noexcept {
  Crc32c crc;
  crc.update(data, size);
  return crc.value();
}

}  // namespace model_router::detail
