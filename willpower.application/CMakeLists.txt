set(WILLPOWER_EMBEDDED_SCHEMA_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(WILLPOWER_EMBEDDED_SCHEMA_HEADER
    "${WILLPOWER_EMBEDDED_SCHEMA_DIR}/EmbeddedResourceManifestSchemas.h")
set(WILLPOWER_EMBEDDED_SCHEMA_SOURCE
    "${WILLPOWER_EMBEDDED_SCHEMA_DIR}/EmbeddedResourceManifestSchemas.cpp")
set(WILLPOWER_SCHEMA_RECORDS "")
set(WILLPOWER_SCHEMA_INPUTS "")
# WP-026 and the built-in schema tickets append canonical schemas here. These
# focused fixtures exercise the catalog infrastructure without implementing
# those blocked schema definitions early.
if(BUILD_TESTING)
    list(APPEND WILLPOWER_SCHEMA_RECORDS
        "Infrastructure||https://schemas.willpower.test/infrastructure.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/tests/schemas/infrastructure.schema.json"
        "||https://schemas.willpower.test/infrastructure-types.schema.json|${CMAKE_CURRENT_SOURCE_DIR}/tests/schemas/infrastructure-types.schema.json")
    list(APPEND WILLPOWER_SCHEMA_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/schemas/infrastructure.schema.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/schemas/infrastructure-types.schema.json")
endif()
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
    PRIVATE "${WILLPOWER_EMBEDDED_SCHEMA_DIR}")
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

if(BUILD_TESTING)
    add_executable(willpower_application_schema_catalog_tests
        tests/schema_catalog_tests.cpp
        src/resourcesystem/ResourceManifestValidator.cpp
        "${WILLPOWER_EMBEDDED_SCHEMA_SOURCE}")
    target_compile_features(willpower_application_schema_catalog_tests PRIVATE cxx_std_20)
    target_include_directories(willpower_application_schema_catalog_tests PRIVATE
        src/resourcesystem "${WILLPOWER_EMBEDDED_SCHEMA_DIR}")
    target_link_libraries(willpower_application_schema_catalog_tests PRIVATE ext::Utils)
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
