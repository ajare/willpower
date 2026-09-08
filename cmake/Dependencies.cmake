include_guard(GLOBAL)
include(ExternalProject)
include(FetchContent)

find_package(OpenGL REQUIRED)

# Fetch once in the top-level build, then hand the pinned header-only source to
# Utils' independent build. Utils has the same pinned fallback for standalone
# MassivePolyPusher builds.
FetchContent_Declare(willpower_valijson
    URL "https://github.com/tristanpenman/valijson/archive/refs/tags/v1.1.2.tar.gz"
    URL_HASH "SHA256=8e3cb09aead72f6f8653c966669cab52ff921ac52cc9d7498cd9387a35acce93"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(willpower_valijson)
if(NOT willpower_valijson_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(willpower_valijson)
    if(POLICY CMP0169)
        cmake_policy(POP)
    endif()
endif()

# ResourceSchemaCatalog parses untrusted bundle metadata privately. Pin the
# same header-only JSON implementation used by Utils without exposing it from
# Willpower.Application's public include directories.
FetchContent_Declare(willpower_nlohmann_json
    URL "https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz"
    URL_HASH "SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(willpower_nlohmann_json)
if(NOT willpower_nlohmann_json_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(willpower_nlohmann_json)
    if(POLICY CMP0169)
        cmake_policy(POP)
    endif()
endif()
set(WILLPOWER_NLOHMANN_JSON_INCLUDE_DIR
    "${willpower_nlohmann_json_SOURCE_DIR}/single_include")

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
        "-DUTILS_VALIJSON_SOURCE_DIR=${willpower_valijson_SOURCE_DIR}"
        "-DFETCHCONTENT_SOURCE_DIR_UTILS_NLOHMANN_JSON=${willpower_nlohmann_json_SOURCE_DIR}"
    BUILD_COMMAND
        "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG> --parallel
        --target MassivePolyPusher MppMesh MppHelper MppProgram MppData Utils glew
                 MppAppSupport ImGui SDL3-shared
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
            IMPORTED_CONFIGURATIONS "Debug;Release;Shipping;MemCheck"
            MAP_IMPORTED_CONFIG_SHIPPING "Shipping;Release"
            IMPORTED_IMPLIB_RELEASE "${_mpp_lib}/Release/${stem}.lib"
            IMPORTED_IMPLIB_SHIPPING "${_mpp_lib}/Shipping/${stem}.lib"
            IMPORTED_IMPLIB_DEBUG "${_mpp_lib}/Debug/${stem}d.lib"
            # MemCheck is a Debug variant, but DEBUG_POSTFIX only applies to a
            # config literally named "Debug" (see massive-poly-pusher and utils
            # CMakeLists.txt), so its artifacts keep the bare stem name.
            IMPORTED_IMPLIB_MEMCHECK "${_mpp_lib}/MemCheck/${stem}.lib"
            IMPORTED_LOCATION_RELEASE "${_mpp_bin}/Release/${stem}.dll"
            IMPORTED_LOCATION_SHIPPING "${_mpp_bin}/Shipping/${stem}.dll"
            IMPORTED_LOCATION_DEBUG "${_mpp_bin}/Debug/${stem}d.dll"
            IMPORTED_LOCATION_MEMCHECK "${_mpp_bin}/MemCheck/${stem}.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${ARG_INCLUDE}")
    else()
        # Linux is single-config and MPP's output layout includes the selected
        # build type (verified against a real MPP build). The root project
        # defaults an otherwise unspecified CMAKE_BUILD_TYPE to Release.
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

function(willpower_import_mpp_static target stem)
    cmake_parse_arguments(ARG "" "" "INCLUDE" ${ARGN})
    add_library(${target} STATIC IMPORTED GLOBAL)
    if(WIN32)
        set_target_properties(${target} PROPERTIES
            IMPORTED_CONFIGURATIONS "Debug;Release;Shipping;MemCheck"
            IMPORTED_LOCATION_RELEASE "${_mpp_lib}/Release/${stem}.lib"
            IMPORTED_LOCATION_SHIPPING "${_mpp_lib}/Shipping/${stem}.lib"
            IMPORTED_LOCATION_DEBUG "${_mpp_lib}/Debug/${stem}d.lib"
            IMPORTED_LOCATION_MEMCHECK "${_mpp_lib}/MemCheck/${stem}.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${ARG_INCLUDE}")
    else()
        set_target_properties(${target} PROPERTIES
            IMPORTED_LOCATION "${_mpp_lib}/${CMAKE_BUILD_TYPE}/lib${stem}.a"
            INTERFACE_INCLUDE_DIRECTORIES "${ARG_INCLUDE}")
    endif()
    add_dependencies(${target} willpower_mpp_external)
endfunction()

willpower_import_mpp_static(ext::imgui ImGui
    INCLUDE "${WILLPOWER_MPP_SOURCE_DIR}/ext/imgui/include;${WILLPOWER_MPP_SOURCE_DIR}/ext/imgui/include/imgui")
willpower_import_mpp_static(ext::mpp-app-support MppAppSupport
    INCLUDE "${WILLPOWER_MPP_SOURCE_DIR}/mpp-app-support/include")
# Resource Manifest Editor keeps yaml-cpp private to its implementation. It is
# built by Utils in MPP's dependency build and no YAML type crosses a public ABI.
willpower_import_mpp_static(ext::yaml-cpp yaml-cpp
    INCLUDE "${WILLPOWER_MPP_SOURCE_DIR}/ext/utils/vendor/yaml-cpp/include")
set_property(TARGET ext::yaml-cpp APPEND PROPERTY
    INTERFACE_COMPILE_DEFINITIONS YAML_CPP_STATIC_DEFINE)

willpower_import_mpp(ext::sdl SDL3
    INCLUDE "${WILLPOWER_MPP_SOURCE_DIR}/ext/sdl/include")
willpower_import_mpp(ext::glew glew32
    LINUX_STEM GLEW
    INCLUDE "${_mpp_glew_include}")
set_property(TARGET ext::glew APPEND PROPERTY
    INTERFACE_COMPILE_DEFINITIONS GLEW_NO_GLU)

# Runtime dependencies which cannot be inferred from an imported DLL alone.
set_property(TARGET ext::mpp APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES "ext::mpp-data;ext::glew")
set_property(TARGET ext::mpp-app-support APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES
        "ext::mpp;ext::mpp-mesh;ext::mpp-program;ext::Utils;ext::imgui;ext::sdl;ext::glew;${CMAKE_DL_LIBS}")
if(WIN32)
    set_property(TARGET ext::mpp-app-support APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "comdlg32;shell32;ole32")
endif()

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
    # FMOD is located by explicit paths rather than by a root directory. The
    # official SDK layout is only one shape the Engine API appears in, so a
    # project-vendored SDK can be used without recreating that layout.
    set(WILLPOWER_FMOD_CORE_INCLUDE "" CACHE PATH
        "Directory containing fmod.hpp")
    set(WILLPOWER_FMOD_STUDIO_INCLUDE "" CACHE PATH
        "Directory containing fmod_studio.hpp")
    set(WILLPOWER_FMOD_CORE_LIBRARY "" CACHE FILEPATH
        "FMOD core link library (fmod_vc.lib on Windows or libfmod.so on Linux)")
    set(WILLPOWER_FMOD_STUDIO_LIBRARY "" CACHE FILEPATH
        "FMOD Studio link library (fmodstudio_vc.lib on Windows or libfmodstudio.so on Linux)")

    set(_fmod_required_paths
        WILLPOWER_FMOD_CORE_INCLUDE
        WILLPOWER_FMOD_STUDIO_INCLUDE
        WILLPOWER_FMOD_CORE_LIBRARY
        WILLPOWER_FMOD_STUDIO_LIBRARY)
    if(WIN32)
        # Windows has separate import libraries and runtime DLLs. On Linux the
        # shared object supplied as *_LIBRARY serves both purposes.
        set(WILLPOWER_FMOD_CORE_DLL "" CACHE FILEPATH
            "FMOD core runtime DLL (fmod.dll)")
        set(WILLPOWER_FMOD_STUDIO_DLL "" CACHE FILEPATH
            "FMOD Studio runtime DLL (fmodstudio.dll)")
        list(APPEND _fmod_required_paths
            WILLPOWER_FMOD_CORE_DLL
            WILLPOWER_FMOD_STUDIO_DLL)
    endif()

    set(_fmod_problems "")
    foreach(_fmod_var IN LISTS _fmod_required_paths)
        if(NOT ${_fmod_var})
            string(APPEND _fmod_problems "  ${_fmod_var} is not set\n")
        elseif(NOT EXISTS "${${_fmod_var}}")
            string(APPEND _fmod_problems
                "  ${_fmod_var} does not exist: ${${_fmod_var}}\n")
        endif()
    endforeach()
    if(_fmod_problems)
        string(CONCAT _fmod_example
            "  -DWILLPOWER_FMOD_CORE_INCLUDE=<sdk>/api/core/inc\n"
            "  -DWILLPOWER_FMOD_STUDIO_INCLUDE=<sdk>/api/studio/inc\n")
        if(WIN32)
            string(APPEND _fmod_example
                "  -DWILLPOWER_FMOD_CORE_LIBRARY=<sdk>/api/core/lib/x64/fmod_vc.lib\n"
                "  -DWILLPOWER_FMOD_STUDIO_LIBRARY=<sdk>/api/studio/lib/x64/fmodstudio_vc.lib\n"
                "  -DWILLPOWER_FMOD_CORE_DLL=<sdk>/api/core/lib/x64/fmod.dll\n"
                "  -DWILLPOWER_FMOD_STUDIO_DLL=<sdk>/api/studio/lib/x64/fmodstudio.dll")
        else()
            string(APPEND _fmod_example
                "  -DWILLPOWER_FMOD_CORE_LIBRARY=<sdk>/api/core/lib/x86_64/libfmod.so\n"
                "  -DWILLPOWER_FMOD_STUDIO_LIBRARY=<sdk>/api/studio/lib/x86_64/libfmodstudio.so")
        endif()
        message(FATAL_ERROR
            "WILLPOWER_ENABLE_FMOD needs the FMOD Engine API located explicitly:\n"
            "${_fmod_problems}"
            "Point each variable at your FMOD tree, for example an official SDK install:\n"
            "${_fmod_example}")
    endif()
    unset(_fmod_example)
    unset(_fmod_problems)
    unset(_fmod_required_paths)

    add_library(vendor::fmod SHARED IMPORTED GLOBAL)
    add_library(vendor::fmodstudio SHARED IMPORTED GLOBAL)
    if(WIN32)
        set_target_properties(vendor::fmod PROPERTIES
            IMPORTED_CONFIGURATIONS "Debug;Release;Shipping;MemCheck"
            IMPORTED_IMPLIB "${WILLPOWER_FMOD_CORE_LIBRARY}"
            IMPORTED_LOCATION "${WILLPOWER_FMOD_CORE_DLL}"
            MAP_IMPORTED_CONFIG_DEBUG Release
            MAP_IMPORTED_CONFIG_SHIPPING Release
            MAP_IMPORTED_CONFIG_MEMCHECK Release
            IMPORTED_IMPLIB_RELEASE "${WILLPOWER_FMOD_CORE_LIBRARY}"
            IMPORTED_LOCATION_RELEASE "${WILLPOWER_FMOD_CORE_DLL}")
        set_target_properties(vendor::fmodstudio PROPERTIES
            IMPORTED_CONFIGURATIONS "Debug;Release;Shipping;MemCheck"
            IMPORTED_IMPLIB "${WILLPOWER_FMOD_STUDIO_LIBRARY}"
            IMPORTED_LOCATION "${WILLPOWER_FMOD_STUDIO_DLL}"
            MAP_IMPORTED_CONFIG_DEBUG Release
            MAP_IMPORTED_CONFIG_SHIPPING Release
            MAP_IMPORTED_CONFIG_MEMCHECK Release
            IMPORTED_IMPLIB_RELEASE "${WILLPOWER_FMOD_STUDIO_LIBRARY}"
            IMPORTED_LOCATION_RELEASE "${WILLPOWER_FMOD_STUDIO_DLL}")
    else()
        set_target_properties(vendor::fmod PROPERTIES
            IMPORTED_LOCATION "${WILLPOWER_FMOD_CORE_LIBRARY}")
        set_target_properties(vendor::fmodstudio PROPERTIES
            IMPORTED_LOCATION "${WILLPOWER_FMOD_STUDIO_LIBRARY}")
    endif()
    set_target_properties(vendor::fmod PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${WILLPOWER_FMOD_CORE_INCLUDE}")
    set_target_properties(vendor::fmodstudio PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${WILLPOWER_FMOD_STUDIO_INCLUDE}")
endif()
