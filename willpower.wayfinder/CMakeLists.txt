add_library(Willpower.Wayfinder SHARED
    src/Agent.cpp
    src/AgentPath.cpp
    src/AgentSwarm.cpp
    src/AgentTarget.cpp
    src/ConvexPolygonisation.cpp
    src/FlockingAgent.cpp
    src/Floor.cpp
    src/FloorLine.cpp
    src/Mesh.cpp
    src/PathDatabase.cpp
    src/Sector.cpp)

target_include_directories(Willpower.Wayfinder PUBLIC include)
willpower_add_header_filter(Willpower.Wayfinder)

target_compile_definitions(Willpower.Wayfinder PRIVATE
    $<$<PLATFORM_ID:Windows>:WIN32>
    $<$<PLATFORM_ID:Windows>:_WINDOWS>
    $<$<PLATFORM_ID:Windows>:_USRDLL>
    WP_WAYFINDER_DLL_EXPORT
    WP_USE_ASSERT_TRACE
    $<$<CONFIG:Debug>:WP_USE_MEMLEAK_TRACKING>)

target_link_libraries(Willpower.Wayfinder
    PUBLIC  Willpower.Common Willpower.Geometry
    PRIVATE ext::Utils vendor::poly2tri)

target_compile_features(Willpower.Wayfinder PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(Willpower.Wayfinder PRIVATE /W4)
else()
    target_compile_options(Willpower.Wayfinder PRIVATE -Wall -Wextra)
endif()
willpower_target_defaults(Willpower.Wayfinder)
willpower_output_dirs(Willpower.Wayfinder)
set_target_properties(Willpower.Wayfinder PROPERTIES FOLDER Willpower)
