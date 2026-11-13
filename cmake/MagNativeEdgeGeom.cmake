# Opt-in native Edge geometry job (src/native_edgegeom, docs/native-edgegeom.md).
#
# Off by default. With MAG_NATIVE_EDGEGEOM=ON the engine links a payload override that runs the
# streamed Edge geometry job natively when the environment sets MAG_NATIVE_EDGEGEOM=1; the lifted
# job keeps running for every invocation it refuses. The override is only reachable through the
# payload-override lift step (tools/payload_override_hooks.py), so configuring without it fails
# instead of building a binary whose native path could never run.
option(MAG_NATIVE_EDGEGEOM "Link the opt-in native Edge geometry job (enabled at run time by MAG_NATIVE_EDGEGEOM=1)" OFF)

# Adds the implementation to `target` and returns its receipt entries in MAG_NATIVE_EDGEGEOM_RECEIPT_EXTRA.
function(mag_enable_native_edgegeom target sdk spu_dir image)
    set(MAG_NATIVE_EDGEGEOM_RECEIPT_EXTRA "" PARENT_SCOPE)
    if(NOT MAG_NATIVE_EDGEGEOM)
        return()
    endif()
    if(NOT image STREQUAL "mage_g")
        message(FATAL_ERROR "MAG_NATIVE_EDGEGEOM applies to the mage_g engine image only")
    endif()
    set(_lift "${spu_dir}/provenance.json")
    if(NOT EXISTS "${_lift}")
        message(FATAL_ERROR "MAG_NATIVE_EDGEGEOM needs the engine SPU lift (${_lift})")
    endif()
    file(READ "${_lift}" _record)
    string(JSON _hooks ERROR_VARIABLE _missing GET "${_record}" payload_override_hooks)
    if(_missing)
        message(FATAL_ERROR "MAG_NATIVE_EDGEGEOM needs the payload-override lift step: run "
            "tools/payload_override_hooks.py --directory ${spu_dir} before configuring")
    endif()
    get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(_sources "${_root}/src/native_edgegeom/edgegeom_core.cpp" "${_root}/src/native_edgegeom/edgegeom_runtime.cpp")
    target_sources(${target} PRIVATE ${_sources})
    # Only linked engines may select a native default before static initialization.
    target_compile_definitions(${target} PRIVATE MAG_HAS_NATIVE_EDGEGEOM=1)
    set_source_files_properties(${_sources} TARGET_DIRECTORY ${target} PROPERTIES
        INCLUDE_DIRECTORIES "${_root}/src/native_edgegeom;${sdk}/runtime/spu;${sdk}/runtime;${sdk}")
    # The facing rule needs unfused area products; clang sources say so themselves.
    if(NOT MSVC)
        set_source_files_properties(${_sources} TARGET_DIRECTORY ${target} PROPERTIES
            COMPILE_OPTIONS "-O2;-ffp-contract=off")
    endif()
    set(MAG_NATIVE_EDGEGEOM_RECEIPT_EXTRA
        "native-edgegeom-sources=${_root}/src/native_edgegeom"
        "native-edgegeom-build=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MagNativeEdgeGeom.cmake"
        PARENT_SCOPE)
endfunction()
