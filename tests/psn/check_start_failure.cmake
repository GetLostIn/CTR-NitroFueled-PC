execute_process(COMMAND "${TEST_PROGRAM}" "${FIXTURE}" selected-missing
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
if(NOT result EQUAL 21 OR NOT error MATCHES "explicitly selected PSN backend is unavailable")
    message(FATAL_ERROR "Backend startup did not fail explicitly (exit ${result}): ${output}${error}")
endif()
