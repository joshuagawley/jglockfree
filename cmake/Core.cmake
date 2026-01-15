option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(BUILD_BENCHMARKS "Build benchmarks" ON)
option(USE_SYSTEM_GOOGLETEST "Use system GoogleTest" OFF)
option(USE_SYSTEM_GOOGLEBENCHMARK "Use system GoogleBenchmark" OFF)

# Export compile commands used for custom targets
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set a default build type if none was specified
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'RelWithDebInfo' as none was specified.")
    set(CMAKE_BUILD_TYPE
            RelWithDebInfo
            CACHE STRING "Choose the type of build." FORCE)
    # Set the possible values of build type for cmake-gui, ccmake
    set_property(
            CACHE CMAKE_BUILD_TYPE
            PROPERTY STRINGS
            "Debug"
            "Release"
            "MinSizeRel"
            "RelWithDebInfo")
endif ()