# Explicit 2.12 development build. The proven default remains retail v1.0.
set(MAG_RETAIL_TARGET "retail-v1.0" CACHE STRING "Retail build target")
set_property(CACHE MAG_RETAIL_TARGET PROPERTY STRINGS retail-v1.0 retail-v2.12)
option(MAG_V212_EXPERIMENTAL_BUILD "Permit the isolated unverified 2.12 native build" OFF)
set(MAG_RETAIL_V212 FALSE)
if(NOT MAG_RETAIL_TARGET STREQUAL "retail-v1.0" AND NOT MAG_RETAIL_TARGET STREQUAL "retail-v2.12")
    message(FATAL_ERROR "MAG_RETAIL_TARGET must be retail-v1.0 or retail-v2.12")
endif()
if(MAG_RETAIL_TARGET STREQUAL "retail-v2.12")
    if(NOT MAG_V212_EXPERIMENTAL_BUILD)
        message(FATAL_ERROR
            "retail-v2.12 remains audit-only without MAG_V212_EXPERIMENTAL_BUILD=ON. "
            "Use tools/inventory.py and tools/lift.py --target retail-v2.12 for offline generation. "
            "Native acceptance is separate from the explicit experimental build.")
    endif()
    set(MAG_RETAIL_V212 TRUE)
    foreach(_unported MAG_RENDER_FRONTEND_RUST MAG_RENDER_FRONTEND_NATIVE MAG_RENDER_ADAPTER
            MAG_RENDER_ADAPTER_SHADOW_VIEW MAG_RENDER_ADAPTER_COHORT MAG_RENDER_ADAPTER_UI_HANDOFF
            MAG_GAME_CONTROLS MAG_NATIVE_EDGEGEOM MAG_VOICE MAG_INPUT_RUST)
        if(${_unported})
            message(FATAL_ERROR "${_unported} is not ported to retail-v2.12")
        endif()
    endforeach()
    if(MAG_EXTENSIONS)
        message(FATAL_ERROR "MAG_EXTENSIONS are not ported to retail-v2.12")
    endif()
endif()
