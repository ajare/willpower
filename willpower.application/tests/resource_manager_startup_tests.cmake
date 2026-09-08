if(NOT DEFINED TOOL OR NOT DEFINED INI OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "TOOL, INI, and WORK_DIR are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/preferences")
file(WRITE "${WORK_DIR}/preferences/resource-manager.log" "previous log\n")

function(run_startup expected_result preferences ini stdout_var stderr_var)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
      "WILLPOWER_RESOURCE_MANAGER_PREFERENCE_DIR=${preferences}"
      "${TOOL}" --startup-check --ini "${ini}"
    RESULT_VARIABLE actual_result
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr)
  if(NOT actual_result EQUAL expected_result)
    message(FATAL_ERROR
      "resource-manager startup returned ${actual_result}, expected ${expected_result}\n"
      "stdout: ${actual_stdout}\nstderr: ${actual_stderr}")
  endif()
  set(${stdout_var} "${actual_stdout}" PARENT_SCOPE)
  set(${stderr_var} "${actual_stderr}" PARENT_SCOPE)
endfunction()

run_startup(0 "${WORK_DIR}/preferences" "${INI}" valid_stdout valid_stderr)
if(NOT valid_stdout MATCHES "startup check passed")
  message(FATAL_ERROR "Successful startup check was not reported: ${valid_stdout}")
endif()
execute_process(
  COMMAND "${TOOL}" --verify-schemas --ini "${INI}"
  RESULT_VARIABLE verify_result
  OUTPUT_VARIABLE verify_stdout
  ERROR_VARIABLE verify_stderr)
if(NOT verify_result EQUAL 0 OR NOT verify_stdout MATCHES "Verified Resource Schema")
  message(FATAL_ERROR
    "Headless schema verification failed (${verify_result})\n"
    "stdout: ${verify_stdout}\nstderr: ${verify_stderr}")
endif()
if(NOT EXISTS "${WORK_DIR}/preferences/resource-manager.log" OR
   NOT EXISTS "${WORK_DIR}/preferences/resource-manager.log.1")
  message(FATAL_ERROR "Preference-directory log was not created and rotated")
endif()
file(READ "${WORK_DIR}/preferences/resource-manager.log.1" old_log)
if(NOT old_log STREQUAL "previous log\n")
  message(FATAL_ERROR "Rotated log did not preserve the previous log")
endif()

run_startup(3 "${WORK_DIR}/preferences" "${WORK_DIR}/missing.ini"
  missing_stdout missing_stderr)
if(NOT missing_stderr MATCHES "configuration failure")
  message(FATAL_ERROR "Missing deployment INI did not report configuration failure: ${missing_stderr}")
endif()

file(WRITE "${WORK_DIR}/invalid.ini"
  "[ResourceManifestEditor]\nformatVersion=2\n")
run_startup(3 "${WORK_DIR}/preferences" "${WORK_DIR}/invalid.ini"
  invalid_stdout invalid_stderr)
if(NOT invalid_stderr MATCHES "formatVersion")
  message(FATAL_ERROR "Invalid deployment INI diagnostic was incomplete: ${invalid_stderr}")
endif()
execute_process(
  COMMAND "${TOOL}" --verify-schemas --ini "${WORK_DIR}/invalid.ini"
  RESULT_VARIABLE invalid_verify_result
  ERROR_VARIABLE invalid_verify_stderr)
if(NOT invalid_verify_result EQUAL 3 OR
   NOT invalid_verify_stderr MATCHES "schema configuration failure")
  message(FATAL_ERROR
    "Malformed schema configuration did not fail headlessly with status 3: "
    "${invalid_verify_result}; ${invalid_verify_stderr}")
endif()

file(WRITE "${WORK_DIR}/not-a-directory" "blocking preference path")
run_startup(7 "${WORK_DIR}/not-a-directory" "${INI}"
  logging_stdout logging_stderr)
if(NOT logging_stderr MATCHES "logging failure")
  message(FATAL_ERROR "Logging setup failure was not reported: ${logging_stderr}")
endif()
