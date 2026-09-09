// Model Router - optional real CUDA backend proof.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// This module proves that a backend worker can establish REAL local CUDA
// capability evidence bound to its current process incarnation. Model Router
// never performs model inference; no LLM execution is claimed here.

#ifndef MODEL_ROUTER_CUDA_CUDA_REFERENCE_PROBE_HPP
#define MODEL_ROUTER_CUDA_CUDA_REFERENCE_PROBE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace model_router::cuda {

/// Result of a real device probe. Every field is observed on this host, never
/// inferred from configuration.
struct DeviceProbe {
  bool available{false};
  std::uint32_t device_count{0};
  std::string device_name;
  std::uint32_t compute_capability_major{0};
  std::uint32_t compute_capability_minor{0};
  std::uint64_t total_memory_bytes{0};
  std::uint64_t free_memory_before_bytes{0};
  std::uint64_t free_memory_after_bytes{0};
  /// Elements transferred host-to-device and device-to-host.
  std::uint32_t element_count{0};
  /// True when the device result matched the CPU reference result exactly.
  bool parity_verified{false};
  /// Deterministic checksum of the device result.
  std::uint64_t result_checksum{0};
  /// True when the device free memory returned to at least the pre-probe value.
  bool memory_restored{false};
  std::string detail;

  [[nodiscard]] std::string to_json() const;
  [[nodiscard]] std::string to_text() const;
};

/// Runs a real deterministic CUDA proof on the current process:
/// enumerate device, allocate device memory, host-to-device copy, launch a
/// deterministic kernel, synchronize, device-to-host copy, verify CPU parity,
/// free device memory. Returns available=false with a detail message when no
/// usable CUDA device or runtime is present.
[[nodiscard]] DeviceProbe run_device_probe(std::uint32_t element_count);

/// True when this build was compiled with CUDA support.
[[nodiscard]] bool compiled_with_cuda() noexcept;

/// Human-readable CUDA runtime version, or an empty string when unavailable.
[[nodiscard]] std::string runtime_version();

}  // namespace model_router::cuda

#endif  // MODEL_ROUTER_CUDA_CUDA_REFERENCE_PROBE_HPP
