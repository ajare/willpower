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
        DEBUG_POSTFIX "d"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_compile_options(${target} PRIVATE /MP)
    target_compile_definitions(${target} PRIVATE
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
endfunction()

function(willpower_output_dirs target)
    set(runtime "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/${target}")
    set(archive "${CMAKE_BINARY_DIR}/lib/$<CONFIG>/${target}")
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${runtime}"
        LIBRARY_OUTPUT_DIRECTORY "${runtime}"
        ARCHIVE_OUTPUT_DIRECTORY "${archive}"
        PDB_OUTPUT_DIRECTORY "${runtime}")
endfunction()

function(willpower_enable_sdl_checks)
    foreach(target ${ARGN})
        target_compile_options(${target} PRIVATE /sdl)
    endforeach()
endfunction()

function(willpower_deploy_runtime_dlls target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_RUNTIME_DLLS:${target}>" "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS VERBATIM
        COMMENT "Staging runtime DLLs for ${target}")
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
