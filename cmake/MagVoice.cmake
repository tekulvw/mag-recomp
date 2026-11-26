# Opt-in MAG voice service (src/voice, docs/voice-chat.md).
#
# Off by default. With MAG_VOICE=ON the mage_g engine links the service; it still does nothing
# until the environment selects a voice mode, and until then the runtime's own cellVoice stub
# answers every call. With the option off the build, its outputs and its receipts are unchanged.
option(MAG_VOICE "Link the opt-in MAG voice service into the mage_g engine (selected at run time)" OFF)
set(MAG_OPUS_ROOT "" CACHE PATH "Opus install prefix for MAG_VOICE (include/opus/opus.h, lib/); blank searches the system")

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
    # Opus (BSD): an install prefix, or the system's (pkg-config, or the default search paths).
    set(_hints "")
    if(MAG_OPUS_ROOT)
        set(_hints HINTS "${MAG_OPUS_ROOT}" NO_DEFAULT_PATH)
    else()
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(MAG_OPUS_PC QUIET opus)
            if(MAG_OPUS_PC_FOUND)
                # Opus's pkg-config names include/opus; the sources include <opus/opus.h>.
                list(TRANSFORM MAG_OPUS_PC_INCLUDE_DIRS REPLACE "/opus$" "" OUTPUT_VARIABLE _pc_includes)
                set(_hints HINTS ${_pc_includes} ${MAG_OPUS_PC_LIBRARY_DIRS})
            endif()
        endif()
    endif()
    find_path(MAG_OPUS_INCLUDE_DIR opus/opus.h ${_hints} PATH_SUFFIXES include)
    find_library(MAG_OPUS_LIBRARY NAMES opus ${_hints} PATH_SUFFIXES lib lib64)
    if(NOT MAG_OPUS_INCLUDE_DIR OR NOT MAG_OPUS_LIBRARY)
        message(FATAL_ERROR "MAG_VOICE needs Opus: install it (libopus-dev, or the installer's managed copy) "
            "or set MAG_OPUS_ROOT to its install prefix")
    endif()
    set(_sources "${_root}/src/voice/mag_voice_core.cpp" "${_root}/src/voice/mag_voice_opus.cpp"
        "${_root}/src/voice/mag_voice_hle.cpp")
    target_sources(${target} PRIVATE ${_sources})
    set_source_files_properties(${_sources} TARGET_DIRECTORY ${target} PROPERTIES
        INCLUDE_DIRECTORIES "${_root}/src/voice;${_root}/src;${sdk}/runtime;${sdk}/include;${MAG_OPUS_INCLUDE_DIR}")
    target_link_libraries(${target} PRIVATE "${MAG_OPUS_LIBRARY}")
    set(MAG_VOICE_RECEIPT_EXTRA
        "voice-sources=${_root}/src/voice"
        "voice-build=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MagVoice.cmake"
        "voice-opus=${MAG_OPUS_LIBRARY}"
        PARENT_SCOPE)
endfunction()
