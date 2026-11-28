# Transitional target selection for the 2.12 branch. Keep the proven native
# build unchanged until the new target's handoff and guest adapters are ported.
set(MAG_RETAIL_TARGET "retail-v1.0" CACHE STRING "Retail build target (2.12 is audit-only)")
set_property(CACHE MAG_RETAIL_TARGET PROPERTY STRINGS retail-v1.0 retail-v2.12)
if(NOT MAG_RETAIL_TARGET STREQUAL "retail-v1.0" AND NOT MAG_RETAIL_TARGET STREQUAL "retail-v2.12")
    message(FATAL_ERROR "MAG_RETAIL_TARGET must be retail-v1.0 or retail-v2.12")
endif()
if(MAG_RETAIL_TARGET STREQUAL "retail-v2.12")
    message(FATAL_ERROR
        "retail-v2.12 is audit-only on this branch. Native handoff and guest adapters are not ported. "
        "Use tools/inventory.py and tools/lift.py --target retail-v2.12 for offline generation. "
        "Keep its analysis/generated/build directories separate from retail-v1.0.")
endif()
