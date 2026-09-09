// Model Router - optional real CUDA backend proof.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Every field of DeviceProbe is observed on this host: the device is enumerated
// through the CUDA runtime, real device memory is allocated, a real
// host-to-device copy is performed, the deterministic kernel is launched, the
// launch is synchronized, the result is copied back, exact parity against the
// CPU reference is verified, a deterministic checksum is computed, and the
// device memory is released. Model Router never performs model inference: no
// LLM execution is claimed here, only device capability evidence.

#include "model_router/cuda/cuda_reference_probe.hpp"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include "cuda_probe_kernel.hpp"

namespace model_router::cuda {
namespace {

using detail::kProbeSeed;
using detail::kProbeThreadsPerBlock;

/// FNV-1a 64-bit offset basis and prime. The checksum folds the device result
/// in element order, so it is reproducible on any host.
constexpr std::uint64_t kChecksumOffsetBasis = 0xCBF29CE484222325ull;
constexpr std::uint64_t kChecksumPrime = 0x100000001B3ull;

/// Index of the device the probe runs on.
constexpr int kProbeDeviceIndex = 0;

[[nodiscard]] std::string cuda_error_text(cudaError_t error) {
  const char* name = cudaGetErrorName(error);
  const char* text = cudaGetErrorString(error);
  std::string result = name != nullptr ? std::string(name) : std::string("cudaErrorUnknown");
  result += ": ";
  result += text != nullptr ? std::string(text) : std::string("no description");
  return result;
}

/// Owning handle for one device allocation. Releasing is explicit so the probe
/// can report a cudaFree failure, and the destructor releases anything still
/// owned when a step fails, so no device memory is leaked on any path.
class DeviceAllocation {
 public:
  DeviceAllocation() noexcept = default;
  DeviceAllocation(const DeviceAllocation&) = delete;
  DeviceAllocation& operator=(const DeviceAllocation&) = delete;
  ~DeviceAllocation() { (void)release(); }

  [[nodiscard]] cudaError_t allocate(std::size_t bytes) noexcept {
    return cudaMalloc(&pointer_, bytes);
  }

  [[nodiscard]] cudaError_t release() noexcept {
    if (pointer_ == nullptr) {
      return cudaSuccess;
    }
    void* pointer = pointer_;
    pointer_ = nullptr;
    return cudaFree(pointer);
  }

  [[nodiscard]] std::uint64_t* data() const noexcept {
    return static_cast<std::uint64_t*>(pointer_);
  }

 private:
  void* pointer_{nullptr};
};

/// Runs one complete allocate / host-to-device copy / launch / synchronize /
/// device-to-host copy / free cycle over p element_count elements. Returns the
/// first CUDA error observed (cudaSuccess when the whole cycle succeeded) and
/// reports the failing step through p stage.
[[nodiscard]] cudaError_t run_probe_cycle(const std::vector<std::uint64_t>& host_input,
                                          std::vector<std::uint64_t>* host_output,
                                          std::uint32_t element_count, std::size_t bytes,
                                          const char** stage) {
  *stage = "cudaMalloc(device input)";
  DeviceAllocation device_input;
  cudaError_t error = device_input.allocate(bytes);
  if (error != cudaSuccess) {
    return error;
  }

  *stage = "cudaMalloc(device output)";
  DeviceAllocation device_output;
  error = device_output.allocate(bytes);
  if (error != cudaSuccess) {
    return error;
  }

  *stage = "cudaMemcpy(host to device)";
  error = cudaMemcpy(device_input.data(), host_input.data(), bytes, cudaMemcpyHostToDevice);
  if (error != cudaSuccess) {
    return error;
  }

  *stage = "kernel launch";
  const int launch_error =
      detail::launch_probe_kernel(device_input.data(), device_output.data(), element_count, kProbeSeed);
  if (launch_error != static_cast<int>(cudaSuccess)) {
    return static_cast<cudaError_t>(launch_error);
  }

  *stage = "cudaDeviceSynchronize";
  error = cudaDeviceSynchronize();
  if (error != cudaSuccess) {
    return error;
  }

  *stage = "cudaMemcpy(device to host)";
  error = cudaMemcpy(host_output->data(), device_output.data(), bytes, cudaMemcpyDeviceToHost);
  if (error != cudaSuccess) {
    return error;
  }

  *stage = "cudaFree(device output)";
  error = device_output.release();
  if (error != cudaSuccess) {
    return error;
  }

  *stage = "cudaFree(device input)";
  return device_input.release();
}

/// Deterministic FNV-1a fold over the device result in element order.
[[nodiscard]] std::uint64_t checksum_of(const std::vector<std::uint64_t>& values) noexcept {
  std::uint64_t checksum = kChecksumOffsetBasis;
  for (const std::uint64_t value : values) {
    checksum ^= value;
    checksum *= kChecksumPrime;
  }
  return checksum;
}

[[nodiscard]] std::string json_escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (const char character : value) {
    switch (character) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += character;
        break;
    }
  }
  return escaped;
}

