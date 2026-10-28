#ifndef MAG_LINUX_COMPAT_H
#define MAG_LINUX_COMPAT_H
/* Source-scoped compatibility for the pinned SPU coherency diagnostics.
 * These are actual Linux time/load-bias observations, not placeholder values. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <link.h>

#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC_RAW
static inline uint64_t clock_gettime_nsec_np(clockid_t clock_id)
{
    struct timespec ts;
    if (clock_gettime(clock_id, &ts) != 0) {
        perror("MAG diagnostic clock_gettime");
        abort();
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
struct mag_linux_executable_bias {
    intptr_t bias;
    int found;
};
static int mag_linux_find_executable_bias(struct dl_phdr_info* info, size_t size, void* opaque)
{
    (void)size;
    struct mag_linux_executable_bias* result = (struct mag_linux_executable_bias*)opaque;
    /* glibc represents the main executable with the empty name. dlpi_addr is
     * its ELF load bias: nonzero for PIE under ASLR and correctly zero for EXEC. */
    if (info->dlpi_name && info->dlpi_name[0] == '\0') {
        result->bias = (intptr_t)info->dlpi_addr;
        result->found = 1;
        return 1;
    }
    return 0;
}
static inline intptr_t _dyld_get_image_vmaddr_slide(uint32_t index)
{
    struct mag_linux_executable_bias result = {0, 0};
    if (index == 0) dl_iterate_phdr(mag_linux_find_executable_bias, &result);
    if (!result.found) {
        fprintf(stderr, "MAG diagnostic executable load bias unavailable (image %u)\n", index);
        abort();
    }
    return result.bias;
}
#endif
