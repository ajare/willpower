# Generates the private immutable backing data for the public Resource Schema Catalog.
# RECORDS entries are:
#   kind|resource-type|factory-type|schema-id|document-name|absolute-path.
if(NOT DEFINED OUTPUT_HEADER OR NOT DEFINED OUTPUT_SOURCE)
    message(FATAL_ERROR "OUTPUT_HEADER and OUTPUT_SOURCE are required")
endif()

set(_initialiser "")
set(_count 0)
foreach(_record IN LISTS RECORDS)
    string(REPLACE "|" ";" _fields "${_record}")
    list(LENGTH _fields _field_count)
    if(NOT _field_count EQUAL 6)
        message(FATAL_ERROR "Invalid embedded schema record: ${_record}")
    endif()
    list(GET _fields 0 _kind)
    list(GET _fields 1 _resource_type)
    list(GET _fields 2 _factory_type)
    list(GET _fields 3 _schema_id)
    list(GET _fields 4 _document)
    list(GET _fields 5 _path)
    # Encode document bytes rather than interpolating them as a raw string.
    # On Windows, interpolating an existing CRLF into generated text can turn
    # it into CR-CR-LF and make the embedded bytes differ from the bundle hash.
    file(READ "${_path}" _source_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\\\x\\1" _source_literal "${_source_hex}")
    string(LENGTH "${_source_hex}" _source_hex_length)
    math(EXPR _source_size "${_source_hex_length} / 2")
    file(SHA256 "${_path}" _hash)
    set(_document_hash "sha256:${_hash}")
    foreach(_value_var _kind _resource_type _factory_type _schema_id _document _document_hash)
        if("${${_value_var}}" MATCHES "\\)WP_SCHEMA\"")
            message(FATAL_ERROR "${_path} contains the reserved embedding delimiter")
        endif()
    endforeach()
    string(APPEND _initialiser
        "    EmbeddedSchemaRecord{R\"WP_SCHEMA(${_kind})WP_SCHEMA\", "
        "R\"WP_SCHEMA(${_resource_type})WP_SCHEMA\", "
        "R\"WP_SCHEMA(${_factory_type})WP_SCHEMA\", "
        "R\"WP_SCHEMA(${_schema_id})WP_SCHEMA\", "
        "R\"WP_SCHEMA(schemas/${_document})WP_SCHEMA\", "
        "R\"WP_SCHEMA(${_document_hash})WP_SCHEMA\", "
        "std::string_view(\"${_source_literal}\", ${_source_size})},\n")
    math(EXPR _count "${_count} + 1")
endforeach()

get_filename_component(_header_dir "${OUTPUT_HEADER}" DIRECTORY)
get_filename_component(_source_dir "${OUTPUT_SOURCE}" DIRECTORY)
file(MAKE_DIRECTORY "${_header_dir}" "${_source_dir}")
file(WRITE "${OUTPUT_HEADER}" [=[#pragma once

#include <span>
#include <string_view>

namespace wp::application::resourcesystem::detail {
struct EmbeddedSchemaRecord {
  std::string_view kind;
  std::string_view resourceType;
  std::string_view factoryType;
  std::string_view id;
  std::string_view document;
  std::string_view documentHash;
  std::string_view source;
};
std::span<EmbeddedSchemaRecord const> embeddedResourceManifestSchemas();
}  // namespace wp::application::resourcesystem::detail
]=])
file(WRITE "${OUTPUT_SOURCE}"
    "#include \"EmbeddedResourceManifestSchemas.h\"\n\n"
    "#include <array>\n\n"
    "namespace wp::application::resourcesystem::detail {\n"
    "namespace {\n"
    "constexpr std::array<EmbeddedSchemaRecord, ${_count}> schemas{{\n"
    "${_initialiser}"
    "}};\n"
    "}  // namespace\n\n"
    "std::span<EmbeddedSchemaRecord const> embeddedResourceManifestSchemas() { return schemas; }\n"
    "}  // namespace wp::application::resourcesystem::detail\n")
