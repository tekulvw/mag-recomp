# Linux runtime configuration defaults, applied before the runtime subdirectory.
#
# glibc carves a thread's static TLS out of its stack. Unless it is allocated per
# thread on first use, the runtime's SPU interpreter decode cache is a 2 MiB
# thread_local, so the static TLS block outgrows a 1 MiB stack and every thread
# created with one fails to start (pthread_create returns EINVAL). The SPURS
# workers are such threads: SPURS initialization then fails and the launcher
# cannot start its decompression jobs. macOS and Windows keep TLS apart from the
# stack and are unaffected. A cache entry the user sets explicitly still wins.
include_guard(GLOBAL)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PS3RECOMP_SPU_DECODE_CACHE_HEAP ON CACHE BOOL
        "Allocate the SPU interpreter decode cache per thread on first use instead of in static TLS")
endif()
