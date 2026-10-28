# Pinned-runtime diagnostics use two Darwin APIs outside their Apple guards.
# Scope this compatibility surface to that translation unit, keeping SDK source
# and every other source's compile flags unchanged.
get_filename_component(MAG_LINUX_COMPAT_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/linux/compat" ABSOLUTE)
function(mag_linux_compat_runtime sdk)
    set(source "${sdk}/runtime/spu/spu_coherency.c")
    set_property(SOURCE "${source}" DIRECTORY "${sdk}" APPEND PROPERTY
        INCLUDE_DIRECTORIES "${MAG_LINUX_COMPAT_DIR}")
    set_property(SOURCE "${source}" DIRECTORY "${sdk}" APPEND PROPERTY
        COMPILE_OPTIONS "-include" "${MAG_LINUX_COMPAT_DIR}/mag_linux_compat.h")
    target_link_libraries(ps3recomp_runtime PUBLIC ${CMAKE_DL_LIBS})
endfunction()
