# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.23)
project(jglockfree LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(Core)
include(ProjectSettings)

add_library(project_settings INTERFACE)
set_project_settings(project_settings WARNING_AS_ERRORS)

add_subdirectory(jglockfree)

include(CTest)
if (BUILD_TESTING)
    if (USE_SYSTEM_GOOGLETEST)
        find_package(GTest REQUIRED)
    else ()
        set(INSTALL_GTEST OFF CACHE BOOL "Disable GoogleTest installation")
        add_subdirectory(third_party/googletest)
    endif ()
    include(GoogleTest)

    enable_testing()

    add_subdirectory(tests)
endif ()

if (BUILD_BENCHMARKS)
    if (USE_SYSTEM_GOOGLEBENCHMARK)
        find_package(benchmark REQUIRED)
    else ()
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Disable benchmark testing")
        add_subdirectory(third_party/benchmark)
    endif ()
    add_subdirectory(benchmarks)
endif ()