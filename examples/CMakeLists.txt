# Model Router - runnable examples.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# SPDX-License-Identifier: Apache-2.0
#
# Every example is a small self-contained program that uses only the public API,
# prints deterministic text, and exits 0 on success.

set(MODEL_ROUTER_EXAMPLE_NAMES
  capability
  cost
  policy
  fallback
  recovery
  distributed)

foreach(example IN LISTS MODEL_ROUTER_EXAMPLE_NAMES)
  set(target model_router_example_${example})
  add_executable(${target} ${example}.cpp)
  target_link_libraries(${target} PRIVATE model_router $<BUILD_INTERFACE:model_router_warnings>)
  if(WIN32)
    target_link_libraries(${target} PRIVATE ws2_32)
  endif()
endforeach()
