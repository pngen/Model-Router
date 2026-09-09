# Model Router - reference multiprocess architecture.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# SPDX-License-Identifier: Apache-2.0
#
# Real independent operating-system processes over framed loopback TCP on one
# physical host.

add_library(model_router_reference STATIC
  coordinator.cpp
  process.cpp)
target_include_directories(model_router_reference
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(model_router_reference
  PUBLIC model_router
  PRIVATE $<BUILD_INTERFACE:model_router_warnings>)
if(WIN32)
  target_link_libraries(model_router_reference PUBLIC ws2_32)
endif()
if(MODEL_ROUTER_CUDA_FOUND)
  target_link_libraries(model_router_reference PRIVATE model_router_cuda_probe)
  target_compile_definitions(model_router_reference PRIVATE MODEL_ROUTER_HAS_CUDA=1)
endif()

add_executable(router_coordinator router_coordinator_main.cpp)
target_link_libraries(router_coordinator
  PRIVATE model_router_reference $<BUILD_INTERFACE:model_router_warnings>)
if(MODEL_ROUTER_CUDA_FOUND)
  target_link_libraries(router_coordinator PRIVATE model_router_cuda_probe)
endif()

add_executable(backend_worker backend_worker_main.cpp)
target_link_libraries(backend_worker
  PRIVATE model_router_reference $<BUILD_INTERFACE:model_router_warnings>)
if(MODEL_ROUTER_CUDA_FOUND)
  target_link_libraries(backend_worker PRIVATE model_router_cuda_probe)
  target_compile_definitions(backend_worker PRIVATE MODEL_ROUTER_HAS_CUDA=1)
endif()

add_executable(reference_harness reference_harness_main.cpp)
target_link_libraries(reference_harness
  PRIVATE model_router_reference $<BUILD_INTERFACE:model_router_warnings>)
if(MODEL_ROUTER_CUDA_FOUND)
  target_link_libraries(reference_harness PRIVATE model_router_cuda_probe)
endif()

# The harness spawns the other two binaries itself.
set_target_properties(reference_harness PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
