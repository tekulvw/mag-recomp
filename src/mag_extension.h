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
    /* Optional host-only setup before settings and guest allocation. */
    void (*prepare)(const char* elf);
    /* Optional whole-payload replacement at the WWS policy's payload call. Called with the SPU
     * context and the payload's entry (local-store address) before the lifted job runs. Return
     * true only after completing the invocation (every guest-visible effect, including the job
     * manager hand-back) and leaving the context at the policy's continuation; return false,
     * with no guest-visible change, to run the lifted job unchanged. Reached only from a build
     * whose policy lift carries tools/payload_override_hooks.py. */
    bool (*payload_override)(void* spu_ctx, uint32_t target);
} MagExtensionCallbacks;

/* Adds every non-null slot. prepare, start, frame_poll and stop accept up to eight registrants,
 * called in registration order (stop in reverse); payload_override accepts up to eight, asked in
 * registration order until one handles the payload; trace_enter, trace_exit, payload_select and
 * payload_begin have one owner each. Returns 0, or -1 (changing nothing) if a single-owner slot
 * is already taken or a lifecycle slot is full. */
int mag_extension_register(const MagExtensionCallbacks* callbacks);
/* Never null; every slot is null when nothing is registered. */
const MagExtensionCallbacks* mag_extension(void);
/* The payload-call seam (tools/payload_override_hooks.py): nonzero when a registered
 * payload_override handled the payload; zero, at the cost of one branch, when none is registered. */
int mag_payload_override(void* spu_ctx, uint32_t target);

#ifdef __cplusplus
}
#endif
