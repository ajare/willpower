include_guard(GLOBAL)

function(willpower_add_header_filter target)
    if(ARGC EQUAL 1)
        set(header_dirs "${CMAKE_CURRENT_SOURCE_DIR}/include")
    else()
        set(header_dirs ${ARGN})
    endif()

    foreach(header_dir IN LISTS header_dirs)
        if(IS_DIRECTORY "${header_dir}")
            file(GLOB_RECURSE headers CONFIGURE_DEPENDS
                "${header_dir}/*.h" "${header_dir}/*.hpp" "${header_dir}/*.inl")
            if(headers)
                target_sources(${target} PRIVATE ${headers})
                source_group(TREE "${header_dir}" PREFIX "Header Files" FILES ${headers})
            endif()
        endif()
    endforeach()
endfunction()

function(willpower_target_defaults target)
    set_target_properties(${target} PROPERTIES
        DEBUG_POSTFIX "d")
    if(MSVC)
        set_target_properties(${target} PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<OR:$<CONFIG:Debug>,$<CONFIG:MemCheck>>:Debug>DLL"
            # CMake's default DebugInformationFormat only recognises the literal
            # "Debug" config (among the ones Willpower uses); MemCheck needs it
            # set explicitly too or ASan builds emit no PDB (MSVC warning C5072)
            # and lose symbolised reports.
            MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,MemCheck>:ProgramDatabase>")
        target_compile_options(${target} PRIVATE
            /MP /W4
            $<$<CONFIG:MemCheck>:/fsanitize=address>)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
    target_compile_definitions(${target} PRIVATE
        $<$<OR:$<CONFIG:Debug>,$<CONFIG:MemCheck>>:_DEBUG>
        $<$<NOT:$<OR:$<CONFIG:Debug>,$<CONFIG:MemCheck>>>:NDEBUG>)
endfunction()

function(willpower_output_dirs target)
    set(runtime "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/${target}")
    set(archive "${CMAKE_BINARY_DIR}/lib/$<CONFIG>/${target}")
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${runtime}"
        LIBRARY_OUTPUT_DIRECTORY "${runtime}"
        ARCHIVE_OUTPUT_DIRECTORY "${archive}")
    # PDBs are an MSVC concept; on GCC/Clang debug info lives in the objects.
    if(MSVC)
        set_target_properties(${target} PROPERTIES
            PDB_OUTPUT_DIRECTORY "${runtime}")
    endif()
endfunction()

function(willpower_enable_sdl_checks)
    # /sdl is an MSVC-only flag; no-op elsewhere.
    if(NOT MSVC)
        return()
    endif()
    foreach(target ${ARGN})
        target_compile_options(${target} PRIVATE /sdl)
    endforeach()
endfunction()

function(willpower_deploy_runtime_dlls target)
    # $<TARGET_RUNTIME_DLLS> is MSVC-only; off MSVC the build-tree RPATH
    # (CMake default) already lets executables find the build-tree .so files.
    if(MSVC)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_RUNTIME_DLLS:${target}>" "$<TARGET_FILE_DIR:${target}>"
            COMMAND_EXPAND_LISTS VERBATIM
            COMMENT "Staging runtime DLLs for ${target}")
    endif()
    willpower_copy_asan_runtime(${target})
endfunction()

# Copies the MSVC ASan runtime DLL/PDB next to a MemCheck config target so it
# can run standalone outside a Visual Studio developer command prompt. A
# no-op for every other configuration and generator; WILLPOWER_ASAN_DLL is
# only defined for MSVC multi-config generators (see the MemCheck setup near
# the top of the top-level CMakeLists.txt).
function(willpower_copy_asan_runtime target)
    if(NOT DEFINED WILLPOWER_ASAN_DLL)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "$<$<CONFIG:MemCheck>:${CMAKE_COMMAND};-E;copy_if_different;${WILLPOWER_ASAN_DLL};${WILLPOWER_ASAN_PDB};$<TARGET_FILE_DIR:${target}>>"
        COMMAND_EXPAND_LISTS
        VERBATIM)
endfunction()

function(willpower_deploy_vendor_dlls target)
    if(NOT WILLPOWER_ENABLE_FMOD)
        return()
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:vendor::fmod>"
                "$<TARGET_FILE:vendor::fmodstudio>"
                "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS VERBATIM
        COMMENT "Staging FMOD runtime DLLs for ${target}")
endfunction()
