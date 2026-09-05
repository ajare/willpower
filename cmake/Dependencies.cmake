include_guard(GLOBAL)
include(ExternalProject)

set(WILLPOWER_EXT_DIR "${PROJECT_SOURCE_DIR}/ext")
set(WILLPOWER_MPP_SOURCE_DIR "${WILLPOWER_EXT_DIR}/massive-poly-pusher")
set(WILLPOWER_MPP_BUILD_DIR "${CMAKE_BINARY_DIR}/_deps/massive-poly-pusher-build")
# MPP deliberately places artifacts under its source checkout's build tree,
# independently of the CMake binary directory used to configure it.
set(WILLPOWER_MPP_OUTPUT_DIR "${WILLPOWER_MPP_SOURCE_DIR}/build")

foreach(required_path
        "${WILLPOWER_MPP_SOURCE_DIR}/CMakeLists.txt"
        "${WILLPOWER_MPP_SOURCE_DIR}/ext/utils/CMakeLists.txt"
        "${WILLPOWER_MPP_SOURCE_DIR}/ext/sdl/CMakeLists.txt"
        "${WILLPOWER_MPP_SOURCE_DIR}/ext/assimp/CMakeLists.txt"
        "${WILLPOWER_MPP_SOURCE_DIR}/ext/glew/.git"
        "${WILLPOWER_EXT_DIR}/earcut.hpp/include/mapbox/earcut.hpp"
        "${WILLPOWER_EXT_DIR}/SplineLibrary/spline_library/spline.h"
        "${WILLPOWER_MPP_SOURCE_DIR}/ext/assimp/contrib/poly2tri/poly2tri/poly2tri.h")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR
            "A required dependency is missing (${required_path}).\n"
            "Run: git submodule update --init --recursive")
    endif()
endforeach()

# MassivePolyPusher is kept in an independent build tree because its project
# defines applications and global output settings in addition to its libraries.
# Building the ExternalProject target produces only the libraries Willpower uses.
ExternalProject_Add(willpower_mpp_external
    SOURCE_DIR "${WILLPOWER_MPP_SOURCE_DIR}"
    BINARY_DIR "${WILLPOWER_MPP_BUILD_DIR}"
    CMAKE_ARGS
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
        "-DCMAKE_BUILD_TYPE=$<CONFIG>"
    BUILD_COMMAND
        "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG> --parallel
        --target MassivePolyPusher MppMesh MppHelper MppProgram MppData Utils glew
    # MPP is consumed directly from its build tree; make the no-op explicit
    # rather than having ExternalProject print a misleading "No install step".
    INSTALL_COMMAND "${CMAKE_COMMAND}" -E true
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE)
set_target_properties(willpower_mpp_external PROPERTIES FOLDER Dependencies)

set(_mpp_lib "${WILLPOWER_MPP_OUTPUT_DIR}/lib")
set(_mpp_bin "${WILLPOWER_MPP_OUTPUT_DIR}/bin")
set(_mpp_glew_include "${WILLPOWER_MPP_BUILD_DIR}/_deps/glew-2.3.1/include")
# Imported include directories must exist while CMake generates the build.
file(MAKE_DIRECTORY "${_mpp_glew_include}")

