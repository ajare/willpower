cmake_minimum_required(VERSION 3.25)

project(Willpower VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Willpower and MassivePolyPusher support x64 builds only. Configure with -A x64.")
endif()

if(MSVC AND CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release;MemCheck" CACHE STRING "" FORCE)

    # MemCheck is a Debug variant instrumented with MSVC AddressSanitizer,
    # matching the MemCheck configuration already built by the
    # massive-poly-pusher and utils submodules (see their CMakeLists.txt).
    # Base MemCheck flags mirror Debug; /fsanitize=address itself is applied
    # per-target in willpower_target_defaults() (cmake/Helpers.cmake), only
    # to Willpower's own targets.
    # /RTC1 (runtime checks) cannot be combined with /fsanitize=address under MSVC.
    string(REPLACE "/RTC1" "" WILLPOWER_MEMCHECK_CXX_FLAGS "${CMAKE_CXX_FLAGS_DEBUG}")
    set(CMAKE_CXX_FLAGS_MEMCHECK "${WILLPOWER_MEMCHECK_CXX_FLAGS}" CACHE STRING "Flags used by the C++ compiler for MemCheck builds" FORCE)
    unset(WILLPOWER_MEMCHECK_CXX_FLAGS)

    # ASan is incompatible with incremental linking.
    string(REPLACE "/INCREMENTAL" "" WILLPOWER_MEMCHECK_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS_DEBUG}")
    string(REPLACE "/INCREMENTAL" "" WILLPOWER_MEMCHECK_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS_DEBUG}")
    set(CMAKE_EXE_LINKER_FLAGS_MEMCHECK "${WILLPOWER_MEMCHECK_EXE_LINKER_FLAGS} /INCREMENTAL:NO" CACHE STRING "Flags used to link an executable for MemCheck builds" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS_MEMCHECK "${WILLPOWER_MEMCHECK_SHARED_LINKER_FLAGS} /INCREMENTAL:NO" CACHE STRING "Flags used by the shared library linker for MemCheck builds" FORCE)
    unset(WILLPOWER_MEMCHECK_EXE_LINKER_FLAGS)
    unset(WILLPOWER_MEMCHECK_SHARED_LINKER_FLAGS)
    mark_as_advanced(
        CMAKE_CXX_FLAGS_MEMCHECK
        CMAKE_EXE_LINKER_FLAGS_MEMCHECK
        CMAKE_SHARED_LINKER_FLAGS_MEMCHECK)

    # The MSVC ASan runtime isn't on PATH by default, so MemCheck executables
    # fail to start outside a Visual Studio developer command prompt unless
    # its DLL (and matching PDB, for symbolised reports) travel alongside
    # them; see willpower_copy_asan_runtime() in cmake/Helpers.cmake. It
    # lives next to link.exe.
    get_filename_component(WILLPOWER_ASAN_RUNTIME_DIR "${CMAKE_LINKER}" DIRECTORY)
    set(WILLPOWER_ASAN_DLL "${WILLPOWER_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.dll")
    set(WILLPOWER_ASAN_PDB "${WILLPOWER_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.pdb")
    if(NOT EXISTS "${WILLPOWER_ASAN_DLL}")
        message(WARNING "Could not find the MSVC ASan runtime DLL at ${WILLPOWER_ASAN_DLL}; MemCheck executables may fail to start.")
    endif()
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
add_compile_definitions(NOMINMAX)
if(WIN32)
    add_compile_definitions(UNICODE _UNICODE)
endif()

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
add_subdirectory(willpower.wayfinder)
add_subdirectory(willpower.collide)
add_subdirectory(willpower.application)
add_subdirectory(willpower.viz)
