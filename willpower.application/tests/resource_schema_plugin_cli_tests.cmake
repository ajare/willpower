foreach(_required TOOL SAMPLE_PLUGIN INCOMPATIBLE_PLUGIN WORK_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

execute_process(
    COMMAND "${TOOL}" list --plugin "${SAMPLE_PLUGIN}"
    RESULT_VARIABLE _list_result
    OUTPUT_VARIABLE _list_output
    ERROR_VARIABLE _list_error)
if(NOT _list_result EQUAL 0)
    message(FATAL_ERROR "Plugin list failed (${_list_result}): ${_list_error}")
endif()
if(NOT _list_output MATCHES "PluginWidget" OR NOT _list_output MATCHES "GlowFactory")
    message(FATAL_ERROR "Plugin schemas were absent from CLI list output: ${_list_output}")
endif()

execute_process(
    COMMAND "${TOOL}" list --plugin "${INCOMPATIBLE_PLUGIN}"
    RESULT_VARIABLE _incompatible_result
    OUTPUT_VARIABLE _incompatible_output
    ERROR_VARIABLE _incompatible_error)
if(NOT _incompatible_result EQUAL 3)
    message(FATAL_ERROR
        "Incompatible plugin returned ${_incompatible_result}, not version error 3: ${_incompatible_error}")
endif()
string(JSON _incompatible_path GET "${_incompatible_error}" plugin)
string(JSON _incompatible_code GET "${_incompatible_error}" code)
if(NOT _incompatible_code EQUAL 3 OR NOT _incompatible_path STREQUAL "${INCOMPATIBLE_PLUGIN}")
    message(FATAL_ERROR "Incompatible plugin diagnostic is not machine readable: ${_incompatible_error}")
endif()

execute_process(
    COMMAND "${TOOL}" export --plugin "${SAMPLE_PLUGIN}"
            --output "${WORK_DIR}/exported"
    RESULT_VARIABLE _export_result
    OUTPUT_VARIABLE _export_output
    ERROR_VARIABLE _export_error)
if(NOT _export_result EQUAL 0)
    message(FATAL_ERROR "Plugin export failed (${_export_result}): ${_export_error}")
endif()
file(READ "${WORK_DIR}/exported/catalog.json" _catalog)
file(READ "${WORK_DIR}/exported/resource-manifest.schema.json" _root)
if(NOT _catalog MATCHES "PluginWidget" OR NOT _catalog MATCHES "GlowFactory" OR
   NOT _root MATCHES "PluginWidget" OR NOT _root MATCHES "GlowFactory")
    message(FATAL_ERROR "Plugin export did not compose custom schemas with the built-in catalog")
endif()
