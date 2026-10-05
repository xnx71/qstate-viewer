# Helpers shared by all native modules.
#
# A module lives in native/<name>/ and follows one layout:
#   include/qstate/<name>/*.h   public headers
#   src/*.cpp                   implementation
#   tests/*.cpp                 doctest test cases (no main; it is provided)
#   CMakeLists.txt              calls qstate_add_library() / qstate_add_tests()

function(qstate_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)
    endif()
endfunction()

# qstate_add_library(<name> [DEPS <public deps>...] [PRIVATE_DEPS <private deps>...])
# Creates the static library qstate_<name> (alias qstate::<name>) from src/*.cpp.
function(qstate_add_library name)
    cmake_parse_arguments(ARG "" "" "DEPS;PRIVATE_DEPS" ${ARGN})
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
    add_library(qstate_${name} STATIC ${_sources})
    add_library(qstate::${name} ALIAS qstate_${name})
    target_include_directories(qstate_${name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(qstate_${name} PUBLIC ${ARG_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
    qstate_set_warnings(qstate_${name})
endfunction()

# qstate_add_tests(<name> [DEPS <deps>...])
# Creates qstate_<name>_tests from tests/*.cpp, linked against qstate::<name> and doctest, registered with CTest.
function(qstate_add_tests name)
    if(NOT QSTATE_BUILD_TESTS)
        return()
    endif()
    cmake_parse_arguments(ARG "" "" "DEPS" ${ARGN})
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
    if(NOT _sources)
        return()
    endif()
    add_executable(qstate_${name}_tests ${_sources} "${PROJECT_SOURCE_DIR}/native/testing/doctest_main.cpp")
    target_include_directories(qstate_${name}_tests PRIVATE "${PROJECT_SOURCE_DIR}/native/testing")
    target_link_libraries(qstate_${name}_tests PRIVATE qstate::${name} doctest::doctest ${ARG_DEPS})
    qstate_set_warnings(qstate_${name}_tests)
    add_test(NAME ${name} COMMAND qstate_${name}_tests)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT
        "QSTATE_TEST_CORE_REPO=${QSTATE_TEST_CORE_REPO};QSTATE_TEST_STATE_DIR=${QSTATE_TEST_STATE_DIR};QSTATE_TEST_CORE_DIR_229=${QSTATE_TEST_CORE_DIR_229};QSTATE_SOURCE_DIR=${PROJECT_SOURCE_DIR}")
endfunction()
