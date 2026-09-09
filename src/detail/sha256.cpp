// Model Router - SHA-256 semantic digest.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "model_router/detail/sha256.hpp"

#include <cstring>

namespace model_router::detail {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t bits) noexcept {
  return (value >> bits) | (value << (32u - bits));
}

}  // namespace

void Sha256::transform(const std::uint8_t* block) noexcept {
  std::uint32_t schedule[64];
  for (std::uint32_t index = 0; index < 16; ++index) {
    const std::size_t base = static_cast<std::size_t>(index) * 4u;
    schedule[index] = (static_cast<std::uint32_t>(block[base]) << 24) |
                      (static_cast<std::uint32_t>(block[base + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[base + 2]) << 8) |
                      static_cast<std::uint32_t>(block[base + 3]);
  }
  for (std::uint32_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15], 7) ^ rotr(schedule[index - 15], 18) ^
                             (schedule[index - 15] >> 3);
    const std::uint32_t s1 = rotr(schedule[index - 2], 17) ^ rotr(schedule[index - 2], 19) ^
                             (schedule[index - 2] >> 10);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::uint32_t index = 0; index < 64; ++index) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[index] + schedule[index];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
  if (data == nullptr || size == 0) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += size;

  std::size_t offset = 0;
  if (buffer_size_ != 0) {
    const std::size_t needed = 64 - buffer_size_;
    const std::size_t taken = size < needed ? size : needed;
    std::memcpy(buffer_ + buffer_size_, bytes, taken);
    buffer_size_ += taken;
    offset += taken;
    if (buffer_size_ == 64) {
      transform(buffer_);
      buffer_size_ = 0;
    }
  }

  while (offset + 64 <= size) {
    transform(bytes + offset);
    offset += 64;
  }

  if (offset < size) {
    const std::size_t remaining = size - offset;
    std::memcpy(buffer_, bytes + offset, remaining);
    buffer_size_ = remaining;
  }
}

void Sha256::update(std::span<const std::uint8_t> bytes) noexcept {
  update(bytes.data(), bytes.size());
}

void Sha256::update(std::string_view text) noexcept { update(text.data(), text.size()); }

std::array<std::uint8_t, 32> Sha256::finish() noexcept {
  const std::uint64_t bit_length = total_bytes_ * 8ull;

  std::uint8_t padding[72];
  std::memset(padding, 0, sizeof(padding));
  padding[0] = 0x80u;
  const std::size_t pad_length = buffer_size_ < 56 ? (56 - buffer_size_) : (120 - buffer_size_);
  update(padding, pad_length);

  std::uint8_t length_bytes[8];
  for (std::size_t index = 0; index < 8; ++index) {
    length_bytes[index] = static_cast<std::uint8_t>(bit_length >> ((7u - index) * 8u));
  }
  // update() would count these bytes; total_bytes_ is no longer used.
  const std::uint64_t saved_total = total_bytes_;
  update(length_bytes, sizeof(length_bytes));
  total_bytes_ = saved_total;

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t index = 0; index < 8; ++index) {
    digest[index * 4] = static_cast<std::uint8_t>(state_[index] >> 24);
    digest[index * 4 + 1] = static_cast<std::uint8_t>(state_[index] >> 16);
    digest[index * 4 + 2] = static_cast<std::uint8_t>(state_[index] >> 8);
    digest[index * 4 + 3] = static_cast<std::uint8_t>(state_[index]);
  }
  return digest;
}

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes) noexcept {
  Sha256 hasher;
  hasher.update(bytes);
  return hasher.finish();
}

std::array<std::uint8_t, 32> sha256(std::string_view text) noexcept {
  Sha256 hasher;
  hasher.update(text);
  return hasher.finish();
}

std::string to_hex(const std::array<std::uint8_t, 32>& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const std::uint8_t byte : digest) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0Fu]);
  }
  return out;
}

}  // namespace model_router::detail
