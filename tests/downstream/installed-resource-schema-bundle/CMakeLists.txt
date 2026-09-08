cmake_minimum_required(VERSION 3.25)
project(InstalledWillpowerResourceSchemas LANGUAGES NONE)

if(NOT DEFINED WILLPOWER_INSTALL_PREFIX)
    message(FATAL_ERROR "WILLPOWER_INSTALL_PREFIX is required")
endif()

# Consume only files from cmake --install. No Willpower source-tree path or
# application binary is needed to author and compose a schema bundle.
list(APPEND CMAKE_MODULE_PATH
    "${WILLPOWER_INSTALL_PREFIX}/share/willpower/cmake")
include(ResourceSchemas)

willpower_declare_resource_schema(
    RESOURCE_TYPE Widget
    SCHEMA_ID "https://schemas.example.test/widget.schema.json"
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../resource-schema-bundle/schemas/widget.schema.json")
willpower_declare_resource_schema(
    RESOURCE_TYPE Widget
    FACTORY_TYPE GlowFactory
    SCHEMA_ID "https://schemas.example.test/widget-glow.schema.json"
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../resource-schema-bundle/schemas/widget-glow.schema.json")

willpower_compose_resource_schema_bundle(
    TARGET installed_application_resource_schemas
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/resource-schema-bundle"
    ROOT_SCHEMA_ID "https://schemas.example.test/resource-manifest.schema.json")
