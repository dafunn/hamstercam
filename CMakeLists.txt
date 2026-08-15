cmake_minimum_required(VERSION 3.20)

project(hamstercam
    VERSION 0.1.0
    DESCRIPTION "hamster camera"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(HAMSTERCAM_BUILD_TESTS "Build the test suite" ON)
option(HAMSTERCAM_BUILD_INTEGRATION_TESTS "Build the hardware integration suite (needs a real V4L2 device; never run in CI)" OFF)
option(HAMSTERCAM_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(HAMSTERCAM_WERROR "Treat warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Warning and sanitizer policy, carried by an interface target so every
# target opts in the same way.
# ---------------------------------------------------------------------------
add_library(hamstercam_flags INTERFACE)

target_compile_options(hamstercam_flags INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wcast-qual
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wdouble-promotion)

if(HAMSTERCAM_WERROR)
    target_compile_options(hamstercam_flags INTERFACE -Werror)
endif()

if(HAMSTERCAM_SANITIZE)
    target_compile_options(hamstercam_flags INTERFACE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    target_link_options(hamstercam_flags INTERFACE -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Core library
#
# Everything here is portable, hardware-free logic: the state machine,
# backoff, stall detection, and metric accounting. It is exercised entirely
# through ICaptureDevice, so it builds and tests identically with or without
# a real V4L2 backend.
# ---------------------------------------------------------------------------
add_library(hamstercam_core STATIC
    src/capture_device.cpp
    src/capture_state_machine.cpp
    src/metrics.cpp
    src/json_logger.cpp
    src/http_server.cpp
    src/process_info.cpp)
add_library(hamstercam::core ALIAS hamstercam_core)

target_include_directories(hamstercam_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(hamstercam_core PUBLIC hamstercam_flags)

find_package(Threads REQUIRED)
target_link_libraries(hamstercam_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Build info (version + git SHA), baked into hamstercam_build_info.
# ---------------------------------------------------------------------------
find_package(Git QUIET)
set(HAMSTERCAM_GIT_SHA "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE HAMSTERCAM_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE HAMSTERCAM_GIT_RESULT)
    if(NOT HAMSTERCAM_GIT_RESULT EQUAL 0 OR NOT HAMSTERCAM_GIT_SHA)
        set(HAMSTERCAM_GIT_SHA "unknown")
    endif()
endif()
configure_file(cmake/version.hpp.in ${CMAKE_BINARY_DIR}/generated/hamstercam/version.hpp @ONLY)

# ---------------------------------------------------------------------------
# hamstercamd
#
# The only target that touches real V4L2. Everything it depends on is
# already exercised hardware-free through hamstercam_core.
# ---------------------------------------------------------------------------
add_executable(hamstercamd
    src/main.cpp
    src/v4l2_capture_device.cpp)

target_include_directories(hamstercamd PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(hamstercamd PRIVATE hamstercam_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(HAMSTERCAM_BUILD_TESTS OR HAMSTERCAM_BUILD_INTEGRATION_TESTS)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    include(GoogleTest)
endif()

if(HAMSTERCAM_BUILD_TESTS)
    add_executable(hamstercam_tests
        tests/backoff_test.cpp
        tests/histogram_test.cpp
        tests/capture_state_machine_test.cpp
        tests/metrics_test.cpp
        tests/json_logger_test.cpp)

    target_include_directories(hamstercam_tests PRIVATE tests/support)
    target_link_libraries(hamstercam_tests PRIVATE hamstercam_core GTest::gtest_main)
    gtest_discover_tests(hamstercam_tests)
endif()

# Needs a real V4L2 device; never registered with ctest, so it cannot run in
# CI and cannot block a CI pass. Build and run it by hand:
#   cmake -S . -B build-integration -DHAMSTERCAM_BUILD_INTEGRATION_TESTS=ON
#   HAMSTERCAM_TEST_DEVICE=/dev/videoN ./build-integration/hamstercam_integration_tests
if(HAMSTERCAM_BUILD_INTEGRATION_TESTS)
    add_executable(hamstercam_integration_tests
        src/v4l2_capture_device.cpp
        tests/integration/v4l2_capture_device_integration_test.cpp)

    target_link_libraries(hamstercam_integration_tests PRIVATE hamstercam_core GTest::gtest_main)
endif()
