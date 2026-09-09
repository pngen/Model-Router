// Model Router - policy-, capability-, cost-, and authority-aware routing runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef MODEL_ROUTER_VERSION_HPP
#define MODEL_ROUTER_VERSION_HPP

#include <cstdint>
#include <string>
#include <string_view>

#define MODEL_ROUTER_VERSION_MAJOR 1
#define MODEL_ROUTER_VERSION_MINOR 0
#define MODEL_ROUTER_VERSION_PATCH 0

namespace model_router {

inline constexpr std::uint32_t version_major = MODEL_ROUTER_VERSION_MAJOR;
inline constexpr std::uint32_t version_minor = MODEL_ROUTER_VERSION_MINOR;
inline constexpr std::uint32_t version_patch = MODEL_ROUTER_VERSION_PATCH;

/// Compile-time version number encoded as (major << 16) | (minor << 8) | patch.
inline constexpr std::uint32_t version_number =
    (version_major << 16) | (version_minor << 8) | version_patch;

/// Human readable semantic version, for example "1.0.0".
[[nodiscard]] inline std::string_view version_string() noexcept { return "1.0.0"; }

/// Full product identification string, for example "Model Router 1.0.0".
[[nodiscard]] inline std::string product_string() {
  return std::string("Model Router ") + std::string(version_string());
}

/// Durable persistence format version. Changing the on-disk record layout
/// requires bumping this value or adding a decoder for the older version.
inline constexpr std::uint32_t persistence_format_version = 1;

/// Reference framed-protocol version used by the multiprocess reference system.
inline constexpr std::uint32_t protocol_version = 1;

}  // namespace model_router

#endif  // MODEL_ROUTER_VERSION_HPP