[[nodiscard]] const char* bool_text(bool value) noexcept { return value ? "true" : "false"; }

[[nodiscard]] std::string hex64(std::uint64_t value) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string result = "0x";
  bool started = false;
  for (int shift = 60; shift >= 0; shift -= 4) {
    const std::uint32_t digit = static_cast<std::uint32_t>((value >> static_cast<unsigned>(shift)) & 0xFull);
    if (digit != 0 || started || shift == 0) {
      started = true;
      result += kDigits[digit];
    }
  }
  return result;
}

[[nodiscard]] std::string cuda_version_text(int version_code) {
  if (version_code <= 0) {
    return std::string();
  }
  const int major = version_code / 1000;
  const int minor = (version_code % 1000) / 10;
  return std::to_string(major) + "." + std::to_string(minor);
}

/// Fills p probe with available=false and p detail. Used by every failure path
/// so the function can never throw out to its caller.
[[nodiscard]] DeviceProbe unavailable(std::uint32_t element_count, std::string detail) {
  DeviceProbe probe;
  probe.element_count = element_count;
  probe.detail = std::move(detail);
  return probe;
}

}  // namespace

bool compiled_with_cuda() noexcept {
#if defined(MODEL_ROUTER_HAS_CUDA)
  return true;
#else
  return false;
#endif
}

std::string runtime_version() {
  if (!compiled_with_cuda()) {
    return std::string();
  }
  int version_code = 0;
  if (cudaRuntimeGetVersion(&version_code) != cudaSuccess) {
    return std::string();
  }
  return cuda_version_text(version_code);
}

std::string DeviceProbe::to_json() const {
  std::string json = "{";
  json += "\"available\":";
  json += bool_text(available);
  json += ",\"device_count\":" + std::to_string(device_count);
  json += ",\"device_name\":\"" + json_escape(device_name) + "\"";
  json += ",\"compute_capability_major\":" + std::to_string(compute_capability_major);
  json += ",\"compute_capability_minor\":" + std::to_string(compute_capability_minor);
  json += ",\"total_memory_bytes\":" + std::to_string(total_memory_bytes);
  json += ",\"free_memory_before_bytes\":" + std::to_string(free_memory_before_bytes);
  json += ",\"free_memory_after_bytes\":" + std::to_string(free_memory_after_bytes);
  json += ",\"element_count\":" + std::to_string(element_count);
  json += ",\"parity_verified\":";
  json += bool_text(parity_verified);
  json += ",\"result_checksum\":" + std::to_string(result_checksum);
  json += ",\"memory_restored\":";
  json += bool_text(memory_restored);
  json += ",\"detail\":\"" + json_escape(detail) + "\"}";
  return json;
}

std::string DeviceProbe::to_text() const {
  std::string text;
  text += "available: " + std::string(bool_text(available)) + "\n";
  text += "device_count: " + std::to_string(device_count) + "\n";
  text += "device_name: " + device_name + "\n";
  text += "compute_capability: " + std::to_string(compute_capability_major) + "." +
          std::to_string(compute_capability_minor) + "\n";
  text += "total_memory_bytes: " + std::to_string(total_memory_bytes) + "\n";
  text += "free_memory_before_bytes: " + std::to_string(free_memory_before_bytes) + "\n";
  text += "free_memory_after_bytes: " + std::to_string(free_memory_after_bytes) + "\n";
  text += "element_count: " + std::to_string(element_count) + "\n";
  text += "parity_verified: " + std::string(bool_text(parity_verified)) + "\n";
  text += "result_checksum: " + hex64(result_checksum) + "\n";
  text += "memory_restored: " + std::string(bool_text(memory_restored)) + "\n";
  text += "detail: " + detail + "\n";
  return text;
}

