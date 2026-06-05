##
## CMakeLists.txt
## norxs Deterministic TSN Zonal Backbone
## AUTOSAR C++14 / ISO 26262 ASIL-D build configuration
##
## Targets:
##   norxs-tsn-backbone         : Static library (cross-compile for aarch64-imx8x)
##   norxs-tsn-tests            : GoogleTest host binary (x86_64)
##
## Usage:
##   # Cross-compile for NXP i.MX8X
##   cmake -B build-imx8x -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-aarch64-imx8x.cmake
##   cmake --build build-imx8x
##
##   # Host unit tests
##   cmake -B build-host -DNORXS_BUILD_TESTS=ON
##   cmake --build build-host
##   ctest --test-dir build-host -V
##

cmake_minimum_required(VERSION 3.21)

project(norxs-tsn-zonal-backbone
        VERSION      1.0.0
        DESCRIPTION  "Deterministic TSN Zonal Backbone — norxs Technology LLC"
        LANGUAGES    CXX
)

# ─────────────────────────────────────────────────────────────────────────────
# Options
# ─────────────────────────────────────────────────────────────────────────────

option(NORXS_BUILD_TESTS      "Build GoogleTest unit test suite"     OFF)
option(NORXS_ENABLE_COVERAGE  "Enable gcov/lcov coverage (host only)" OFF)
option(NORXS_STRICT_WARNINGS  "Treat all warnings as errors"          ON)

# ─────────────────────────────────────────────────────────────────────────────
# C++14 Standard  (AUTOSAR C++14 — no later extensions)
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD          14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)  # -std=c++14, not -std=gnu++14

# ─────────────────────────────────────────────────────────────────────────────
# Compiler flags
# ─────────────────────────────────────────────────────────────────────────────

set(NORXS_COMPILE_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wundef
    -Wno-unused-parameter          # Permitted: pure virtual override stubs
    -fno-exceptions                # AUTOSAR M15-3-1: no exceptions
    -fno-rtti                      # AUTOSAR M18-1-1: no RTTI
    -fstack-usage                  # Stack analysis (ISO 26262 Part 6 §9.4.3)
    -ffunction-sections            # Enable dead-code elimination
    -fdata-sections
)

if(NORXS_STRICT_WARNINGS)
    list(APPEND NORXS_COMPILE_FLAGS -Werror)
endif()

if(NORXS_ENABLE_COVERAGE)
    list(APPEND NORXS_COMPILE_FLAGS --coverage -O0 -g)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Library target: norxs-tsn-backbone
# ─────────────────────────────────────────────────────────────────────────────

set(NORXS_TSN_SOURCES
    src/GateControlManager.cpp
    src/PtpClockManager.cpp
    src/FrerManager.cpp
    src/TsnOrchestrator.cpp
)

add_library(norxs-tsn-backbone STATIC ${NORXS_TSN_SOURCES})

target_include_directories(norxs-tsn-backbone
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(norxs-tsn-backbone PRIVATE ${NORXS_COMPILE_FLAGS})

target_compile_definitions(norxs-tsn-backbone
    PRIVATE
        NORXS_TSN_VERSION_MAJOR=1
        NORXS_TSN_VERSION_MINOR=0
        NORXS_TSN_VERSION_PATCH=0
)

# ─────────────────────────────────────────────────────────────────────────────
# Unit Tests
# ─────────────────────────────────────────────────────────────────────────────

if(NORXS_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)

    add_executable(norxs-tsn-tests
        tests/test_GateControlManager.cpp
        tests/test_PtpClockManager.cpp
        tests/test_FrerManager.cpp
    )

    target_link_libraries(norxs-tsn-tests
        PRIVATE
            norxs-tsn-backbone
            GTest::gtest_main
    )

    target_compile_options(norxs-tsn-tests PRIVATE
        -Wall -Wextra -fno-exceptions -fno-rtti
    )

    if(NORXS_ENABLE_COVERAGE)
        target_compile_options(norxs-tsn-tests PRIVATE --coverage -O0 -g)
        target_link_options(norxs-tsn-tests    PRIVATE --coverage)
    endif()

    include(GoogleTest)
    gtest_discover_tests(norxs-tsn-tests
        TEST_SUFFIX     "_norxs"
        DISCOVERY_TIMEOUT 30
    )
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Stack usage report removed — CI job "Stack Usage Analysis" handles this.
# The previous add_custom_command used find -exec cat {} \; which breaks
# under the Ninja generator (the semicolon escape is lost in build.ninja).

# ─────────────────────────────────────────────────────────────────────────────
# Install rules
# ─────────────────────────────────────────────────────────────────────────────

install(TARGETS norxs-tsn-backbone
        EXPORT  norxsTsnTargets
        ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
        DESTINATION include
)

install(EXPORT norxsTsnTargets
        FILE        norxsTsnTargets.cmake
        NAMESPACE   norxs::
        DESTINATION lib/cmake/norxs-tsn
)
