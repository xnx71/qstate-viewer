# Runs a command and checks its exit code and (optionally) a regex on stdout + stderr.
#   cmake -DCMD="prog;arg1;arg2" -DEXPECT_EXIT=1 [-DEXPECT_OUTPUT=regex] -P expect_exit.cmake
execute_process(COMMAND ${CMD} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "exit code ${rc}, expected ${EXPECT_EXIT}\n${out}\n${err}")
endif()
if(EXPECT_OUTPUT AND NOT "${out}${err}" MATCHES "${EXPECT_OUTPUT}")
    message(FATAL_ERROR "output does not match '${EXPECT_OUTPUT}'\n${out}\n${err}")
endif()
