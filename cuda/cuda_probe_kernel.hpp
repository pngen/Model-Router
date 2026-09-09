// Model Router - deterministic CUDA probe kernel and its matching CPU reference.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// This unit performs no model inference and no LLM execution. It exists so that
// a backend worker can prove REAL local CUDA capability: a fixed integer hash
// over an index range whose device result can be compared bit-for-bit against a
// host reference. The transform uses integer arithmetic only, so host and
// device results are exactly comparable and reproducible across runs.

#ifndef MODEL_ROUTER_CUDA_CUDA_PROBE_KERNEL_HPP
#define MODEL_ROUTER_CUDA_CUDA_PROBE_KERNEL_HPP

#include <cstdint>

#if defined(__CUDACC__)
#define MODEL_ROUTER_PROBE_HD __host__ __device__
#else
#define MODEL_ROUTER_PROBE_HD
#endif

namespace model_router::cuda::detail {

/// Fixed probe seed. It is a constant, never a random value: the probe must
/// produce the same result on every run, process, and machine.
inline constexpr std::uint64_t kProbeSeed = 0x9E3779B97F4A7C15ull;

/// Threads per block used by the probe launch.
inline constexpr std::uint32_t kProbeThreadsPerBlock = 256u;

/// Deterministic 64-bit integer mix (SplitMix64-style finalizer over the input
/// value combined with the element index and the fixed seed). Pure integer
/// arithmetic with defined unsigned wrap-around: identical on host and device,
/// with no floating point anywhere.
[[nodiscard]] MODEL_ROUTER_PROBE_HD inline std::uint64_t probe_transform(std::uint64_t value,
                                                                         std::uint64_t index,
                                                                         std::uint64_t seed)
    noexcept {
  std::uint64_t mixed = value ^ (index * 0x9E3779B97F4A7C15ull + seed);
  mixed = (mixed ^ (mixed >> 30u)) * 0xBF58476D1CE4E5B9ull;
  mixed = (mixed ^ (mixed >> 27u)) * 0x94D049BB133111EBull;
  return mixed ^ (mixed >> 31u);
}

/// CPU reference implementation of the probe transform over p count elements.
/// Defined in cuda_probe_kernel.cu so the host reference and the device kernel
/// share exactly one transform definition.
void probe_reference(const std::uint64_t* input, std::uint64_t* output, std::uint32_t count,
                     std::uint64_t seed) noexcept;

/// Launches the deterministic probe kernel over p count elements. Returns the
/// CUDA error code observed immediately after the launch (0 on success). It
/// never throws and never synchronizes: the caller synchronizes and checks the
/// launch error separately.
[[nodiscard]] int launch_probe_kernel(const std::uint64_t* device_input,
                                      std::uint64_t* device_output, std::uint32_t count,
                                      std::uint64_t seed) noexcept;

}  // namespace model_router::cuda::detail

#undef MODEL_ROUTER_PROBE_HD

#endif  // MODEL_ROUTER_CUDA_CUDA_PROBE_KERNEL_HPP
