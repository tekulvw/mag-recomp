# macOS player UI. Opt-in is selected by the packaged supervisor at runtime.
# Keep the module independent of generated title sources so it can also be
# built and exercised against the synthetic input/window fixture.
function(mag_enable_player_settings target sdk)
    get_filename_component(player_src "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src" ABSOLUTE)
    find_package(ZLIB REQUIRED)
    set(_controls_source "${player_src}/player_controls.cpp")
    if(MAG_INPUT_RUST)
        target_sources(${target} PRIVATE "${player_src}/player_controls_settings.cpp" "${player_src}/player_controls_macos.mm")
        set_property(SOURCE "${player_src}/player_settings_runtime.mm" TARGET_DIRECTORY ${target} APPEND PROPERTY COMPILE_DEFINITIONS MAG_INPUT_RUST=1)
        set_source_files_properties("${player_src}/player_controls_macos.mm" TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        set(_controls_source "${player_src}/player_controls_rust.cpp")
        set_property(SOURCE "${player_src}/player_input_desktop_macos.mm" TARGET_DIRECTORY ${target}
            APPEND PROPERTY COMPILE_DEFINITIONS MAG_INPUT_RUST=1)
    endif()
    target_sources(${target} PRIVATE "${_controls_source}" "${player_src}/player_controls_store.cpp" "${player_src}/player_input_capture.cpp" "${player_src}/player_preferences_file.cpp")
    target_sources(${target} PRIVATE
        "${player_src}/player_settings_store.m" "${player_src}/player_settings_macos.m"
        "${player_src}/player_settings_runtime.mm" "${player_src}/player_server_overlay.mm" "${player_src}/player_input_desktop_macos.mm"
        "${player_src}/player_archive.cpp" "${player_src}/player_render_scale.cpp")
    set_property(SOURCE "${player_src}/main.cpp" TARGET_DIRECTORY ${target}
        APPEND PROPERTY COMPILE_DEFINITIONS MAG_PLAYER_SETTINGS=1)
    set_source_files_properties("${player_src}/player_settings_store.m" "${player_src}/player_settings_macos.m"
        "${player_src}/player_settings_runtime.mm" "${player_src}/player_server_overlay.mm" "${player_src}/player_input_desktop_macos.mm"
        TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_source_files_properties("${player_src}/player_server_overlay.mm" TARGET_DIRECTORY ${target}
        PROPERTIES INCLUDE_DIRECTORIES "${sdk}/runtime/ppu")
    set_source_files_properties("${player_src}/player_render_scale.cpp" TARGET_DIRECTORY ${target}
        PROPERTIES INCLUDE_DIRECTORIES "${sdk}")
    set_property(TARGET ${target} PROPERTY OBJCXX_STANDARD 20)
    target_link_libraries(${target} PRIVATE ZLIB::ZLIB "-framework AppKit" "-framework QuartzCore" "-framework GameController" "-framework CoreGraphics")

    # Forward the SDK's existing input unchanged unless the panel owns input.
    target_sources(ps3recomp_runtime PRIVATE "${player_src}/player_input_macos.m")
    set_source_files_properties("${player_src}/player_input_macos.m" DIRECTORY "${sdk}"
        PROPERTIES INCLUDE_DIRECTORIES "${sdk}")
    set_property(SOURCE "${sdk}/libs/input/cellPad.c" DIRECTORY "${sdk}"
        APPEND PROPERTY COMPILE_DEFINITIONS SDL_GameControllerGetButton=mag_player_controller_button
        SDL_GameControllerGetAxis=mag_player_controller_axis
        SDL_GameControllerUpdate=mag_player_controller_update SDL_GameControllerOpen=mag_player_controller_open
        SDL_GameControllerClose=mag_player_controller_close SDL_GameControllerGetAttached=mag_player_controller_attached)
    set_property(SOURCE "${sdk}/libs/input/pad_macos.m" DIRECTORY "${sdk}"
        APPEND PROPERTY COMPILE_DEFINITIONS ps3recomp_pad_macos_state=mag_sdk_pad_macos_state
        ps3recomp_pad_macos_buttons=mag_sdk_pad_macos_buttons)
    # Intercept only Metal's backend registration. The SDK draw engine and its
    # original backend remain intact, and native scale returns that exact backend.
    set_property(SOURCE "${sdk}/libs/video/rsx_metal_backend.m" DIRECTORY "${sdk}"
        APPEND PROPERTY COMPILE_DEFINITIONS rsx_draw_engine_set_backend=mag_player_set_render_backend)
endfunction()
