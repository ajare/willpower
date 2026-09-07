cmake_policy(VERSION 3.25)

foreach(_required BASE_BUNDLE_DIR CUSTOM_RECORDS_FILE OUTPUT_DIR ROOT_SCHEMA_ID GENERATOR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()
if(NOT EXISTS "${BASE_BUNDLE_DIR}/catalog.json")
    message(FATAL_ERROR "Built-in Resource Schema Bundle is missing catalog.json: ${BASE_BUNDLE_DIR}")
endif()
include("${CUSTOM_RECORDS_FILE}")

function(_wp_json_quote OUTPUT VALUE)
    set(_value "${VALUE}")
    string(REPLACE "\\" "\\\\" _value "${_value}")
    string(REPLACE "\"" "\\\"" _value "${_value}")
    string(REPLACE "\r" "\\r" _value "${_value}")
    string(REPLACE "\n" "\\n" _value "${_value}")
    string(REPLACE "\t" "\\t" _value "${_value}")
    set(${OUTPUT} "\"${_value}\"" PARENT_SCOPE)
endfunction()

file(READ "${BASE_BUNDLE_DIR}/catalog.json" _base_catalog)
string(JSON _format ERROR_VARIABLE _format_error GET "${_base_catalog}" bundleFormatVersion)
string(JSON _manifest_version ERROR_VARIABLE _version_error GET
       "${_base_catalog}" resourceManifestSchemaVersion)
if(_format_error OR _version_error OR NOT _format STREQUAL "1.0" OR
   NOT _manifest_version STREQUAL "1.0")
    message(FATAL_ERROR "The built-in bundle must use supported bundle and manifest version 1.0")
endif()
string(JSON _base_count ERROR_VARIABLE _catalog_error LENGTH "${_base_catalog}" schemas)
if(_catalog_error OR _base_count LESS 1)
    message(FATAL_ERROR "The built-in bundle catalog has no schemas")
endif()

set(RECORDS "")
set(_lookup_records "")
set(_manifest_count 0)
math(EXPR _base_last "${_base_count} - 1")
foreach(_index RANGE 0 ${_base_last})
    string(JSON _kind GET "${_base_catalog}" schemas ${_index} kind)
    string(JSON _resource_type_type TYPE "${_base_catalog}" schemas ${_index} resourceType)
    if(_resource_type_type STREQUAL "NULL")
        set(_resource_type "")
    else()
        string(JSON _resource_type GET "${_base_catalog}" schemas ${_index} resourceType)
    endif()
    string(JSON _factory_type_type TYPE "${_base_catalog}" schemas ${_index} factoryType)
    if(_factory_type_type STREQUAL "NULL")
        set(_factory_type "")
    else()
        string(JSON _factory_type GET "${_base_catalog}" schemas ${_index} factoryType)
    endif()
    string(JSON _schema_id GET "${_base_catalog}" schemas ${_index} schemaId)
    string(JSON _document GET "${_base_catalog}" schemas ${_index} document)
    string(JSON _declared_hash GET "${_base_catalog}" schemas ${_index} documentHash)
    if(NOT _document MATCHES "^schemas/(.+)$")
        message(FATAL_ERROR "Built-in catalog document has an unsupported path: ${_document}")
    endif()
    set(_document_name "${CMAKE_MATCH_1}")
    set(_source "${BASE_BUNDLE_DIR}/${_document}")
    if(NOT EXISTS "${_source}" OR IS_DIRECTORY "${_source}")
        message(FATAL_ERROR "Built-in catalog document is missing: ${_document}")
    endif()
    file(SHA256 "${_source}" _actual_hash)
    if(NOT _declared_hash STREQUAL "sha256:${_actual_hash}")
        message(FATAL_ERROR "Built-in catalog hash mismatch for ${_document}")
    endif()

    if(_kind STREQUAL "manifest")
        math(EXPR _manifest_count "${_manifest_count} + 1")
    else()
        list(APPEND RECORDS
            "${_kind}|${_resource_type}|${_factory_type}|${_schema_id}|${_document_name}|${_source}")
        if(_kind STREQUAL "resourceType")
            list(APPEND _lookup_records "${_resource_type}|${_factory_type}|${_schema_id}")
        endif()
    endif()
endforeach()
if(NOT _manifest_count EQUAL 1)
    message(FATAL_ERROR "The built-in bundle must contain exactly one manifest schema")
endif()

foreach(_record IN LISTS CUSTOM_RECORDS)
    string(REPLACE "|" ";" _fields "${_record}")
    list(LENGTH _fields _field_count)
    if(NOT _field_count EQUAL 6)
        message(FATAL_ERROR "Invalid declared Resource schema record: ${_record}")
    endif()
    list(GET _fields 1 _resource_type)
    list(GET _fields 2 _factory_type)
    list(GET _fields 3 _schema_id)
    list(APPEND RECORDS "${_record}")
    list(APPEND _lookup_records "${_resource_type}|${_factory_type}|${_schema_id}")
endforeach()

# Index the complete Resource Type catalog. Duplicate keys and schema IDs are
# finally checked by GenerateResourceSchemaBundle.cmake, including collisions
# between an installed built-in bundle and downstream declarations.
set(_resource_types "")
foreach(_lookup IN LISTS _lookup_records)
    string(REPLACE "|" ";" _fields "${_lookup}")
    list(GET _fields 0 _resource_type)
    list(APPEND _resource_types "${_resource_type}")
endforeach()
list(REMOVE_DUPLICATES _resource_types)
list(SORT _resource_types)

_wp_json_quote(_root_id_json "${ROOT_SCHEMA_ID}")
set(_common_id "https://schemas.willpower.dev/resource-manifest/common.schema.json")
set(_resource_id "https://schemas.willpower.dev/resource-manifest/resource.schema.json")
_wp_json_quote(_common_name_ref "${_common_id}#/definitions/nonEmptyString")
_wp_json_quote(_common_resource_ref "${_resource_id}#/definitions/resource")
_wp_json_quote(_generic_definition_ref "${_resource_id}#/definitions/definition")

set(_resource_branches "")
set(_generated_definitions "")
set(_known_type_values "")
set(_branch_separator "")
set(_definition_separator "")
set(_resource_number 0)
foreach(_resource_type IN LISTS _resource_types)
    _wp_json_quote(_resource_type_json "${_resource_type}")
    string(APPEND _known_type_values "${_branch_separator}${_resource_type_json}")

    set(_default_id "")
    set(_special_factories "")
    set(_special_ids "")
    foreach(_lookup IN LISTS _lookup_records)
        string(REPLACE "|" ";" _fields "${_lookup}")
        list(GET _fields 0 _candidate_resource)
        list(GET _fields 1 _candidate_factory)
        list(GET _fields 2 _candidate_id)
        if(_candidate_resource STREQUAL _resource_type)
            if(_candidate_factory STREQUAL "")
                set(_default_id "${_candidate_id}")
            else()
                list(APPEND _special_factories "${_candidate_factory}")
                list(APPEND _special_ids "${_candidate_id}")
            endif()
        endif()
    endforeach()

    set(_dispatch_constraint "")
    if(_special_factories)
        # Keep factory/schema pairs together while imposing deterministic factory order.
        set(_special_pairs "")
        list(LENGTH _special_factories _special_count)
        math(EXPR _special_last "${_special_count} - 1")
        foreach(_special_index RANGE 0 ${_special_last})
            list(GET _special_factories ${_special_index} _factory)
            list(GET _special_ids ${_special_index} _special_id)
            list(APPEND _special_pairs "${_factory}|${_special_id}")
        endforeach()
        list(SORT _special_pairs)

        set(_definition_branches "")
        set(_registered_factory_values "")
        set(_special_separator "")
        foreach(_pair IN LISTS _special_pairs)
            string(REPLACE "|" ";" _pair_fields "${_pair}")
            list(GET _pair_fields 0 _factory)
            list(GET _pair_fields 1 _special_id)
            _wp_json_quote(_factory_json "${_factory}")
            _wp_json_quote(_special_ref_json "${_special_id}")
            string(APPEND _registered_factory_values "${_special_separator}${_factory_json}")
            string(APPEND _definition_branches
                "${_special_separator}        {\n"
                "          \"allOf\": [\n"
                "            { \"$ref\": ${_generic_definition_ref} },\n"
                "            { \"required\": [\"factory\"], \"properties\": { \"factory\": { \"enum\": [${_factory_json}] } } },\n"
                "            { \"$ref\": ${_special_ref_json} }\n"
                "          ]\n"
                "        }")
            set(_special_separator ",\n")
        endforeach()
        string(APPEND _definition_branches
            ",\n        {\n"
            "          \"allOf\": [\n"
            "            { \"$ref\": ${_generic_definition_ref} },\n"
            "            { \"not\": { \"required\": [\"factory\"], \"properties\": { \"factory\": { \"enum\": [${_registered_factory_values}] } } } }\n"
            "          ]\n"
            "        }")

        string(APPEND _generated_definitions
            "${_definition_separator}    \"applicationDefinition${_resource_number}\": {\n"
            "      \"oneOf\": [\n${_definition_branches}\n      ]\n"
            "    },\n"
            "    \"applicationDefinitionCollection${_resource_number}\": {\n"
            "      \"oneOf\": [\n"
            "        { \"$ref\": \"#/definitions/applicationDefinition${_resource_number}\" },\n"
            "        { \"type\": \"array\", \"minItems\": 1, \"items\": { \"$ref\": \"#/definitions/applicationDefinition${_resource_number}\" } }\n"
            "      ]\n"
            "    },\n"
            "    \"applicationDefinitions${_resource_number}\": {\n"
            "      \"type\": \"object\",\n"
            "      \"additionalProperties\": false,\n"
            "      \"required\": [\"Definition\"],\n"
            "      \"properties\": { \"Definition\": { \"$ref\": \"#/definitions/applicationDefinitionCollection${_resource_number}\" } }\n"
            "    }")
        set(_definition_separator ",\n")
        set(_dispatch_constraint
            ",\n          { \"properties\": { \"Definitions\": { \"$ref\": \"#/definitions/applicationDefinitions${_resource_number}\" } } }")
    endif()

    if(_default_id)
        _wp_json_quote(_default_ref_json "${_default_id}")
        set(_base_constraint "{ \"$ref\": ${_default_ref_json} }")
    else()
        set(_base_constraint
            "{ \"allOf\": [ { \"$ref\": ${_common_resource_ref} }, { \"properties\": { \"type\": { \"enum\": [${_resource_type_json}] } } } ] }")
    endif()
    string(APPEND _resource_branches
        "${_branch_separator}        {\n"
        "          \"allOf\": [\n"
        "          ${_base_constraint}${_dispatch_constraint}\n"
        "          ]\n"
        "        }")
    set(_branch_separator ",\n")
    math(EXPR _resource_number "${_resource_number} + 1")
endforeach()

# Registered names are excluded from this branch, so an invalid registered
# declaration cannot silently pass as an unknown custom Resource Type.
string(APPEND _resource_branches
    "${_branch_separator}        {\n"
    "          \"allOf\": [\n"
    "            { \"$ref\": ${_common_resource_ref} },\n"
    "            { \"properties\": { \"type\": { \"not\": { \"enum\": [${_known_type_values}] } } } }\n"
    "          ]\n"
    "        }")

if(NOT _generated_definitions STREQUAL "")
    string(APPEND _generated_definitions ",\n")
endif()
string(CONCAT _root_schema
"{\n"
"  \"$schema\": \"http://json-schema.org/draft-07/schema#\",\n"
"  \"$id\": ${_root_id_json},\n"
"  \"title\": \"Application Resource Manifest\",\n"
"  \"type\": \"object\",\n"
"  \"additionalProperties\": false,\n"
"  \"required\": [\"Resources\"],\n"
"  \"properties\": {\n"
"    \"Resources\": {\n"
"      \"type\": \"object\",\n"
"      \"additionalProperties\": false,\n"
"      \"properties\": {\n"
"        \"Resource\": { \"$ref\": \"#/definitions/resourceCollection\" },\n"
"        \"Namespace\": {\n"
"          \"oneOf\": [\n"
"            { \"$ref\": \"#/definitions/namespace\" },\n"
"            { \"type\": \"array\", \"minItems\": 1, \"items\": { \"$ref\": \"#/definitions/namespace\" } }\n"
"          ]\n"
"        }\n"
"      }\n"
"    }\n"
"  },\n"
"  \"definitions\": {\n"
"${_generated_definitions}"
"    \"resource\": {\n"
"      \"oneOf\": [\n${_resource_branches}\n      ]\n"
"    },\n"
"    \"resourceCollection\": {\n"
"      \"oneOf\": [\n"
"        { \"$ref\": \"#/definitions/resource\" },\n"
"        { \"type\": \"array\", \"minItems\": 1, \"items\": { \"$ref\": \"#/definitions/resource\" } }\n"
"      ]\n"
"    },\n"
"    \"namespace\": {\n"
"      \"type\": \"object\",\n"
"      \"additionalProperties\": false,\n"
"      \"required\": [\"name\", \"Resource\"],\n"
"      \"properties\": {\n"
"        \"name\": { \"$ref\": ${_common_name_ref} },\n"
"        \"Resource\": { \"$ref\": \"#/definitions/resourceCollection\" }\n"
"      }\n"
"    }\n"
"  }\n"
"}\n")

set(_working_dir "${OUTPUT_DIR}.working")
file(REMOVE_RECURSE "${_working_dir}")
file(MAKE_DIRECTORY "${_working_dir}")
set(_root_source "${_working_dir}/resource-manifest.schema.json")
file(WRITE "${_root_source}" "${_root_schema}")
list(APPEND RECORDS
    "manifest|||${ROOT_SCHEMA_ID}|resource-manifest.schema.json|${_root_source}")
set(OUTPUT_DIR "${OUTPUT_DIR}")
include("${GENERATOR}")
file(REMOVE_RECURSE "${_working_dir}")
