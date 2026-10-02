# qstate-cli: command line front end of the service (verify / schema / dump / table / search / digest / serve).
# All logic lives in qstate::service; the commands only call RPC methods (or core_loader) and print.
qstate_add_library(cli DEPS qstate::service qstate::httpd qstate::rpc nlohmann_json::nlohmann_json Threads::Threads)

add_executable(qstate-cli app/main.cpp)
target_link_libraries(qstate-cli PRIVATE qstate::cli Threads::Threads)
qstate_set_warnings(qstate-cli)
install(TARGETS qstate-cli RUNTIME DESTINATION bin)

qstate_add_tests(cli)

if(QSTATE_BUILD_TESTS)
    add_test(NAME cli_help COMMAND qstate-cli --help)
    add_test(NAME cli_bad_command
        COMMAND ${CMAKE_COMMAND} "-DCMD=$<TARGET_FILE:qstate-cli>;frobnicate" -DEXPECT_EXIT=2 "-DEXPECT_OUTPUT=unknown command"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_exit.cmake)
    # Smoke tests on the real data (epoch-229 files against the matching core version).
    if(QSTATE_TEST_CORE_DIR AND QSTATE_TEST_STATE_DIR)
        add_test(NAME cli_verify_real
            COMMAND qstate-cli verify --core ${QSTATE_TEST_CORE_DIR} --ref auto --state ${QSTATE_TEST_STATE_DIR})
        set_tests_properties(cli_verify_real PROPERTIES PASS_REGULAR_EXPRESSION "29 ok, 0 not ok")
        # The HEAD working tree has a different NOST layout: verify must exit with 1 and say why.
        add_test(NAME cli_verify_real_worktree_mismatch
            COMMAND ${CMAKE_COMMAND}
                "-DCMD=$<TARGET_FILE:qstate-cli>;verify;--core;${QSTATE_TEST_CORE_DIR};--state;${QSTATE_TEST_STATE_DIR}"
                -DEXPECT_EXIT=1 "-DEXPECT_OUTPUT=size-mismatch"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_exit.cmake)
        add_test(NAME cli_schema_real
            COMMAND qstate-cli schema --core ${QSTATE_TEST_CORE_DIR} --contract 1)
        set_tests_properties(cli_schema_real PROPERTIES PASS_REGULAR_EXPRESSION "QX::StateData")
        add_test(NAME cli_dump_real
            COMMAND qstate-cli dump --core ${QSTATE_TEST_CORE_DIR} --ref auto --state ${QSTATE_TEST_STATE_DIR} --contract 3)
        set_tests_properties(cli_dump_real PROPERTIES PASS_REGULAR_EXPRESSION "contract 3")
    endif()
endif()
