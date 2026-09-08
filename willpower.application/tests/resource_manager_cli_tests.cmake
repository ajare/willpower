if(NOT DEFINED TOOL OR NOT DEFINED VALID_MANIFEST OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "TOOL, VALID_MANIFEST, and WORK_DIR are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/base")
get_filename_component(VALID_BASE "${VALID_MANIFEST}" DIRECTORY)

function(run_tool expected_result stdout_var stderr_var)
  execute_process(
    COMMAND "${TOOL}" ${ARGN}
    RESULT_VARIABLE actual_result
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr)
  if(NOT actual_result EQUAL expected_result)
    message(FATAL_ERROR
      "resource-manager returned ${actual_result}, expected ${expected_result}\n"
      "stdout: ${actual_stdout}\nstderr: ${actual_stderr}")
  endif()
  set(${stdout_var} "${actual_stdout}" PARENT_SCOPE)
  set(${stderr_var} "${actual_stderr}" PARENT_SCOPE)
endfunction()

run_tool(0 valid_stdout valid_stderr
  --validate "${VALID_MANIFEST}" --base-directory "${VALID_BASE}")
if(NOT valid_stdout MATCHES "Valid Resource Manifest")
  message(FATAL_ERROR "Valid manifest success was not reported: ${valid_stdout}")
endif()

file(WRITE "${WORK_DIR}/malformed.YAML" "Resources:\n  Resource: [\n")
run_tool(4 malformed_stdout malformed_stderr
  --validate "${WORK_DIR}/malformed.YAML" --base-directory "${WORK_DIR}/base")
if(NOT malformed_stderr MATCHES "YAML syntax")
  message(FATAL_ERROR "Malformed YAML diagnostic was not distinct: ${malformed_stderr}")
endif()

file(WRITE "${WORK_DIR}/structural.yml" "Resources:\n  unexpected: true\n")
run_tool(4 structural_stdout structural_stderr
  --validate "${WORK_DIR}/structural.yml" --base-directory "${WORK_DIR}/base")
if(NOT structural_stderr MATCHES "structural validation" OR
   NOT structural_stderr MATCHES "/Resources")
  message(FATAL_ERROR "Structural diagnostic was incomplete: ${structural_stderr}")
endif()

set(deep_yaml "Resources:\n")
set(deep_indent "  ")
foreach(index RANGE 1 129)
  string(APPEND deep_yaml "${deep_indent}level${index}:\n")
  string(APPEND deep_indent "  ")
endforeach()
string(APPEND deep_yaml "${deep_indent}value\n")
file(WRITE "${WORK_DIR}/deep.yaml" "${deep_yaml}")
run_tool(4 deep_stdout deep_stderr
  --validate "${WORK_DIR}/deep.yaml" --base-directory "${WORK_DIR}/base")
if(NOT deep_stderr MATCHES "defensive limit")
  message(FATAL_ERROR "Nesting limit diagnostic was not reported: ${deep_stderr}")
endif()

set(canonical_one "${WORK_DIR}/canonical-one.yaml")
set(canonical_two "${WORK_DIR}/canonical-two.yaml")
run_tool(0 canonical_stdout canonical_stderr
  --validate "${VALID_MANIFEST}" --base-directory "${VALID_BASE}"
  --canonical-output "${canonical_one}")
run_tool(0 repeat_stdout repeat_stderr
  --validate "${canonical_one}" --base-directory "${VALID_BASE}"
  --canonical-output "${canonical_two}")
file(READ "${canonical_one}" canonical_one_bytes)
file(READ "${canonical_two}" canonical_two_bytes)
if(NOT canonical_one_bytes STREQUAL canonical_two_bytes)
  message(FATAL_ERROR "Canonical output changed after executable round-trip")
endif()
string(FIND "${canonical_one_bytes}" "\r" carriage_return)
if(NOT carriage_return EQUAL -1)
  message(FATAL_ERROR "Canonical output contains CR line endings")
endif()
if(NOT canonical_one_bytes MATCHES "\n$")
  message(FATAL_ERROR "Canonical output has no final newline")
endif()

run_tool(6 base_stdout base_stderr
  --validate "${VALID_MANIFEST}" --base-directory "${WORK_DIR}/missing-base")
if(NOT base_stderr MATCHES "base directory")
  message(FATAL_ERROR "Missing base-directory diagnostic was not reported: ${base_stderr}")
endif()

file(WRITE "${WORK_DIR}/semantic.yaml"
  "Resources:\n  Resource:\n    type: TextFile\n    name: Missing\n    location: missing.txt\n")
run_tool(5 semantic_stdout semantic_stderr
  --validate "${WORK_DIR}/semantic.yaml" --base-directory "${WORK_DIR}/base" --semantic)
if(NOT semantic_stderr MATCHES "semantic validation" OR
   NOT semantic_stderr MATCHES "does not exist")
  message(FATAL_ERROR "Semantic validation status was not reported: ${semantic_stderr}")
endif()

file(WRITE "${WORK_DIR}/containment.yaml"
  "Resources:\n  Resource:\n    type: TextFile\n    name: Escape\n    location: ../outside.txt\n")
run_tool(6 containment_stdout containment_stderr
  --validate "${WORK_DIR}/containment.yaml" --base-directory "${WORK_DIR}/base" --semantic)
if(NOT containment_stderr MATCHES "outside the canonical base")
  message(FATAL_ERROR "Path-containment status was not reported: ${containment_stderr}")
endif()
