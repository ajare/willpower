willpower_declare_resource_schema(
    RESOURCE_TYPE Widget
    SCHEMA_ID "https://schemas.example.test/widget.schema.json"
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/schemas/widget.schema.json")
willpower_declare_resource_schema(
    RESOURCE_TYPE Widget
    FACTORY_TYPE GlowFactory
    SCHEMA_ID "https://schemas.example.test/widget-glow.schema.json"
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/schemas/widget-glow.schema.json")

willpower_compose_resource_schema_bundle(
    TARGET willpower_downstream_resource_schema_bundle
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/resource-schema-bundle"
    ROOT_SCHEMA_ID "https://schemas.example.test/resource-manifest.schema.json")

add_executable(willpower_downstream_schema_exporter schema_exporter.cpp)
target_compile_features(willpower_downstream_schema_exporter PRIVATE cxx_std_20)
target_compile_definitions(willpower_downstream_schema_exporter PRIVATE
    WILLPOWER_EXAMPLE_SCHEMA_BUNDLE="${willpower_downstream_resource_schema_bundle_BUNDLE_DIR}")
target_link_libraries(willpower_downstream_schema_exporter PRIVATE Willpower.Application)
willpower_target_defaults(willpower_downstream_schema_exporter)
willpower_output_dirs(willpower_downstream_schema_exporter)
willpower_deploy_runtime_dlls(willpower_downstream_schema_exporter)
willpower_deploy_vendor_dlls(willpower_downstream_schema_exporter)
set_target_properties(willpower_downstream_schema_exporter PROPERTIES FOLDER Willpower/Tests)
add_dependencies(willpower_downstream_schema_exporter
    willpower_downstream_resource_schema_bundle)

add_library(willpower_downstream_widget_resource STATIC WidgetResource.cpp WidgetResource.h)
target_compile_features(willpower_downstream_widget_resource PRIVATE cxx_std_20)
target_link_libraries(willpower_downstream_widget_resource PRIVATE Willpower.Application)
willpower_target_defaults(willpower_downstream_widget_resource)
set_target_properties(willpower_downstream_widget_resource PROPERTIES FOLDER Willpower/Tests)

# This executable is intentionally independent of Willpower.Application: it is
# the language-neutral external consumer in the end-to-end acceptance test.
add_executable(willpower_downstream_external_schema_consumer external_schema_consumer.cpp)
target_compile_features(willpower_downstream_external_schema_consumer PRIVATE cxx_std_20)
target_link_libraries(willpower_downstream_external_schema_consumer PRIVATE ext::Utils)
willpower_target_defaults(willpower_downstream_external_schema_consumer)
willpower_output_dirs(willpower_downstream_external_schema_consumer)
willpower_deploy_runtime_dlls(willpower_downstream_external_schema_consumer)
willpower_deploy_vendor_dlls(willpower_downstream_external_schema_consumer)
set_target_properties(willpower_downstream_external_schema_consumer PROPERTIES
    FOLDER Willpower/Tests)

add_executable(willpower_downstream_resource_schema_tests bundle_tests.cpp)
target_compile_features(willpower_downstream_resource_schema_tests PRIVATE cxx_std_20)
target_link_libraries(willpower_downstream_resource_schema_tests
    PRIVATE Willpower.Application ext::Utils willpower_downstream_widget_resource)
willpower_target_defaults(willpower_downstream_resource_schema_tests)
willpower_output_dirs(willpower_downstream_resource_schema_tests)
willpower_deploy_runtime_dlls(willpower_downstream_resource_schema_tests)
willpower_deploy_vendor_dlls(willpower_downstream_resource_schema_tests)
set_target_properties(willpower_downstream_resource_schema_tests PROPERTIES FOLDER Willpower/Tests)
add_dependencies(willpower_downstream_resource_schema_tests
    willpower_downstream_resource_schema_bundle)
willpower_add_test(NAME willpower_downstream_resource_schema_bundle
    COMMAND willpower_downstream_resource_schema_tests
        "${willpower_downstream_resource_schema_bundle_BUNDLE_DIR}")

willpower_add_test(NAME willpower_downstream_resource_schema_acceptance
    COMMAND "${CMAKE_COMMAND}"
        "-DEXTERNAL_CONSUMER=$<TARGET_FILE:willpower_downstream_external_schema_consumer>"
        "-DRUNTIME_CONSUMER=$<TARGET_FILE:willpower_downstream_resource_schema_tests>"
        "-DBUNDLE_DIR=${willpower_downstream_resource_schema_bundle_BUNDLE_DIR}"
        "-DMANIFEST_DIR=${CMAKE_CURRENT_SOURCE_DIR}/manifests"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/compare_consumers.cmake")

willpower_add_test(NAME willpower_downstream_installed_resource_schema_bundle
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
        "-DSOURCE_BUNDLE=${willpower_downstream_resource_schema_bundle_BUNDLE_DIR}"
        "-DINSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/installed-prefix"
        "-DDOWNSTREAM_SOURCE=${PROJECT_SOURCE_DIR}/tests/downstream/installed-resource-schema-bundle"
        "-DDOWNSTREAM_BUILD=${CMAKE_CURRENT_BINARY_DIR}/installed-build"
        "-DEXTERNAL_CONSUMER=$<TARGET_FILE:willpower_downstream_external_schema_consumer>"
        "-DMANIFEST_DIR=${CMAKE_CURRENT_SOURCE_DIR}/manifests"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DCONFIG=$<CONFIG>"
        -P "${PROJECT_SOURCE_DIR}/tests/downstream/installed-resource-schema-bundle/installed_package_test.cmake")

willpower_add_test(NAME willpower_resource_schema_cli
    COMMAND "${CMAKE_COMMAND}"
        "-DTOOL=$<TARGET_FILE:willpower_resource_schemas>"
        "-DDOWNSTREAM_TOOL=$<TARGET_FILE:willpower_downstream_schema_exporter>"
        "-DEXTERNAL_CONSUMER=$<TARGET_FILE:willpower_downstream_external_schema_consumer>"
        "-DMANIFEST_DIR=${CMAKE_CURRENT_SOURCE_DIR}/manifests"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-test"
        -P "${PROJECT_SOURCE_DIR}/willpower.application/tests/resource_schema_cli_tests.cmake")
