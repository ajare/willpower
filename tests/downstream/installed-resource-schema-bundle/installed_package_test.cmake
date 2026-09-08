foreach(_required BUILD_DIR SOURCE_BUNDLE INSTALL_PREFIX DOWNSTREAM_SOURCE DOWNSTREAM_BUILD
                  EXTERNAL_CONSUMER MANIFEST_DIR GENERATOR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${INSTALL_PREFIX}" "${DOWNSTREAM_BUILD}")
set(_install_command "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${INSTALL_PREFIX}"
    --component Willpower_Application)
if(DEFINED CONFIG AND NOT CONFIG STREQUAL "")
    list(APPEND _install_command --config "${CONFIG}")
endif()
execute_process(COMMAND ${_install_command} RESULT_VARIABLE _result
                OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Willpower installation failed (${_result})\n${_output}\n${_error}")
endif()

set(_configure_command "${CMAKE_COMMAND}" -S "${DOWNSTREAM_SOURCE}" -B "${DOWNSTREAM_BUILD}"
    -G "${GENERATOR}" "-DWILLPOWER_INSTALL_PREFIX=${INSTALL_PREFIX}")
if(DEFINED GENERATOR_PLATFORM AND NOT GENERATOR_PLATFORM STREQUAL "")
    list(APPEND _configure_command -A "${GENERATOR_PLATFORM}")
endif()
if(DEFINED GENERATOR_TOOLSET AND NOT GENERATOR_TOOLSET STREQUAL "")
    list(APPEND _configure_command -T "${GENERATOR_TOOLSET}")
endif()
execute_process(COMMAND ${_configure_command} RESULT_VARIABLE _result
                OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Installed-package downstream configure failed (${_result})\n${_output}\n${_error}")
endif()

set(_build_command "${CMAKE_COMMAND}" --build "${DOWNSTREAM_BUILD}" --target
    installed_application_resource_schemas)
if(DEFINED CONFIG AND NOT CONFIG STREQUAL "")
    list(APPEND _build_command --config "${CONFIG}")
endif()
execute_process(COMMAND ${_build_command} RESULT_VARIABLE _result
                OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Installed-package downstream build failed (${_result})\n${_output}\n${_error}")
endif()

set(_installed_bundle "${DOWNSTREAM_BUILD}/resource-schema-bundle")
file(GLOB_RECURSE _source_documents RELATIVE "${SOURCE_BUNDLE}" "${SOURCE_BUNDLE}/schemas/*.json")
foreach(_relative catalog.json ${_source_documents})
    if(NOT EXISTS "${_installed_bundle}/${_relative}")
        message(FATAL_ERROR "Installed-package composition omitted ${_relative}")
    endif()
    file(SHA256 "${SOURCE_BUNDLE}/${_relative}" _source_hash)
    file(SHA256 "${_installed_bundle}/${_relative}" _installed_hash)
    if(NOT _source_hash STREQUAL _installed_hash)
        message(FATAL_ERROR "Installed and source-tree composition differ at ${_relative}")
    endif()
endforeach()

foreach(_case valid-mixed invalid-default invalid-specialized)
    execute_process(
        COMMAND "${EXTERNAL_CONSUMER}" "${_installed_bundle}" "${MANIFEST_DIR}/${_case}.yaml"
        RESULT_VARIABLE _validation_result)
    if(_case STREQUAL "valid-mixed")
        set(_expected 0)
    else()
        set(_expected 1)
    endif()
    if(NOT _validation_result EQUAL _expected)
        message(FATAL_ERROR
            "External validation through the installed package returned ${_validation_result} for ${_case}")
    endif()
endforeach()
