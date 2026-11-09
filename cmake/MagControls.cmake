# Opt-in game controls adapter (docs/native-controls.md): keyboard and mouse play through the
# portable input engine, connected to the engine image by the `controls` lift hook stage.
# With MAG_GAME_CONTROLS off (the default) this file defines the option only: no file is read, no
# source, definition or library is added and receipts are unchanged.
option(MAG_GAME_CONTROLS "Build the game controls adapter into the mage_g engine" OFF)

# Adds the adapter to `target` and returns its receipt entries in MAG_GAME_CONTROLS_RECEIPT_EXTRA.
function(mag_enable_game_controls target)
    set(MAG_GAME_CONTROLS_RECEIPT_EXTRA "" PARENT_SCOPE)
    if(NOT MAG_GAME_CONTROLS)
        return()
    endif()
    if(NOT MAG_IMAGE STREQUAL "mage_g")
        # The launcher image has no player input; the same configure flags build both images.
        message(STATUS "MAG_GAME_CONTROLS: not used by the ${MAG_IMAGE} image")
        return()
    endif()
    if(NOT MAG_INPUT_RUST)
        message(FATAL_ERROR "MAG_GAME_CONTROLS requires MAG_INPUT_RUST=ON (the portable input engine)")
    endif()
    get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(_src "${_root}/src")
    set(_controls "${_src}/game_controls")

    # The lift must carry the controls hook stage, prepared by the current hook tools.
    set(_prepare "tools/render_hooks/prepare.py --base <lift> --output <new directory> --stage controls")
    set(_record "{}")
    if(EXISTS "${RECOMP_DIR}/provenance.json")
        file(READ "${RECOMP_DIR}/provenance.json" _record)
    endif()
    string(JSON _hooks ERROR_VARIABLE _error GET "${_record}" render_adapter_hooks)
    if(_error)
        message(FATAL_ERROR "MAG_GAME_CONTROLS needs a lift prepared with the controls hooks; run\n  ${_prepare}\nand point RECOMP_DIR at the output.")
    endif()
    string(JSON _schema ERROR_VARIABLE _error GET "${_hooks}" schema_version)
    if(_error OR NOT _schema EQUAL 1)
        message(FATAL_ERROR "RECOMP_DIR hook record has an unsupported schema; prepare the lift again")
    endif()
    set(_found FALSE)
    string(JSON _count LENGTH "${_hooks}" stages)
    if(_count GREATER 0)
        math(EXPR _last "${_count} - 1")
        foreach(_index RANGE ${_last})
            string(JSON _stage GET "${_hooks}" stages ${_index})
            if(_stage STREQUAL "controls")
                set(_found TRUE)
            endif()
        endforeach()
    endif()
    if(NOT _found)
        message(FATAL_ERROR "RECOMP_DIR was prepared without the controls hook stage; run\n  ${_prepare}")
    endif()
    file(GLOB _tools RELATIVE "${_root}" CONFIGURE_DEPENDS "${_root}/tools/render_hooks/*.py")
    foreach(_tool IN LISTS _tools)
        string(JSON _expected ERROR_VARIABLE _error GET "${_hooks}" tools "${_tool}")
        file(SHA256 "${_root}/${_tool}" _actual)
        if(_error OR NOT _actual STREQUAL _expected)
            message(FATAL_ERROR "RECOMP_DIR was prepared by a different ${_tool}; prepare it again")
        endif()
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RECOMP_DIR}/provenance.json")

    # Only the rewritten lifted files see the hook header and guard.
    string(JSON _count LENGTH "${_hooks}" changed_outputs)
    set(_changed "")
    if(_count GREATER 0)
        math(EXPR _last "${_count} - 1")
        foreach(_index RANGE ${_last})
            string(JSON _name GET "${_hooks}" changed_outputs ${_index})
            list(APPEND _changed "${RECOMP_DIR}/${_name}")
        endforeach()
    endif()
    if(_changed)
        set_property(SOURCE ${_changed} TARGET_DIRECTORY ${target} APPEND PROPERTY INCLUDE_DIRECTORIES "${_controls}")
        set_property(SOURCE ${_changed} TARGET_DIRECTORY ${target} APPEND PROPERTY COMPILE_DEFINITIONS MAG_GAME_CONTROLS_HOOKS=1)
    endif()

    set(_sources
        "${_controls}/mag_game_controls_adapter.cpp" "${_controls}/mag_game_controls_catalog.cpp"
        "${_controls}/mag_game_controls_guest.cpp" "${_controls}/mag_game_controls_hooks.cpp"
        "${_controls}/mag_game_controls_mouse_aim.cpp" "${_controls}/mag_game_controls_runtime.cpp")
    if(NOT APPLE)
        # macOS adds the portable player-controls module with its settings UI
        # (MagPlayerSettings.cmake). Elsewhere add it here, without a desktop capture backend yet.
        foreach(_name player_controls_rust player_controls_store player_controls_settings
                player_input_capture player_preferences_file)
            list(APPEND _sources "${_src}/${_name}.cpp")
        endforeach()
        list(APPEND _sources "${_controls}/mag_game_controls_desktop_none.cpp")
    endif()
    target_sources(${target} PRIVATE ${_sources})
    set_source_files_properties(${_sources} TARGET_DIRECTORY ${target} PROPERTIES
        INCLUDE_DIRECTORIES "${RECOMP_DIR};${PS3RECOMP_DIR}/runtime;${PS3RECOMP_DIR};${PS3RECOMP_DIR}/include;${_src};${_controls}")
    set(MAG_GAME_CONTROLS_RECEIPT_EXTRA
        "game-controls-sources=${_controls}"
        "game-controls-build=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MagControls.cmake"
        PARENT_SCOPE)
endfunction()
