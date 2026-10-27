# Windows hosts have no POSIX headers. A handful of opt-in diagnostics in MAG and
# in the pinned runtime use pthreads, unistd.h or clock_gettime unguarded; on
# Windows those files get src/win32/posix on their include path and its
# implementation header force-included. The pinned runtime itself is unchanged.

set(MAG_WIN32_POSIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/win32/posix")
get_filename_component(MAG_WIN32_POSIX_DIR "${MAG_WIN32_POSIX_DIR}" ABSOLUTE)

# Runtime sources (relative to PS3RECOMP_DIR) that need the subset.
set(MAG_WIN32_POSIX_RUNTIME_SOURCES
    runtime/diagnostics/critical_trace.c
    runtime/diagnostics/job_io_recorder.c
    runtime/diagnostics/render_ablation.c
    libs/video/rsx_draw_engine.c
    libs/video/cellGcmSys.c
)

# Call after add_subdirectory(${sdk}): the properties belong to that directory.
function(mag_win32_posix_runtime sdk)
    foreach(source IN LISTS MAG_WIN32_POSIX_RUNTIME_SOURCES)
        set_property(SOURCE "${sdk}/${source}" DIRECTORY "${sdk}" APPEND PROPERTY
            INCLUDE_DIRECTORIES "${MAG_WIN32_POSIX_DIR}")
        set_property(SOURCE "${sdk}/${source}" DIRECTORY "${sdk}" APPEND PROPERTY
            COMPILE_OPTIONS "/FI${MAG_WIN32_POSIX_DIR}/mag_win32_posix.h")
    endforeach()
endfunction()

# MAG's own diagnostics include <pthread.h>/<unistd.h> directly.
function(mag_win32_posix_target target)
    target_include_directories(${target} PRIVATE "${MAG_WIN32_POSIX_DIR}")
endfunction()
