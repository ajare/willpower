cmake_minimum_required(VERSION 3.25)

project(Willpower VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Willpower currently supports Windows builds with Microsoft Visual C++ only.")
endif()

if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Willpower and MassivePolyPusher support x64 builds only. Configure with -A x64.")
endif()

if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
add_compile_definitions(UNICODE _UNICODE NOMINMAX)

option(WILLPOWER_ENABLE_FMOD "Enable FMOD-backed audio (requires a separately installed FMOD SDK)" OFF)
set(WILLPOWER_FMOD_ROOT "" CACHE PATH "Root of the FMOD Studio API installation")

include(CTest)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Helpers)
include(Dependencies)

# Listed in dependency order for readability; CMake resolves the actual order
# from target_link_libraries.
add_subdirectory(willpower.common)
add_subdirectory(willpower.geometry)
add_subdirectory(willpower.collide)
add_subdirectory(willpower.application)
add_subdirectory(willpower.viz)