DeviceProbe run_device_probe(std::uint32_t element_count) {
  try {
    if (!compiled_with_cuda()) {
      return unavailable(element_count, "this build was not compiled with CUDA support");
    }
    if (element_count == 0) {
      return unavailable(element_count, "element_count must be greater than zero");
    }

    int device_count = 0;
    cudaError_t error = cudaGetDeviceCount(&device_count);
    if (error == cudaErrorNoDevice) {
      return unavailable(element_count, "no CUDA device is present: " + cuda_error_text(error));
    }
    if (error == cudaErrorInsufficientDriver) {
      return unavailable(element_count,
                         "the installed NVIDIA driver is insufficient for this CUDA runtime: " +
                             cuda_error_text(error));
    }
    if (error != cudaSuccess) {
      return unavailable(element_count, "cudaGetDeviceCount failed: " + cuda_error_text(error));
    }
    if (device_count <= 0) {
      return unavailable(element_count, "the CUDA runtime reported no device");
    }

    error = cudaSetDevice(kProbeDeviceIndex);
    if (error != cudaSuccess) {
      return unavailable(element_count, "cudaSetDevice failed: " + cuda_error_text(error));
    }

    cudaDeviceProp properties{};
    error = cudaGetDeviceProperties(&properties, kProbeDeviceIndex);
    if (error != cudaSuccess) {
      return unavailable(element_count, "cudaGetDeviceProperties failed: " + cuda_error_text(error));
    }

    int capability_major = 0;
    error = cudaDeviceGetAttribute(&capability_major, cudaDevAttrComputeCapabilityMajor,
                                   kProbeDeviceIndex);
    if (error != cudaSuccess) {
      return unavailable(element_count,
                         "cudaDeviceGetAttribute(compute capability major) failed: " +
                             cuda_error_text(error));
    }
    int capability_minor = 0;
    error = cudaDeviceGetAttribute(&capability_minor, cudaDevAttrComputeCapabilityMinor,
                                   kProbeDeviceIndex);
    if (error != cudaSuccess) {
      return unavailable(element_count,
                         "cudaDeviceGetAttribute(compute capability minor) failed: " +
                             cuda_error_text(error));
    }

    DeviceProbe probe;
    probe.element_count = element_count;
    probe.device_count = static_cast<std::uint32_t>(device_count);
    probe.device_name = properties.name;
    probe.compute_capability_major = static_cast<std::uint32_t>(capability_major);
    probe.compute_capability_minor = static_cast<std::uint32_t>(capability_minor);
    probe.total_memory_bytes = static_cast<std::uint64_t>(properties.totalGlobalMem);

    const std::size_t bytes = static_cast<std::size_t>(element_count) * sizeof(std::uint64_t);
    std::vector<std::uint64_t> host_input(element_count);
    std::vector<std::uint64_t> host_output(element_count);
    std::vector<std::uint64_t> host_reference(element_count);
    for (std::uint32_t index = 0; index < element_count; ++index) {
      host_input[index] = static_cast<std::uint64_t>(index) + 1u;
    }

    // cudaMemGetInfo creates the primary context when it does not exist yet, so
    // the fixed per-process CUDA context overhead is already accounted for: the
    // comparison below measures only the device memory this probe allocates.
    std::size_t free_before = 0;
    std::size_t total_before = 0;
    error = cudaMemGetInfo(&free_before, &total_before);
    if (error != cudaSuccess) {
      return unavailable(element_count, "cudaMemGetInfo (before) failed: " + cuda_error_text(error));
    }
    probe.free_memory_before_bytes = static_cast<std::uint64_t>(free_before);

    const char* stage = nullptr;
    error = run_probe_cycle(host_input, &host_output, element_count, bytes, &stage);
    if (error != cudaSuccess) {
      return unavailable(element_count, std::string("CUDA probe failed at ") + stage + ": " +
                                           cuda_error_text(error));
    }

    detail::probe_reference(host_input.data(), host_reference.data(), element_count, kProbeSeed);
    probe.parity_verified = host_output == host_reference;
    probe.result_checksum = checksum_of(host_output);

    std::size_t free_after = 0;
    std::size_t total_after = 0;
    error = cudaMemGetInfo(&free_after, &total_after);
    if (error != cudaSuccess) {
      return unavailable(element_count, "cudaMemGetInfo (after) failed: " + cuda_error_text(error));
    }
    probe.free_memory_after_bytes = static_cast<std::uint64_t>(free_after);
    probe.memory_restored = free_after >= free_before;

    int runtime_code = 0;
    int driver_code = 0;
    (void)cudaRuntimeGetVersion(&runtime_code);
    (void)cudaDriverGetVersion(&driver_code);

    probe.available = true;
    probe.detail = "device " + std::to_string(kProbeDeviceIndex) + " of " +
                   std::to_string(device_count) + ": " + probe.device_name +
                   "; compute_capability=" + std::to_string(probe.compute_capability_major) + "." +
                   std::to_string(probe.compute_capability_minor) +
                   "; total_memory_bytes=" + std::to_string(probe.total_memory_bytes) +
                   "; elements=" + std::to_string(probe.element_count) +
                   "; bytes_per_buffer=" + std::to_string(bytes) +
                   "; parity=" + std::string(probe.parity_verified ? "exact" : "mismatch") +
                   "; checksum=" + hex64(probe.result_checksum) +
                   "; memory_restored=(free_memory_after_bytes >= free_memory_before_bytes) => " +
                   std::string(probe.memory_restored ? "true" : "false") + " (" +
                   std::to_string(probe.free_memory_after_bytes) + " >= " +
                   std::to_string(probe.free_memory_before_bytes) +
                   "), where free_memory_before_bytes is read by cudaMemGetInfo after the CUDA "
                   "runtime has created its context, so the fixed per-process CUDA context "
                   "overhead is never charged to the probe" +
                   "; threads_per_block=" + std::to_string(kProbeThreadsPerBlock) +
                   "; cuda_runtime=" + cuda_version_text(runtime_code) +
                   "; cuda_driver_code=" + std::to_string(driver_code);
    return probe;
  } catch (const std::exception& failure) {
    return unavailable(element_count,
                       std::string("CUDA probe raised an exception: ") + failure.what());
  } catch (...) {
    return unavailable(element_count, "CUDA probe raised an unknown exception");
  }
}

}  // namespace model_router::cuda
