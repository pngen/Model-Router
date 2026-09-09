# Model Router - test suite.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# SPDX-License-Identifier: Apache-2.0

# Test sources are globbed with CONFIGURE_DEPENDS so adding a test file needs no
# build-system edit. Tests are never installed, so a glob carries no packaging
# risk here.
file(GLOB MODEL_ROUTER_TEST_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/support/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

add_executable(model_router_tests EXCLUDE_FROM_ALL ${MODEL_ROUTER_TEST_SOURCES})
target_include_directories(model_router_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/support
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(model_router_tests PRIVATE model_router $<BUILD_INTERFACE:model_router_warnings>)
if(WIN32)
  target_link_libraries(model_router_tests PRIVATE ws2_32)
endif()
if(MODEL_ROUTER_CUDA_FOUND)
  target_link_libraries(model_router_tests PRIVATE model_router_cuda_probe)
  target_compile_definitions(model_router_tests PRIVATE MODEL_ROUTER_TEST_HAS_CUDA=1)
endif()

add_custom_target(model_router_check
  COMMAND model_router_tests
  DEPENDS model_router_tests
  USES_TERMINAL)

enable_testing()
add_test(NAME model_router_tests COMMAND model_router_tests)
