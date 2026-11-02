#ifndef MAG_RENDER_BACKEND_BRIDGE_H
#define MAG_RENDER_BACKEND_BRIDGE_H
#include <stdint.h>
#include "rsx_frontend.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MAG_RENDER_SDK_API_VERSION 1u
#define MAG_RENDER_SDK_LAYOUT_WORDS 52u
/* SDK source-bound opt-in bridge; device is borrowed from its existing owner.
 * Rust never discovers, creates or takes ownership of a device/queue. */
typedef struct MagRenderSdkApi {
    uint32_t abi_version, struct_size;
    rsx_frontend_device *device;
    uint32_t layout[MAG_RENDER_SDK_LAYOUT_WORDS];
    int (*caps)(rsx_frontend_device *, rsx_frontend_caps_desc *);
    int (*buffer_create)(rsx_frontend_device *, const rsx_frontend_buffer_desc *, rsx_frontend_buffer *);
    int (*buffer_release)(rsx_frontend_device *, rsx_frontend_buffer);
    int (*buffer_read)(rsx_frontend_device *, rsx_frontend_buffer, uint64_t, void *, uint64_t);
    int (*pipeline_create)(rsx_frontend_device *, const rsx_frontend_pipeline_desc *, rsx_frontend_pipeline *);
    int (*pipeline_release)(rsx_frontend_device *, rsx_frontend_pipeline);
    int (*submit)(rsx_frontend_device *, const rsx_frontend_frame *, rsx_frontend_ticket *);
    int (*poll)(rsx_frontend_device *, rsx_frontend_event *, uint32_t, uint32_t *);
} MagRenderSdkApi;
int mag_render_sdk_api(rsx_frontend_device *, MagRenderSdkApi *);
/* Synthetic diagnostic entry only. All pointers/lengths must be valid for this
 * synchronous call. Callback signals the fixture's genuine queued Metal gate. */
typedef struct MagRenderSdkProbeReport {
    uint32_t abi_version, struct_size;
    int32_t status;
    uint32_t submitted, completed, pending;
    uint64_t retained_before_terminal, retained_after_terminal, upload_bytes;
} MagRenderSdkProbeReport;
int mag_render_sdk_compute_probe(const MagRenderSdkApi *, const rsx_frontend_pipeline_desc *,
    void (*release_gate)(void *), void *gate_context, MagRenderSdkProbeReport *);
#ifdef __cplusplus
}
#endif
#endif
