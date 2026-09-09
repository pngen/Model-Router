# Model Router - command line inspection tool.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# SPDX-License-Identifier: Apache-2.0
#
# The tool is a read-only inspector: it never prompts and never waits.

add_executable(model_router_inspect model_router_inspect.cpp)
target_link_libraries(model_router_inspect
  PRIVATE model_router $<BUILD_INTERFACE:model_router_warnings>)
if(WIN32)
  target_link_libraries(model_router_inspect PRIVATE ws2_32)
endif()
