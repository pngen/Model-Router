cmake_minimum_required(VERSION 3.20)

project(model_router
  VERSION 1.0.0
  DESCRIPTION "Policy-, capability-, cost-, availability-, latency-, locality-, compatibility-, health-, and authority-aware routing runtime across heterogeneous AI models and inference backends"
  HOMEPAGE_URL "https://github.com/pngen/Model-Router"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(MODEL_ROUTER_IS_TOP_LEVEL ON)
else()
  set(MODEL_ROUTER_IS_TOP_LEVEL OFF)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(MODEL_ROUTER_BUILD_TESTS "Build the Model Router test suite" ${MODEL_ROUTER_IS_TOP_LEVEL})
option(MODEL_ROUTER_BUILD_EXAMPLES "Build the Model Router examples" ${MODEL_ROUTER_IS_TOP_LEVEL})
option(MODEL_ROUTER_BUILD_TOOLS "Build the Model Router inspection tool" ${MODEL_ROUTER_IS_TOP_LEVEL})
option(MODEL_ROUTER_BUILD_BENCHMARKS "Build the Model Router benchmarks" ${MODEL_ROUTER_IS_TOP_LEVEL})
option(MODEL_ROUTER_BUILD_REFERENCE "Build the reference multiprocess architecture" ${MODEL_ROUTER_IS_TOP_LEVEL})
option(MODEL_ROUTER_BUILD_CUDA_PROOF "Build the optional real CUDA backend proof" ON)
option(MODEL_ROUTER_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(MODEL_ROUTER_ENABLE_SHARED "Build Model Router as a shared library" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MODEL_ROUTER_ENABLE_SHARED)
  set(MODEL_ROUTER_LIBRARY_TYPE SHARED)
else()
  set(MODEL_ROUTER_LIBRARY_TYPE STATIC)
endif()

# ---------------------------------------------------------------------------
# Warnings: strict, and never suppressed to fake cleanliness.
# ---------------------------------------------------------------------------
add_library(model_router_warnings INTERFACE)
# Options are restricted to C++ compiles: nvcc must not receive host-only flags.
if(MSVC)
  target_compile_options(model_router_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/permissive-;/utf-8;/EHsc;/Zc:__cplusplus;/Zc:preprocessor>)
else()
  target_compile_options(model_router_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror;-Wshadow;-Wconversion;-Wsign-conversion;-Wold-style-cast;-Wnon-virtual-dtor;-Woverloaded-virtual;-Wdouble-promotion>)
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
set(MODEL_ROUTER_CORE_SOURCES
  src/authority.cpp
  src/candidate.cpp
  src/capability.cpp
  src/catalog.cpp
  src/clock.cpp
  src/decision.cpp
  src/detail/crc32c.cpp
  src/detail/sha256.cpp
  src/eligibility.cpp
  src/evidence.cpp
  src/explanation.cpp
  src/ids.cpp
  src/limits.cpp
  src/outcome.cpp
  src/persistence.cpp
  src/policy.cpp
  src/ranking.cpp
  src/reference_backends.cpp
  src/request.cpp
  src/router.cpp
  src/router_dispatch.cpp
  src/router_inspect.cpp
  src/router_persist.cpp
  src/router_route.cpp
  src/router_snapshot.cpp
  src/distributed/protocol.cpp
  src/distributed/transport.cpp)

set(MODEL_ROUTER_PUBLIC_HEADERS
  include/model_router/authority.hpp
  include/model_router/candidate.hpp
  include/model_router/capability.hpp
  include/model_router/catalog.hpp
  include/model_router/clock.hpp
  include/model_router/decision.hpp
  include/model_router/evidence.hpp
  include/model_router/ids.hpp
  include/model_router/limits.hpp
  include/model_router/model_router.hpp
  include/model_router/outcome.hpp
  include/model_router/persistence.hpp
  include/model_router/policy.hpp
  include/model_router/providers.hpp
  include/model_router/reference_profiles.hpp
  include/model_router/request.hpp
  include/model_router/router.hpp
  include/model_router/version.hpp)

add_library(model_router ${MODEL_ROUTER_LIBRARY_TYPE} ${MODEL_ROUTER_CORE_SOURCES})
add_library(model_router::model_router ALIAS model_router)
target_include_directories(model_router
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(model_router PUBLIC cxx_std_20)
target_link_libraries(model_router PRIVATE $<BUILD_INTERFACE:model_router_warnings>)
set_target_properties(model_router PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME model_router
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

if(WIN32)
  target_compile_definitions(model_router PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(model_router PRIVATE ws2_32)
endif()

if(MODEL_ROUTER_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(model_router PUBLIC /fsanitize=address)
    target_link_options(model_router PUBLIC /INCREMENTAL:NO)
  else()
    target_compile_options(model_router PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(model_router PUBLIC -fsanitize=address)
  endif()
endif()

# ---------------------------------------------------------------------------
# Optional real CUDA backend proof (device probe only; never model inference)
# ---------------------------------------------------------------------------
set(MODEL_ROUTER_CUDA_FOUND OFF)
if(MODEL_ROUTER_BUILD_CUDA_PROOF)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    # enable_language(CUDA) caches the nvcc default architecture, so the native
    # default must be established before the language is enabled.
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
      set(CMAKE_CUDA_ARCHITECTURES native)
    endif()
    enable_language(CUDA)
    set(MODEL_ROUTER_CUDA_FOUND ON)
    message(STATUS "Model Router: CUDA backend proof enabled with ${CMAKE_CUDA_COMPILER}")
  else()
    message(STATUS "Model Router: CUDA not available; the optional CUDA backend proof is not built")
  endif()
endif()

if(MODEL_ROUTER_CUDA_FOUND)
  find_package(CUDAToolkit REQUIRED)
  add_library(model_router_cuda_probe STATIC
    cuda/cuda_reference_probe.cpp
    cuda/cuda_probe_kernel.cu)
  add_library(model_router::cuda_probe ALIAS model_router_cuda_probe)
  target_include_directories(model_router_cuda_probe
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(model_router_cuda_probe
    PUBLIC model_router CUDA::cudart
    PRIVATE $<BUILD_INTERFACE:model_router_warnings>)
  set_target_properties(model_router_cuda_probe PROPERTIES
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    CUDA_SEPARABLE_COMPILATION OFF)
  target_compile_definitions(model_router_cuda_probe PRIVATE MODEL_ROUTER_HAS_CUDA=1)
  set_source_files_properties(cuda/cuda_probe_kernel.cu PROPERTIES LANGUAGE CUDA)
  if(MSVC)
    target_compile_options(model_router_cuda_probe PRIVATE
      $<$<COMPILE_LANGUAGE:CUDA>:--Werror=all-warnings;-Xcompiler=/W4,-WX>)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests, examples, tools, benchmarks, reference architecture
# ---------------------------------------------------------------------------
if(MODEL_ROUTER_BUILD_REFERENCE)
  add_subdirectory(reference)
endif()

if(MODEL_ROUTER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MODEL_ROUTER_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(MODEL_ROUTER_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(MODEL_ROUTER_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install rules and package config
# ---------------------------------------------------------------------------
install(TARGETS model_router
  EXPORT model_routerTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(MODEL_ROUTER_CUDA_FOUND)
  install(TARGETS model_router_cuda_probe
    EXPORT model_routerTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Implementation detail headers are not part of the installed interface.
install(DIRECTORY include/model_router DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  PATTERN "detail" EXCLUDE)

install(EXPORT model_routerTargets
  FILE model_routerTargets.cmake
  NAMESPACE model_router::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/model_router)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/model_routerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/model_routerConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/model_router)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/model_routerConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/model_routerConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/model_routerConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/model_router)

install(FILES LICENSE README.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/model_router
  OPTIONAL)

message(STATUS "Model Router ${PROJECT_VERSION}: CUDA proof=${MODEL_ROUTER_CUDA_FOUND} ASan=${MODEL_ROUTER_ENABLE_ASAN}")
