# Model Router - routing throughput benchmark suite.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# SPDX-License-Identifier: Apache-2.0
#
# The benchmark is deliberately NOT registered with CTest. A full run measures
# the 1000 models / 10000 backends catalog scale and 100000 completed route
# decisions, which is far longer than a normal unit test may take. It is a
# normal executable target with a convenience custom target instead:
#
#   cmake --build <build-dir> --target model_router_bench
#
# Exit status: 0 = every guard passed, 1 = a benchmark guard failed,
# 2 = bad command line, 3 = the measurements completed but the core library
# violated a contract this benchmark measures (reported in the output).

set(MODEL_ROUTER_BENCHMARK_SOURCES
  benchmark_main.cpp)

add_executable(model_router_benchmarks ${MODEL_ROUTER_BENCHMARK_SOURCES})
target_link_libraries(model_router_benchmarks
  PRIVATE model_router $<BUILD_INTERFACE:model_router_warnings>)
if(WIN32)
  target_link_libraries(model_router_benchmarks PRIVATE ws2_32)
endif()

add_custom_target(model_router_bench
  COMMAND model_router_benchmarks
  DEPENDS model_router_benchmarks
  USES_TERMINAL)