function(willpower_import_mpp target stem)
    cmake_parse_arguments(ARG "" "LINUX_STEM" "INCLUDE" ${ARGN})
    add_library(${target} SHARED IMPORTED GLOBAL)
    if(WIN32)
        set_target_properties(${target} PROPERTIES
            IMPORTED_CONFIGURATIONS "Debug;Release;MemCheck"
            IMPORTED_IMPLIB_RELEASE "${_mpp_lib}/Release/${stem}.lib"
            IMPORTED_IMPLIB_DEBUG "${_mpp_lib}/Debug/${stem}d.lib"
            # MemCheck is a Debug variant, but DEBUG_POSTFIX only applies to a
            # config literally named "Debug" (see massive-poly-pusher and utils
            # CMakeLists.txt), so its artifacts keep the bare stem name.
            IMPORTED_IMPLIB_MEMCHECK "${_mpp_lib}/MemCheck/${stem}.lib"
            IMPORTED_LOCATION_RELEASE "${_mpp_bin}/Release/${stem}.dll"
            IMPORTED_LOCATION_DEBUG "${_mpp_bin}/Debug/${stem}d.dll"
            IMPORTED_LOCATION_MEMCHECK "${_mpp_bin}/MemCheck/${stem}.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${ARG_INCLUDE}")
    else()
        # Linux is single-config and MPP's CMAKE_LIBRARY_OUTPUT_DIRECTORY puts
        # every shared library in the flat bin/ directory (verified against a
        # real MPP build). The external build passes no CMAKE_BUILD_TYPE, so
        # artifacts carry no debug postfix: bin/<stem>.so.
        #
        # LINUX_STEM overrides the artifact stem where it differs from the
        # Windows one (GLEW builds as libGLEW.so, not libglew32.so).
        set(_mpp_stem "${stem}")
        if(ARG_LINUX_STEM)
            set(_mpp_stem "${ARG_LINUX_STEM}")
        endif()
        if(NOT CMAKE_BUILD_TYPE)
            message(FATAL_ERROR "Linux builds require CMAKE_BUILD_TYPE (for example, Release or Debug).")
        endif()
        set_target_properties(${target} PROPERTIES
            IMPORTED_LOCATION "${_mpp_bin}/${CMAKE_BUILD_TYPE}/lib${_mpp_stem}.so"
            INTERFACE_INCLUDE_DIRECTORIES "${ARG_INCLUDE}")
    endif()
    add_dependencies(${target} willpower_mpp_external)
endfunction()

willpower_import_mpp(ext::Utils Utils
    INCLUDE "${WILLPOWER_MPP_SOURCE_DIR}/ext/utils/include")
willpower_import_mpp(ext::mpp MassivePolyPusher
    INCLUDE "${WILLPOWER_MPP_SOURCE_DIR}/mpp/include;${WILLPOWER_MPP_SOURCE_DIR}/vendor/include;${_mpp_glew_include}")
willpower_import_mpp(ext::mpp-mesh MppMesh
    INCLUDE "${WILLPOWER_MPP_SOURCE_DIR}/mpp-mesh/include")
willpower_import_mpp(ext::mpp-helper MppHelper
    INCLUDE "${WILLPOWER_MPP_SOURCE_DIR}/mpp-helper/include")
willpower_import_mpp(ext::mpp-program MppProgram
    INCLUDE "${WILLPOWER_MPP_SOURCE_DIR}/mpp-program/include")
willpower_import_mpp(ext::mpp-data MppData
    INCLUDE "${WILLPOWER_MPP_SOURCE_DIR}/mpp-data/include")
willpower_import_mpp(ext::glew glew32
    LINUX_STEM GLEW
    INCLUDE "${_mpp_glew_include}")
set_property(TARGET ext::glew APPEND PROPERTY
    INTERFACE_COMPILE_DEFINITIONS GLEW_NO_GLU)

# Runtime dependencies which cannot be inferred from an imported DLL alone.
set_property(TARGET ext::mpp APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES "ext::mpp-data;ext::glew")

set(_poly2tri_dir "${WILLPOWER_MPP_SOURCE_DIR}/ext/assimp/contrib/poly2tri")
add_library(vendor_poly2tri STATIC
    "${_poly2tri_dir}/poly2tri/common/shapes.cc"
    "${_poly2tri_dir}/poly2tri/sweep/advancing_front.cc"
    "${_poly2tri_dir}/poly2tri/sweep/cdt.cc"
    "${_poly2tri_dir}/poly2tri/sweep/sweep.cc"
    "${_poly2tri_dir}/poly2tri/sweep/sweep_context.cc")
