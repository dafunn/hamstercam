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
    src/capture_state_machine.cpp)
add_library(hamstercam::core ALIAS hamstercam_core)

target_include_directories(hamstercam_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(hamstercam_core PUBLIC hamstercam_flags)

find_package(Threads REQUIRED)
target_link_libraries(hamstercam_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(HAMSTERCAM_BUILD_TESTS)
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

    add_executable(hamstercam_tests
        tests/backoff_test.cpp
        tests/histogram_test.cpp
        tests/capture_state_machine_test.cpp)

    target_include_directories(hamstercam_tests PRIVATE tests/support)
    target_link_libraries(hamstercam_tests PRIVATE hamstercam_core GTest::gtest_main)
    gtest_discover_tests(hamstercam_tests)
endif()
