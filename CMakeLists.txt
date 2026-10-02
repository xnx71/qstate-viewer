cmake_minimum_required(VERSION 3.24)
project(qstate-viewer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(QSTATE_BUILD_TESTS "Build the native unit tests" ON)
option(QSTATE_BUILD_GUI "Build the desktop app (needs the system webview development files)" ON)

# Real data for integration tests (tests that need them are skipped when unset).
set(QSTATE_TEST_CORE_REPO "" CACHE PATH "A git clone of the Qubic core with its tags (used as a local repoUrl, no network needed)")
set(QSTATE_TEST_STATE_DIR "" CACHE PATH "Directory with the epoch-229 contractNNNN.229 files")
set(QSTATE_TEST_CORE_DIR_229 "" CACHE PATH "Plain snapshot of the core sources of v1.303.2 (the version of the epoch-229 files)")

if(QSTATE_BUILD_TESTS)
    enable_testing()
endif()

include(QstateModule)

add_subdirectory(native)
