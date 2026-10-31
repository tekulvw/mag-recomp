# Native player UI on macOS, Windows and Linux. Opt-in is selected by the packaged supervisor at runtime.
# Keep the module independent of generated title sources so it can also be
# built and exercised against the synthetic input/window fixture.
function(mag_enable_player_settings target sdk)
    get_filename_component(player_src "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src" ABSOLUTE)
    if(WIN32)
        set(ZLIB_USE_STATIC_LIBS ON)
    endif()
    find_package(ZLIB REQUIRED)
    if(NOT APPLE)
        target_sources(${target} PRIVATE
            "${player_src}/player_settings_portable.cpp" "${player_src}/player_server_overlay_portable.cpp"
            "${player_src}/player_settings_portable_runtime.cpp" "${player_src}/player_archive.cpp")
        set_property(SOURCE "${player_src}/main.cpp" TARGET_DIRECTORY ${target}
            APPEND PROPERTY COMPILE_DEFINITIONS MAG_PLAYER_SETTINGS=1)
        set_source_files_properties("${player_src}/player_server_overlay_portable.cpp" TARGET_DIRECTORY ${target}
            PROPERTIES INCLUDE_DIRECTORIES "${sdk}/runtime/ppu")
        target_link_libraries(${target} PRIVATE ZLIB::ZLIB)
        if(WIN32)
            target_sources(${target} PRIVATE "${player_src}/player_settings_windows.cpp")
            target_link_libraries(${target} PRIVATE bcrypt ws2_32 user32 gdi32 xinput)
            # Include the real import declarations before aliasing calls. A
            # command-line name alias would incorrectly import our wrappers.
            if(MSVC)
                set(_player_input_include "/FI${player_src}/player_input_windows.h")
            else()
                set(_player_input_include "-include;${player_src}/player_input_windows.h")
            endif()
            set_property(SOURCE "${sdk}/libs/input/cellPad.c" DIRECTORY "${sdk}"
                APPEND PROPERTY COMPILE_OPTIONS ${_player_input_include})
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            find_package(OpenSSL REQUIRED COMPONENTS Crypto)
            find_package(SDL2 REQUIRED)
            find_package(Threads REQUIRED)
            file(STRINGS "${sdk}/libs/video/rsx_vulkan_backend.h" _player_fxaa
                REGEX "^#define[ \t]+PS3RECOMP_VULKAN_HAS_FXAA[ \t]+1")
            if(_player_fxaa)
                set_property(SOURCE "${player_src}/player_settings_linux.cpp"
                    "${player_src}/player_settings_portable_runtime.cpp" TARGET_DIRECTORY ${target}
                    APPEND PROPERTY COMPILE_DEFINITIONS MAG_PLAYER_VULKAN_FXAA=1)
            endif()
            target_sources(${target} PRIVATE "${player_src}/player_settings_linux.cpp" "${player_src}/player_render_scale.cpp" "${player_src}/player_renderer_linux.cpp")
            set_source_files_properties("${player_src}/player_render_scale.cpp" "${player_src}/player_renderer_linux.cpp" TARGET_DIRECTORY ${target}
                PROPERTIES INCLUDE_DIRECTORIES "${sdk}")
            set_property(SOURCE "${sdk}/libs/video/rsx_vulkan_backend.c" DIRECTORY "${sdk}"
                APPEND PROPERTY COMPILE_DEFINITIONS rsx_draw_engine_set_backend=mag_player_set_render_backend
                rsx_set_backend=mag_player_set_legacy_backend)
            target_link_libraries(${target} PRIVATE OpenSSL::Crypto SDL2::SDL2 Threads::Threads ${CMAKE_DL_LIBS})
            set_property(SOURCE "${sdk}/libs/video/rsx_vulkan_backend.c" DIRECTORY "${sdk}"
                APPEND PROPERTY COMPILE_DEFINITIONS SDL_CreateWindow=mag_player_create_window
                SDL_PollEvent=mag_player_poll_event)
            set_property(SOURCE "${sdk}/libs/input/cellPad.c" DIRECTORY "${sdk}"
                APPEND PROPERTY COMPILE_DEFINITIONS SDL_GameControllerGetButton=mag_player_controller_button
                SDL_GameControllerGetAxis=mag_player_controller_axis)
        endif()
        return()
    endif()
    target_sources(${target} PRIVATE
        "${player_src}/player_settings_store.m" "${player_src}/player_settings_macos.m"
        "${player_src}/player_settings_runtime.mm" "${player_src}/player_server_overlay.mm"
        "${player_src}/player_archive.cpp" "${player_src}/player_render_scale.cpp")
    set_property(SOURCE "${player_src}/main.cpp" TARGET_DIRECTORY ${target}
        APPEND PROPERTY COMPILE_DEFINITIONS MAG_PLAYER_SETTINGS=1)
    set_source_files_properties("${player_src}/player_settings_store.m" "${player_src}/player_settings_macos.m"
        "${player_src}/player_settings_runtime.mm" "${player_src}/player_server_overlay.mm"
        TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_source_files_properties("${player_src}/player_server_overlay.mm" TARGET_DIRECTORY ${target}
        PROPERTIES INCLUDE_DIRECTORIES "${sdk}/runtime/ppu")
    set_source_files_properties("${player_src}/player_render_scale.cpp" TARGET_DIRECTORY ${target}
        PROPERTIES INCLUDE_DIRECTORIES "${sdk}")
    set_property(TARGET ${target} PROPERTY OBJCXX_STANDARD 20)
    target_link_libraries(${target} PRIVATE ZLIB::ZLIB "-framework AppKit" "-framework QuartzCore")

    # Forward the SDK's existing input unchanged unless the panel owns input.
    target_sources(ps3recomp_runtime PRIVATE "${player_src}/player_input_macos.m")
    set_source_files_properties("${player_src}/player_input_macos.m" DIRECTORY "${sdk}"
        PROPERTIES INCLUDE_DIRECTORIES "${sdk}")
    set_property(SOURCE "${sdk}/libs/input/cellPad.c" DIRECTORY "${sdk}"
        APPEND PROPERTY COMPILE_DEFINITIONS SDL_GameControllerGetButton=mag_player_controller_button
        SDL_GameControllerGetAxis=mag_player_controller_axis)
    set_property(SOURCE "${sdk}/libs/input/pad_macos.m" DIRECTORY "${sdk}"
        APPEND PROPERTY COMPILE_DEFINITIONS ps3recomp_pad_macos_state=mag_sdk_pad_macos_state
        ps3recomp_pad_macos_buttons=mag_sdk_pad_macos_buttons)
    # Intercept only Metal's backend registration. The SDK draw engine and its
    # original backend remain intact, and native scale returns that exact backend.
    set_property(SOURCE "${sdk}/libs/video/rsx_metal_backend.m" DIRECTORY "${sdk}"
        APPEND PROPERTY COMPILE_DEFINITIONS rsx_draw_engine_set_backend=mag_player_set_render_backend)
endfunction()
