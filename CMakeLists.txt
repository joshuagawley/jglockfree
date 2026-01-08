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
    find_package(GTest REQUIRED)
    include(GoogleTest)

    enable_testing()

    add_subdirectory(tests)
endif ()

if (RUN_BENCHMARKS)
    find_package(benchmark REQUIRED)
    add_subdirectory(benchmarks)
endif ()