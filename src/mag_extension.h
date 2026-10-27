#pragma once
/* Optional runtime callbacks for build extensions (MAG_EXTENSIONS). Extension code fills a
 * MagExtensionCallbacks from a static constructor and registers it; with nothing registered every
 * slot is null and each call site costs one branch. Guest contexts are passed as opaque pointers. */
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MagExtensionCallbacks {
    void (*start)(const char* elf);
    void (*frame_poll)(void);
    void (*stop)(void);
    void (*trace_enter)(void* ppu_ctx, uint32_t function, const uint64_t args[3], uint64_t scratch[8]);
    void (*trace_exit)(void* ppu_ctx, uint32_t function, const uint64_t args[3], int completed, uint64_t scratch[8]);
    bool (*payload_select)(uint32_t queue, uint64_t capture);
    void (*payload_begin)(void* spu_ctx, uint64_t span, uint32_t target);
} MagExtensionCallbacks;

/* Adds every non-null slot. Returns 0, or -1 (changing nothing) if any of them is already taken. */
int mag_extension_register(const MagExtensionCallbacks* callbacks);
/* Never null; every slot is null when nothing is registered. */
const MagExtensionCallbacks* mag_extension(void);

#ifdef __cplusplus
}
#endif
