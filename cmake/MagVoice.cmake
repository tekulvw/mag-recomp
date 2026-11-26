# Opt-in MAG voice service (src/voice, docs/voice-chat.md).
#
# Off by default. With MAG_VOICE=ON the mage_g engine links the service; it still does nothing
# until the environment selects a voice mode, and until then the runtime's own cellVoice stub
# answers every call. With the option off the build, its outputs and its receipts are unchanged.
option(MAG_VOICE "Link the opt-in MAG voice service into the mage_g engine (selected at run time)" OFF)

# Adds the service to `target` and returns its receipt entries in MAG_VOICE_RECEIPT_EXTRA.
function(mag_enable_voice target sdk image)
    set(MAG_VOICE_RECEIPT_EXTRA "" PARENT_SCOPE)
    if(NOT MAG_VOICE)
        return()
    endif()
    if(NOT image STREQUAL "mage_g")
        message(FATAL_ERROR "MAG_VOICE applies to the mage_g engine image only")
    endif()
    get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(_sources "${_root}/src/voice/mag_voice_core.cpp" "${_root}/src/voice/mag_voice_hle.cpp")
    target_sources(${target} PRIVATE ${_sources})
    set_source_files_properties(${_sources} TARGET_DIRECTORY ${target} PROPERTIES
        INCLUDE_DIRECTORIES "${_root}/src/voice;${_root}/src;${sdk}/runtime;${sdk}/include")
    set(MAG_VOICE_RECEIPT_EXTRA
        "voice-sources=${_root}/src/voice"
        "voice-build=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MagVoice.cmake"
        PARENT_SCOPE)
endfunction()
