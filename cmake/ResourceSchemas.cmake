include_guard(GLOBAL)

# Declares an application Resource Type schema for the next call to
# willpower_compose_resource_schema_bundle(). A schema without FACTORY_TYPE is
# the default Resource declaration schema. A schema with FACTORY_TYPE validates
# the matching specialized Definition object.
function(willpower_declare_resource_schema)
    cmake_parse_arguments(PARSE_ARGV 0 schema "" "RESOURCE_TYPE;FACTORY_TYPE;SCHEMA_ID;SOURCE_FILE" "")
    if(schema_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "willpower_declare_resource_schema received unknown arguments: ${schema_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_required RESOURCE_TYPE SCHEMA_ID SOURCE_FILE)
        if(NOT DEFINED schema_${_required} OR schema_${_required} STREQUAL "")
            message(FATAL_ERROR "willpower_declare_resource_schema requires ${_required}")
        endif()
    endforeach()
    if(NOT schema_RESOURCE_TYPE MATCHES "^[A-Za-z_][A-Za-z0-9_.:-]*$")
        message(FATAL_ERROR "Invalid Resource Type '${schema_RESOURCE_TYPE}'")
    endif()
    if(DEFINED schema_FACTORY_TYPE AND
       NOT schema_FACTORY_TYPE MATCHES "^[A-Za-z_][A-Za-z0-9_.:-]*$")
        message(FATAL_ERROR "Invalid factory type '${schema_FACTORY_TYPE}'")
    endif()
    if(NOT schema_SCHEMA_ID MATCHES "^[A-Za-z][A-Za-z0-9+.-]*:")
        message(FATAL_ERROR "Schema ID '${schema_SCHEMA_ID}' must be an absolute URI")
    endif()
    foreach(_value "${schema_RESOURCE_TYPE}" "${schema_FACTORY_TYPE}" "${schema_SCHEMA_ID}" "${schema_SOURCE_FILE}")
        if(_value MATCHES "[|;\r\n]")
            message(FATAL_ERROR "Resource schema metadata cannot contain '|', ';', or a newline")
        endif()
    endforeach()

    get_filename_component(_source "${schema_SOURCE_FILE}" ABSOLUTE
                           BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${_source}" OR IS_DIRECTORY "${_source}")
        message(FATAL_ERROR "Missing or non-regular Resource schema: ${_source}")
    endif()
    file(READ "${_source}" _schema_json)
    string(JSON _schema_type ERROR_VARIABLE _type_error TYPE "${_schema_json}")
    if(_type_error OR NOT _schema_type STREQUAL "OBJECT")
        message(FATAL_ERROR "Resource schema '${_source}' must be a JSON object")
    endif()
    string(JSON _actual_id ERROR_VARIABLE _id_error GET "${_schema_json}" "$id")
    if(_id_error OR NOT _actual_id STREQUAL schema_SCHEMA_ID)
        message(FATAL_ERROR
            "Schema ID mismatch for ${_source}: expected '${schema_SCHEMA_ID}', found '${_actual_id}'")
    endif()
    string(JSON _draft ERROR_VARIABLE _draft_error GET "${_schema_json}" "$schema")
    if(_draft_error OR NOT _draft STREQUAL "http://json-schema.org/draft-07/schema#")
        message(FATAL_ERROR
            "Resource schema '${_source}' must declare JSON Schema Draft 7 in $schema")
    endif()

    get_property(_keys GLOBAL PROPERTY WILLPOWER_DECLARED_RESOURCE_SCHEMA_KEYS)
    set(_key "${schema_RESOURCE_TYPE}|${schema_FACTORY_TYPE}")
    if(_key IN_LIST _keys)
        if(schema_FACTORY_TYPE STREQUAL "")
            set(_factory_label "<default>")
        else()
            set(_factory_label "${schema_FACTORY_TYPE}")
        endif()
        message(FATAL_ERROR
            "Duplicate Resource schema key ('${schema_RESOURCE_TYPE}', '${_factory_label}')")
    endif()

    file(SHA256 "${_source}" _hash)
    get_property(_ids GLOBAL PROPERTY WILLPOWER_DECLARED_RESOURCE_SCHEMA_IDS)
    get_property(_hashes GLOBAL PROPERTY WILLPOWER_DECLARED_RESOURCE_SCHEMA_HASHES)
    list(FIND _ids "${schema_SCHEMA_ID}" _id_index)
    if(NOT _id_index EQUAL -1)
        list(GET _hashes ${_id_index} _existing_hash)
        if(NOT _existing_hash STREQUAL _hash)
            message(FATAL_ERROR
                "Duplicate schema ID '${schema_SCHEMA_ID}' has different content")
        endif()
    else()
        set_property(GLOBAL APPEND PROPERTY WILLPOWER_DECLARED_RESOURCE_SCHEMA_IDS
                     "${schema_SCHEMA_ID}")
        set_property(GLOBAL APPEND PROPERTY WILLPOWER_DECLARED_RESOURCE_SCHEMA_HASHES
                     "${_hash}")
    endif()

    string(SHA256 _document_key "${schema_RESOURCE_TYPE}|${schema_FACTORY_TYPE}|${schema_SCHEMA_ID}")
    string(SUBSTRING "${_document_key}" 0 20 _document_key)
    set(_document "application/${_document_key}.schema.json")
    set(_record
        "resourceType|${schema_RESOURCE_TYPE}|${schema_FACTORY_TYPE}|${schema_SCHEMA_ID}|${_document}|${_source}")
    set_property(GLOBAL APPEND PROPERTY WILLPOWER_DECLARED_RESOURCE_SCHEMA_KEYS "${_key}")
    set_property(GLOBAL APPEND PROPERTY WILLPOWER_DECLARED_RESOURCE_SCHEMA_RECORDS "${_record}")
    set_property(GLOBAL APPEND PROPERTY WILLPOWER_DECLARED_RESOURCE_SCHEMA_INPUTS "${_source}")
endfunction()

# Generates a self-contained bundle made from Willpower's built-in bundle and
# every schema declared with willpower_declare_resource_schema().
function(willpower_compose_resource_schema_bundle)
    cmake_parse_arguments(PARSE_ARGV 0 bundle ""
        "TARGET;OUTPUT_DIRECTORY;ROOT_SCHEMA_ID;BUILTIN_BUNDLE;INSTALL_DESTINATION" "")
    if(bundle_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "willpower_compose_resource_schema_bundle received unknown arguments: ${bundle_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_required TARGET OUTPUT_DIRECTORY ROOT_SCHEMA_ID)
        if(NOT DEFINED bundle_${_required} OR bundle_${_required} STREQUAL "")
            message(FATAL_ERROR "willpower_compose_resource_schema_bundle requires ${_required}")
        endif()
    endforeach()
    if(TARGET "${bundle_TARGET}")
        message(FATAL_ERROR "Target '${bundle_TARGET}' already exists")
    endif()
    if(NOT bundle_ROOT_SCHEMA_ID MATCHES "^[A-Za-z][A-Za-z0-9+.-]*:")
        message(FATAL_ERROR "ROOT_SCHEMA_ID '${bundle_ROOT_SCHEMA_ID}' must be an absolute URI")
    endif()
    if(bundle_ROOT_SCHEMA_ID MATCHES "[|;\r\n]")
        message(FATAL_ERROR "ROOT_SCHEMA_ID contains an unsupported character")
    endif()

    if(bundle_BUILTIN_BUNDLE)
        get_filename_component(_builtin_bundle "${bundle_BUILTIN_BUNDLE}" ABSOLUTE
                               BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
        get_property(_builtin_bundle GLOBAL PROPERTY WILLPOWER_BUILTIN_RESOURCE_SCHEMA_BUNDLE_DIR)
        if(NOT _builtin_bundle)
            get_filename_component(_installed_candidate
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../resource-schema-bundle" ABSOLUTE)
            if(EXISTS "${_installed_candidate}/catalog.json")
                set(_builtin_bundle "${_installed_candidate}")
            else()
                message(FATAL_ERROR
                    "Cannot locate Willpower's built-in Resource Schema Bundle; pass BUILTIN_BUNDLE")
            endif()
        endif()
    endif()
    get_filename_component(_output_dir "${bundle_OUTPUT_DIRECTORY}" ABSOLUTE
                           BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    get_property(_builtin_inputs GLOBAL PROPERTY WILLPOWER_BUILTIN_RESOURCE_SCHEMA_INPUTS)
    if(EXISTS "${_builtin_bundle}/schemas")
        file(GLOB_RECURSE _discovered_builtin_inputs
            "${_builtin_bundle}/schemas/*.json")
        list(APPEND _builtin_inputs ${_discovered_builtin_inputs})
    endif()
    list(REMOVE_DUPLICATES _builtin_inputs)

    get_property(_records GLOBAL PROPERTY WILLPOWER_DECLARED_RESOURCE_SCHEMA_RECORDS)
    get_property(_inputs GLOBAL PROPERTY WILLPOWER_DECLARED_RESOURCE_SCHEMA_INPUTS)
    set(_metadata_file "${CMAKE_CURRENT_BINARY_DIR}/${bundle_TARGET}-resource-schema-records.cmake")
    file(WRITE "${_metadata_file}" "set(CUSTOM_RECORDS\n")
    foreach(_record IN LISTS _records)
        file(APPEND "${_metadata_file}" "  [==[${_record}]==]\n")
    endforeach()
    file(APPEND "${_metadata_file}" ")\n")

    set(_stamp "${_output_dir}/generated.stamp")
    set(_root "${_output_dir}/schemas/resource-manifest.schema.json")
    add_custom_command(
        OUTPUT "${_stamp}"
        BYPRODUCTS "${_output_dir}/catalog.json" "${_root}"
        COMMAND "${CMAKE_COMMAND}"
            "-DBASE_BUNDLE_DIR=${_builtin_bundle}"
            "-DCUSTOM_RECORDS_FILE=${_metadata_file}"
            "-DOUTPUT_DIR=${_output_dir}"
            "-DOUTPUT_STAMP=${_stamp}"
            "-DROOT_SCHEMA_ID=${bundle_ROOT_SCHEMA_ID}"
            "-DGENERATOR=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GenerateResourceSchemaBundle.cmake"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ComposeResourceSchemaBundle.cmake"
        DEPENDS
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ComposeResourceSchemaBundle.cmake"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GenerateResourceSchemaBundle.cmake"
            "${_metadata_file}"
            "${_builtin_bundle}/catalog.json"
            ${_builtin_inputs}
            ${_inputs}
        VERBATIM
        COMMENT "Composing ${bundle_TARGET} Resource Schema Bundle")
    add_custom_target("${bundle_TARGET}" ALL DEPENDS "${_stamp}")
    set_target_properties("${bundle_TARGET}" PROPERTIES FOLDER Willpower)
    if(TARGET willpower_resource_schema_bundle AND
       NOT bundle_TARGET STREQUAL "willpower_resource_schema_bundle")
        add_dependencies("${bundle_TARGET}" willpower_resource_schema_bundle)
    endif()

    if(DEFINED bundle_INSTALL_DESTINATION AND NOT bundle_INSTALL_DESTINATION STREQUAL "")
        install(DIRECTORY "${_output_dir}/"
            DESTINATION "${bundle_INSTALL_DESTINATION}"
            PATTERN "generated.stamp" EXCLUDE)
    endif()

    set(${bundle_TARGET}_BUNDLE_DIR "${_output_dir}" PARENT_SCOPE)
endfunction()