add_library(vendor::poly2tri ALIAS vendor_poly2tri)
target_include_directories(vendor_poly2tri PUBLIC "${_poly2tri_dir}")
target_compile_definitions(vendor_poly2tri PUBLIC P2T_STATIC_EXPORTS)
target_compile_features(vendor_poly2tri PUBLIC cxx_std_20)
set_target_properties(vendor_poly2tri PROPERTIES
    FOLDER Dependencies
    POSITION_INDEPENDENT_CODE ON)
unset(_poly2tri_dir)

add_library(vendor_headers INTERFACE)
add_library(vendor::headers ALIAS vendor_headers)
target_include_directories(vendor_headers INTERFACE
    "${WILLPOWER_EXT_DIR}/earcut.hpp/include"
    "${WILLPOWER_EXT_DIR}/SplineLibrary")

if(WILLPOWER_ENABLE_FMOD)
    # FMOD also ships Linux and macOS SDKs; a future per-platform port would
    # extend the find_* paths below (api/core/lib/linux, api/studio/lib/linux,
    # ...). Until then the no-op audio default already covers non-Windows,
    # so keep the explicit error.
    if(NOT WIN32)
        message(FATAL_ERROR "WILLPOWER_ENABLE_FMOD is supported on Windows only.")
    endif()
    if(NOT WILLPOWER_FMOD_ROOT)
        message(FATAL_ERROR
            "WILLPOWER_ENABLE_FMOD requires -DWILLPOWER_FMOD_ROOT=<FMOD Studio API directory>.")
    endif()

    find_path(WILLPOWER_FMOD_CORE_INCLUDE fmod.hpp
        PATHS "${WILLPOWER_FMOD_ROOT}/api/core/inc" NO_DEFAULT_PATH REQUIRED)
    find_path(WILLPOWER_FMOD_STUDIO_INCLUDE fmod_studio.hpp
        PATHS "${WILLPOWER_FMOD_ROOT}/api/studio/inc" NO_DEFAULT_PATH REQUIRED)
    find_library(WILLPOWER_FMOD_CORE_LIBRARY NAMES fmod_vc fmodL_vc
        PATHS "${WILLPOWER_FMOD_ROOT}/api/core/lib/x64" NO_DEFAULT_PATH REQUIRED)
    find_library(WILLPOWER_FMOD_STUDIO_LIBRARY NAMES fmodstudio_vc fmodstudioL_vc
        PATHS "${WILLPOWER_FMOD_ROOT}/api/studio/lib/x64" NO_DEFAULT_PATH REQUIRED)
    find_file(WILLPOWER_FMOD_CORE_DLL NAMES fmod.dll fmodL.dll
        PATHS "${WILLPOWER_FMOD_ROOT}/api/core/lib/x64" NO_DEFAULT_PATH REQUIRED)
    find_file(WILLPOWER_FMOD_STUDIO_DLL NAMES fmodstudio.dll fmodstudioL.dll
        PATHS "${WILLPOWER_FMOD_ROOT}/api/studio/lib/x64" NO_DEFAULT_PATH REQUIRED)

    add_library(vendor::fmod SHARED IMPORTED GLOBAL)
    set_target_properties(vendor::fmod PROPERTIES
        IMPORTED_IMPLIB "${WILLPOWER_FMOD_CORE_LIBRARY}"
        IMPORTED_LOCATION "${WILLPOWER_FMOD_CORE_DLL}"
        INTERFACE_INCLUDE_DIRECTORIES "${WILLPOWER_FMOD_CORE_INCLUDE}")
    add_library(vendor::fmodstudio SHARED IMPORTED GLOBAL)
    set_target_properties(vendor::fmodstudio PROPERTIES
        IMPORTED_IMPLIB "${WILLPOWER_FMOD_STUDIO_LIBRARY}"
        IMPORTED_LOCATION "${WILLPOWER_FMOD_STUDIO_DLL}"
        INTERFACE_INCLUDE_DIRECTORIES "${WILLPOWER_FMOD_STUDIO_INCLUDE}")
endif()
