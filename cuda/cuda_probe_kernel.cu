// Model Router - deterministic CUDA probe kernel and CPU reference.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "cuda_probe_kernel.hpp"

#include <cuda_runtime.h>

namespace model_router::cuda::detail {
namespace {

/// One element per thread. p input and p output are distinct device buffers.
__global__ void probe_kernel(const std::uint64_t* input, std::uint64_t* output,
                             std::uint32_t count, std::uint64_t seed) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index < count) {
    output[index] = probe_transform(input[index], static_cast<std::uint64_t>(index), seed);
  }
}

}  // namespace

void probe_reference(const std::uint64_t* input, std::uint64_t* output, std::uint32_t count,
                     std::uint64_t seed) noexcept {
  for (std::uint32_t index = 0; index < count; ++index) {
    output[index] = probe_transform(input[index], static_cast<std::uint64_t>(index), seed);
  }
}

int launch_probe_kernel(const std::uint64_t* device_input, std::uint64_t* device_output,
                        std::uint32_t count, std::uint64_t seed) noexcept {
  if (count == 0) {
    return static_cast<int>(cudaErrorInvalidValue);
  }
  const std::uint32_t blocks = (count + kProbeThreadsPerBlock - 1u) / kProbeThreadsPerBlock;
  probe_kernel<<<blocks, kProbeThreadsPerBlock>>>(device_input, device_output, count, seed);
  return static_cast<int>(cudaGetLastError());
}

}  // namespace model_router::cuda::detail
