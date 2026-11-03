#ifndef MAG_RENDER_HOST_FRAME_H
#define MAG_RENDER_HOST_FRAME_H
#include "mag_render_host_image.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MAG_RENDER_HOST_FRAME_ABI 1u
/* engine_update_id is the accepted engine-update association, NOT a proven
 * display frame. Opaque source IDs are copied from the actual private cohort transaction.
 * This owner layer does not invent source frame, pass, camera or world facts. */
typedef struct MagRenderFrameSource {
    uint32_t abi_version, struct_size;
    uint64_t engine_update_id, cohort_id, source_publication, pass_occurrence;
} MagRenderFrameSource;
typedef struct MagRenderHostFrame {
    uint32_t abi_version, struct_size;
    uint64_t owner_lifetime, reservation;
    void *userdata;
    /* Called only while the same original owner's native queue lease is held. */
    int (*preflight)(void *, uint64_t, uint64_t, uint64_t, uint64_t);
    /* Real queue commit already occurred: nonzero return means indeterminate,
     * never definitely rejected. disposition0accepted/1indeterminate. */
    int (*committed)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint32_t);
    /* Definitely uncommitted only; must reject a queued/indeterminate frame. */
    int (*cancel)(void *, uint64_t, uint64_t);
} MagRenderHostFrame;
enum mag_render_frame_event_kind {
    MAG_RENDER_FRAME_NATIVE_QUEUED = 1,
    MAG_RENDER_FRAME_FINAL_GPU_TERMINAL = 2,
    MAG_RENDER_FRAME_PRESENTED = 3,
    MAG_RENDER_FRAME_SKIPPED = 4
};
typedef struct MagRenderFrameEvent {
    uint32_t abi_version, struct_size, kind, succeeded;
    uint64_t owner_lifetime, reservation;
    MagRenderFrameSource source;
    uint64_t native_device_generation, native_submission;
    uint64_t target_id, target_generation;
    uint64_t guest_flip_id, drawable_id;
    uint32_t guest_buffer_id, native_status;
    /* Clock is specifically CAMetalDrawable.presentedTime seconds. No FPS or
     * cross-clock arithmetic is implied by this separate physical-present fact. */
    double presented_time;
} MagRenderFrameEvent;
#ifdef __cplusplus
}
#endif
#endif
