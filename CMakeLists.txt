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
option(QSTATE_BUILD_UI "Build the web UI with pnpm as part of the native build" ON)

# Restrict the build to some native modules, e.g. -DQSTATE_MODULES="cpp;schema".
# Empty = every module that is present. Dependencies of a selected module must be selected too.
set(QSTATE_MODULES "" CACHE STRING "Semicolon separated list of native modules to build (empty = all)")

# Real data for integration tests (tests that need them are skipped when unset).
set(QSTATE_TEST_CORE_DIR "" CACHE PATH "Qubic core checkout used by integration tests")
set(QSTATE_TEST_STATE_DIR "" CACHE PATH "Directory with contractNNNN.EEE files used by integration tests")
set(QSTATE_TEST_CORE_DIR_229 "" CACHE PATH "Plain snapshot of the core sources matching the epoch-229 state files (service tests)")

if(QSTATE_BUILD_TESTS)
    enable_testing()
endif()

include(QstateModule)

add_subdirectory(native)
