foreach(_required EXTERNAL_CONSUMER RUNTIME_CONSUMER BUNDLE_DIR MANIFEST_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

foreach(_case valid-mixed invalid-default invalid-specialized)
    execute_process(
        COMMAND "${EXTERNAL_CONSUMER}" "${BUNDLE_DIR}" "${MANIFEST_DIR}/${_case}.yaml"
        RESULT_VARIABLE _external
        OUTPUT_VARIABLE _external_output
        ERROR_VARIABLE _external_error)
    execute_process(
        COMMAND "${RUNTIME_CONSUMER}" "${BUNDLE_DIR}" --validate-manifest
                "${MANIFEST_DIR}/${_case}.yaml"
        RESULT_VARIABLE _runtime
        OUTPUT_VARIABLE _runtime_output
        ERROR_VARIABLE _runtime_error)
    if(_case STREQUAL "valid-mixed")
        set(_expected 0)
    else()
        set(_expected 1)
    endif()
    if(NOT _external EQUAL _runtime OR NOT _external EQUAL _expected)
        message(FATAL_ERROR
            "Schema acceptance differed for ${_case} (external=${_external}, runtime=${_runtime}, expected=${_expected})\n"
            "external stdout: ${_external_output}\nexternal stderr: ${_external_error}\n"
            "runtime stdout: ${_runtime_output}\nruntime stderr: ${_runtime_error}")
    endif()
endforeach()
