add_library(Willpower.Geometry SHARED
    src/BezierOffsetter.cpp
    src/ConvexOffsetter.cpp
    src/CsgUtils.cpp
    src/DirectedEdgeLoop.cpp
    src/Edge.cpp
    src/EdgeFilter.cpp
    src/Filter.cpp
    src/Mesh.cpp
    src/MeshHelpers.cpp
    src/MeshOperations.cpp
    src/MeshQuery.cpp
    src/MeshUtils.cpp
    src/MeshValidator.cpp
    src/Offsetter.cpp
    src/Polygon.cpp
    src/PolygonFilter.cpp
    src/TypeConverters.cpp
    src/Vertex.cpp
    src/VertexFilter.cpp
    src/clipper.cpp)

target_include_directories(Willpower.Geometry PUBLIC include)
willpower_add_header_filter(Willpower.Geometry)

target_compile_definitions(Willpower.Geometry PRIVATE
    WIN32 _WINDOWS _USRDLL
    WP_GEOMETRY_DLL_EXPORT
    WP_USE_ASSERT_TRACE
    $<$<CONFIG:Debug>:WP_USE_MEMLEAK_TRACKING>)

# vendor::headers supplies mapbox (earcut).
target_link_libraries(Willpower.Geometry PUBLIC Willpower.Common vendor::headers)

target_compile_features(Willpower.Geometry PRIVATE cxx_std_20)
target_compile_options(Willpower.Geometry PRIVATE /W4)
willpower_target_defaults(Willpower.Geometry)
willpower_output_dirs(Willpower.Geometry)
set_target_properties(Willpower.Geometry PROPERTIES FOLDER Willpower)

if(BUILD_TESTING)
    add_executable(willpower_geometry_mesh_assignment_tests
        tests/mesh_assignment_tests.cpp)
    target_link_libraries(willpower_geometry_mesh_assignment_tests PRIVATE Willpower.Geometry)
    target_compile_features(willpower_geometry_mesh_assignment_tests PRIVATE cxx_std_20)
    target_compile_options(willpower_geometry_mesh_assignment_tests PRIVATE
        /fsanitize=address /Zi)
    target_link_options(willpower_geometry_mesh_assignment_tests PRIVATE
        /INFERASANLIBS /DEBUG)
    willpower_target_defaults(willpower_geometry_mesh_assignment_tests)
    willpower_output_dirs(willpower_geometry_mesh_assignment_tests)
    willpower_deploy_runtime_dlls(willpower_geometry_mesh_assignment_tests)
    get_filename_component(msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET willpower_geometry_mesh_assignment_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${msvc_bin_dir}/clang_rt.asan_dynamic-x86_64.dll"
                "$<TARGET_FILE_DIR:willpower_geometry_mesh_assignment_tests>")
    add_test(NAME willpower_geometry_mesh_assignment
             COMMAND willpower_geometry_mesh_assignment_tests)
endif()
