#ifndef MAG_RENDER_NATIVE_API_H
#define MAG_RENDER_NATIVE_API_H
#include <stdint.h>
#include "mag_render_native.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MAG_RENDER_NATIVE_API_VERSION 1u
#define MAG_RENDER_NATIVE_LAYOUT_WORDS 52u
/* Frontend-owned native table; device is borrowed from its retained host owner.
 * Rust never discovers, creates or takes ownership of a device/queue. */
typedef struct MagRenderNativeApi {
    uint32_t abi_version, struct_size;
    mag_native_device *device;
    uint32_t layout[MAG_RENDER_NATIVE_LAYOUT_WORDS];
    int (*caps)(mag_native_device *, mag_native_caps_desc *);
    int (*buffer_create)(mag_native_device *, const mag_native_buffer_desc *, mag_native_buffer *);
    int (*buffer_release)(mag_native_device *, mag_native_buffer);
    int (*buffer_read)(mag_native_device *, mag_native_buffer, uint64_t, void *, uint64_t);
    int (*pipeline_create)(mag_native_device *, const mag_native_pipeline_desc *, mag_native_pipeline *);
    int (*pipeline_release)(mag_native_device *, mag_native_pipeline);
    int (*submit)(mag_native_device *, const mag_native_frame *, mag_native_ticket *);
    int (*poll)(mag_native_device *, mag_native_event *, uint32_t, uint32_t *);
} MagRenderNativeApi;
int mag_render_native_get_api(mag_native_device *, MagRenderNativeApi *);
/* Synthetic diagnostic entry only. All pointers/lengths must be valid for this
 * synchronous call. Callback signals the fixture's genuine queued host gate. */
typedef struct MagRenderNativeProbeReport {
    uint32_t abi_version, struct_size;
    int32_t status;
    uint32_t submitted, completed, pending;
    uint64_t retained_before_terminal, retained_after_terminal, upload_bytes;
} MagRenderNativeProbeReport;
int mag_render_native_probe_owner(const MagRenderHostOwner *, const mag_native_pipeline_desc *,
    void (*release_gate)(void *), void *gate_context, MagRenderNativeProbeReport *);
#ifdef __cplusplus
}
#endif
#endif
