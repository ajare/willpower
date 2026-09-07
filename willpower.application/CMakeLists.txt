set(WILLPOWER_EMBEDDED_SCHEMA_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(WILLPOWER_EMBEDDED_SCHEMA_HEADER
    "${WILLPOWER_EMBEDDED_SCHEMA_DIR}/EmbeddedResourceManifestSchemas.h")
set(WILLPOWER_EMBEDDED_SCHEMA_SOURCE
    "${WILLPOWER_EMBEDDED_SCHEMA_DIR}/EmbeddedResourceManifestSchemas.cpp")

# This is the single catalog of built-in schema metadata. Both the private C++
# embedding and the distributable, language-neutral bundle are derived from it;
# the JSON documents remain the only hand-edited schema definitions.
set(WILLPOWER_BUILTIN_SCHEMA_RECORDS
    "manifest|||https://schemas.willpower.dev/resource-manifest/resource-manifest.schema.json|resource-manifest.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/schemas/resource-manifest.schema.json"
    "dependency|||https://schemas.willpower.dev/resource-manifest/common.schema.json|common.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/schemas/common.schema.json"
    "dependency|||https://schemas.willpower.dev/resource-manifest/resource.schema.json|resource.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/schemas/resource.schema.json"
    "resourceType|TextFile||https://schemas.willpower.dev/resource-manifest/text-file.schema.json|text-file.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/schemas/text-file.schema.json"
    "resourceType|XmlFile||https://schemas.willpower.dev/resource-manifest/xml-file.schema.json|xml-file.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/schemas/xml-file.schema.json"
    "resourceType|Shader||https://schemas.willpower.dev/resource-manifest/shader.schema.json|shader.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/schemas/shader.schema.json"
    "resourceType|AudioBank||https://schemas.willpower.dev/resource-manifest/audio-bank.schema.json|audio-bank.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/schemas/audio-bank.schema.json"
    "resourceType|Image||https://schemas.willpower.dev/resource-manifest/image.schema.json|image.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/schemas/image.schema.json"
    "resourceType|ImageSet||https://schemas.willpower.dev/resource-manifest/image-set.schema.json|image-set.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/schemas/image-set.schema.json"
    "resourceType|AnimationSet||https://schemas.willpower.dev/resource-manifest/animation-set.schema.json|animation-set.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/schemas/animation-set.schema.json"
    "resourceType|Program||https://schemas.willpower.dev/resource-manifest/program.schema.json|program.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/schemas/program.schema.json"
    "resourceType|Material||https://schemas.willpower.dev/resource-manifest/material.schema.json|material.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/schemas/material.schema.json")

set(WILLPOWER_SCHEMA_RECORDS ${WILLPOWER_BUILTIN_SCHEMA_RECORDS})
set(WILLPOWER_SCHEMA_INPUTS "")
foreach(_record IN LISTS WILLPOWER_BUILTIN_SCHEMA_RECORDS)
    string(REPLACE "|" ";" _fields "${_record}")
    list(GET _fields 5 _schema_path)
    list(APPEND WILLPOWER_SCHEMA_INPUTS "${_schema_path}")
endforeach()
unset(_record)
unset(_fields)
unset(_schema_path)

set(WILLPOWER_RESOURCE_SCHEMA_BUNDLE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/resource-schema-bundle")
set(WILLPOWER_RESOURCE_SCHEMA_BUNDLE_STAMP
    "${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_DIR}/generated.stamp")
set(WILLPOWER_RESOURCE_SCHEMA_BUNDLE_RECORDS_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/ResourceSchemaBundleRecords.cmake")
file(WRITE "${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_RECORDS_FILE}" "set(RECORDS\n")
foreach(_record IN LISTS WILLPOWER_BUILTIN_SCHEMA_RECORDS)
    file(APPEND "${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_RECORDS_FILE}" "  [==[${_record}]==]\n")
endforeach()
file(APPEND "${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_RECORDS_FILE}" ")\n")
add_custom_command(
    OUTPUT "${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_STAMP}"
    BYPRODUCTS "${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_DIR}/catalog.json"
    COMMAND "${CMAKE_COMMAND}"
        "-DOUTPUT_DIR=${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_DIR}"
        "-DOUTPUT_STAMP=${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_STAMP}"
        "-DRECORDS_FILE=${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_RECORDS_FILE}"
        -P "${PROJECT_SOURCE_DIR}/cmake/GenerateResourceSchemaBundle.cmake"
    DEPENDS "${PROJECT_SOURCE_DIR}/cmake/GenerateResourceSchemaBundle.cmake"
            "${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_RECORDS_FILE}"
            ${WILLPOWER_SCHEMA_INPUTS}
    VERBATIM)
add_custom_target(willpower_resource_schema_bundle ALL
    DEPENDS "${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_STAMP}")
set_target_properties(willpower_resource_schema_bundle PROPERTIES FOLDER Willpower)

include(GNUInstallDirs)
install(DIRECTORY "${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_DIR}/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/willpower/resource-schema-bundle"
    COMPONENT Willpower_Application
    PATTERN "generated.stamp" EXCLUDE)

add_custom_command(
    OUTPUT "${WILLPOWER_EMBEDDED_SCHEMA_HEADER}" "${WILLPOWER_EMBEDDED_SCHEMA_SOURCE}"
    COMMAND "${CMAKE_COMMAND}"
        "-DOUTPUT_HEADER=${WILLPOWER_EMBEDDED_SCHEMA_HEADER}"
        "-DOUTPUT_SOURCE=${WILLPOWER_EMBEDDED_SCHEMA_SOURCE}"
        "-DRECORDS=${WILLPOWER_SCHEMA_RECORDS}"
        -P "${PROJECT_SOURCE_DIR}/cmake/EmbedSchemaCatalog.cmake"
    DEPENDS "${PROJECT_SOURCE_DIR}/cmake/EmbedSchemaCatalog.cmake" ${WILLPOWER_SCHEMA_INPUTS}
    VERBATIM)
set_source_files_properties(
    "${WILLPOWER_EMBEDDED_SCHEMA_HEADER}" "${WILLPOWER_EMBEDDED_SCHEMA_SOURCE}"
    PROPERTIES GENERATED TRUE)

add_library(Willpower.Application SHARED
    "${WILLPOWER_EMBEDDED_SCHEMA_HEADER}"
    "${WILLPOWER_EMBEDDED_SCHEMA_SOURCE}"
    src/ApplicationSettings.cpp
    src/AudioSystem.cpp
    src/InputHelper.cpp
    src/InputStateManager.cpp
    src/PlatformTimer.cpp
    src/Scheduler.cpp
    src/SchedulerTask.cpp
    src/ServiceLocator.cpp
    src/State.cpp
    src/StateFactory.cpp
    src/resourcesystem/AnimationSetDefaultDefinitionFactory.cpp
    src/resourcesystem/AnimationSetResource.cpp
    src/resourcesystem/AnimationSetResourceDefinitionFactory.cpp
    src/resourcesystem/AudioBankDefaultDefinitionFactory.cpp
    src/resourcesystem/AudioBankResource.cpp
    src/resourcesystem/AudioBankResourceDefinitionFactory.cpp
    src/resourcesystem/DataStream.cpp
    src/resourcesystem/DirectoryResourceLocation.cpp
    src/resourcesystem/ImageDefaultDefinitionFactory.cpp
    src/resourcesystem/ImageResource.cpp
    src/resourcesystem/ImageResourceDefinitionFactory.cpp
    src/resourcesystem/ImageSetDefaultDefinitionFactory.cpp
    src/resourcesystem/ImageSetResource.cpp
    src/resourcesystem/ImageSetResourceDefinitionFactory.cpp
    src/resourcesystem/MaterialResource.cpp
    src/resourcesystem/MaterialResourceDefaultDefinitionFactory.cpp
    src/resourcesystem/MaterialResourceDefinitionFactory.cpp
    src/resourcesystem/ProgramDefaultDefinitionFactory.cpp
    src/resourcesystem/ProgramResource.cpp
    src/resourcesystem/ProgramResourceDefinitionFactory.cpp
    src/resourcesystem/Resource.cpp
    src/resourcesystem/ResourceDefinitionFactory.cpp
    src/resourcesystem/ResourceLocation.cpp
    src/resourcesystem/ResourceManager.cpp
    src/resourcesystem/ResourceSchemaCatalog.cpp
    src/resourcesystem/ResourceManifestValidator.cpp
    src/resourcesystem/ResourceManifestValidator.h
    src/resourcesystem/ShaderDefaultDefinitionFactory.cpp
    src/resourcesystem/ShaderResource.cpp
    src/resourcesystem/ShaderResourceDefinitionFactory.cpp
    # Defines TextFileDefaultDefinitionFactory - the file name is a typo.
    src/resourcesystem/TestFileDefaultDefinitionFactory.cpp
    src/resourcesystem/TextFileResource.cpp
    src/resourcesystem/TextFileResourceDefinitionFactory.cpp
    src/resourcesystem/XmlFileDefaultDefinitionFactory.cpp
    src/resourcesystem/XmlFileResource.cpp
    src/resourcesystem/XmlFileResourceDefinitionFactory.cpp)

target_include_directories(Willpower.Application
    PUBLIC include
    PRIVATE "${WILLPOWER_EMBEDDED_SCHEMA_DIR}"
            "${WILLPOWER_NLOHMANN_JSON_INCLUDE_DIR}")
willpower_add_header_filter(Willpower.Application)

target_compile_definitions(Willpower.Application
    PUBLIC  $<$<BOOL:${WILLPOWER_ENABLE_FMOD}>:WP_APPLICATION_USE_FMOD>
    PRIVATE $<$<PLATFORM_ID:Windows>:WIN32 _WINDOWS>
            WP_APPLICATION_DLL_EXPORT
            WP_USE_ASSERT_TRACE
            $<$<CONFIG:Debug>:WP_USE_MEMLEAK_TRACKING>)

target_link_libraries(Willpower.Application
    PUBLIC  Willpower.Common
            ext::mpp ext::mpp-mesh ext::mpp-program
    PRIVATE ext::Utils)
if(WILLPOWER_ENABLE_FMOD)
    target_link_libraries(Willpower.Application PRIVATE vendor::fmod vendor::fmodstudio)
endif()

target_compile_features(Willpower.Application PRIVATE cxx_std_20)
willpower_target_defaults(Willpower.Application)
willpower_output_dirs(Willpower.Application)
willpower_enable_sdl_checks(Willpower.Application)
set_target_properties(Willpower.Application PROPERTIES FOLDER Willpower)
add_dependencies(Willpower.Application willpower_resource_schema_bundle)

if(BUILD_TESTING)
    willpower_add_test(NAME willpower_resource_schema_bundle
        COMMAND "${CMAKE_COMMAND}"
            "-DBUNDLE_DIR=${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_DIR}"
            "-DREGENERATED_DIR=${CMAKE_CURRENT_BINARY_DIR}/resource-schema-bundle-repeated"
            "-DGENERATOR=${PROJECT_SOURCE_DIR}/cmake/GenerateResourceSchemaBundle.cmake"
            "-DRECORDS_FILE=${WILLPOWER_RESOURCE_SCHEMA_BUNDLE_RECORDS_FILE}"
            -P "${PROJECT_SOURCE_DIR}/cmake/VerifyResourceSchemaBundle.cmake")

    add_executable(willpower_application_schema_catalog_tests
        tests/schema_catalog_tests.cpp)
    target_compile_features(willpower_application_schema_catalog_tests PRIVATE cxx_std_20)
    target_link_libraries(willpower_application_schema_catalog_tests PRIVATE Willpower.Application)
    willpower_target_defaults(willpower_application_schema_catalog_tests)
    willpower_output_dirs(willpower_application_schema_catalog_tests)
    set_target_properties(willpower_application_schema_catalog_tests PROPERTIES FOLDER Willpower)
    willpower_deploy_runtime_dlls(willpower_application_schema_catalog_tests)
    willpower_deploy_vendor_dlls(willpower_application_schema_catalog_tests)
    willpower_add_test(NAME willpower_application_schema_catalog
        COMMAND willpower_application_schema_catalog_tests)
    set(WILLPOWER_SCHEMA_TEST_WORKING_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/schema-test-working-directory")
    file(MAKE_DIRECTORY "${WILLPOWER_SCHEMA_TEST_WORKING_DIR}")
    set_tests_properties(willpower_application_schema_catalog PROPERTIES
        WORKING_DIRECTORY "${WILLPOWER_SCHEMA_TEST_WORKING_DIR}")

    add_executable(willpower_application_resource_yaml_tests tests/resource_yaml_tests.cpp)
    target_compile_features(willpower_application_resource_yaml_tests PRIVATE cxx_std_20)
    target_link_libraries(willpower_application_resource_yaml_tests PRIVATE Willpower.Application)
    willpower_target_defaults(willpower_application_resource_yaml_tests)
    willpower_output_dirs(willpower_application_resource_yaml_tests)
    set_target_properties(willpower_application_resource_yaml_tests PROPERTIES FOLDER Willpower)
    willpower_deploy_runtime_dlls(willpower_application_resource_yaml_tests)
    willpower_deploy_vendor_dlls(willpower_application_resource_yaml_tests)
    willpower_add_test(NAME willpower_application_resource_yaml
        COMMAND willpower_application_resource_yaml_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/Resources.yaml")

    add_executable(willpower_application_resource_definition_tests
        tests/resource_definition_tests.cpp)
    target_compile_features(willpower_application_resource_definition_tests PRIVATE cxx_std_20)
    target_link_libraries(willpower_application_resource_definition_tests PRIVATE Willpower.Application)
    willpower_target_defaults(willpower_application_resource_definition_tests)
    willpower_output_dirs(willpower_application_resource_definition_tests)
    set_target_properties(willpower_application_resource_definition_tests PROPERTIES FOLDER Willpower)
    willpower_deploy_runtime_dlls(willpower_application_resource_definition_tests)
    willpower_deploy_vendor_dlls(willpower_application_resource_definition_tests)
    foreach(scenario specialized-before-default idempotent-scan duplicate-default construct-destroy-reconstruct overlapping-construction)
        willpower_add_test(NAME willpower_application_resource_definition_${scenario}
            COMMAND willpower_application_resource_definition_tests ${scenario})
    endforeach()

    add_executable(willpower_application_input_state_manager_tests
        tests/input_state_manager_tests.cpp)
    target_compile_features(willpower_application_input_state_manager_tests PRIVATE cxx_std_20)
    target_link_libraries(willpower_application_input_state_manager_tests PRIVATE Willpower.Application)
    willpower_target_defaults(willpower_application_input_state_manager_tests)
    willpower_output_dirs(willpower_application_input_state_manager_tests)
    set_target_properties(willpower_application_input_state_manager_tests PROPERTIES FOLDER Willpower)
    willpower_deploy_runtime_dlls(willpower_application_input_state_manager_tests)
    willpower_deploy_vendor_dlls(willpower_application_input_state_manager_tests)
    willpower_add_test(NAME willpower_application_input_state_manager
        COMMAND willpower_application_input_state_manager_tests)

    add_executable(willpower_application_scheduler_tests
        tests/scheduler_tests.cpp)
    target_compile_features(willpower_application_scheduler_tests PRIVATE cxx_std_20)
    target_link_libraries(willpower_application_scheduler_tests PRIVATE Willpower.Application)
    willpower_target_defaults(willpower_application_scheduler_tests)
    willpower_output_dirs(willpower_application_scheduler_tests)
    set_target_properties(willpower_application_scheduler_tests PROPERTIES FOLDER Willpower)
    willpower_deploy_runtime_dlls(willpower_application_scheduler_tests)
    willpower_deploy_vendor_dlls(willpower_application_scheduler_tests)
    willpower_add_test(NAME willpower_application_scheduler
        COMMAND willpower_application_scheduler_tests)

    add_executable(willpower_application_resource_dependency_tests
        tests/resource_dependency_tests.cpp)
    target_compile_features(willpower_application_resource_dependency_tests PRIVATE cxx_std_20)
    target_link_libraries(willpower_application_resource_dependency_tests PRIVATE Willpower.Application)
    willpower_target_defaults(willpower_application_resource_dependency_tests)
    willpower_output_dirs(willpower_application_resource_dependency_tests)
    set_target_properties(willpower_application_resource_dependency_tests PROPERTIES FOLDER Willpower)
    willpower_deploy_runtime_dlls(willpower_application_resource_dependency_tests)
    willpower_deploy_vendor_dlls(willpower_application_resource_dependency_tests)
    foreach(scenario cycle missing-namespace missing-resource cross-location)
        willpower_add_test(NAME willpower_application_resource_dependency_${scenario}
            COMMAND willpower_application_resource_dependency_tests ${scenario})
    endforeach()

    add_executable(willpower_application_resource_release_tests
        tests/resource_release_tests.cpp)
    target_compile_features(willpower_application_resource_release_tests PRIVATE cxx_std_20)
    target_link_libraries(willpower_application_resource_release_tests PRIVATE Willpower.Application)
    willpower_target_defaults(willpower_application_resource_release_tests)
    willpower_output_dirs(willpower_application_resource_release_tests)
    set_target_properties(willpower_application_resource_release_tests PROPERTIES FOLDER Willpower)
    willpower_deploy_runtime_dlls(willpower_application_resource_release_tests)
    willpower_deploy_vendor_dlls(willpower_application_resource_release_tests)
    willpower_add_test(NAME willpower_application_resource_release
        COMMAND willpower_application_resource_release_tests)
endif()
