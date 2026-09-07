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

add_library(willpower_downstream_widget_resource STATIC WidgetResource.cpp WidgetResource.h)
target_compile_features(willpower_downstream_widget_resource PRIVATE cxx_std_20)
target_link_libraries(willpower_downstream_widget_resource PRIVATE Willpower.Application)
willpower_target_defaults(willpower_downstream_widget_resource)
set_target_properties(willpower_downstream_widget_resource PROPERTIES FOLDER Willpower/Tests)

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
