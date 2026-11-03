#ifndef MAG_RENDER_HOST_BRIDGE_H
#define MAG_RENDER_HOST_BRIDGE_H
#include "mag_render_host_frame.h"
#include "mag_render_host_image.h"
#include "mag_render_host_owner.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Narrow original-owner metadata only. No discovery/new device, guest packet
 * construction, or fallback. Caller must retain returned metadata before use. */
int mag_render_frontend_native_boundary(uint64_t *original_ticket);
/* Explicit host lifecycle only, outside consumer callback/owner gate. Normal
 * worker drop does not request SDK teardown; retry is qualified by owner life. */
int mag_render_frontend_shutdown_requested(uint64_t owner_lifetime);
int mag_render_frontend_retry_shutdown(uint64_t owner_lifetime);
int mag_render_frontend_borrow_owner(MagRenderHostOwner *);
int mag_render_frontend_borrow_owner_v2(MagRenderHostOwnerV2 *);
int mag_render_frontend_borrow_image(uint64_t owner_lifetime, uint32_t backend_surface, MagRenderHostImage *);
/* Plane initialization callback uses the retained image's existing identity and
 * userdata. Call under its native owner lease after the original boundary.
 * COLOR1/DEPTH2/STENCIL4 are actual full clears, not GPU completion evidence. */
int mag_render_frontend_validate_image_planes(void *, uint64_t owner_lifetime,
    uint64_t resource_id, uint64_t resource_generation, uint32_t required_planes);
int mag_render_frontend_reserve_frame(const MagRenderHostImage *, const MagRenderFrameSource *,
                                      MagRenderHostFrame *);
int mag_render_frontend_cancel_frame(uint64_t owner_lifetime, uint64_t reservation);
int mag_render_frontend_poll_frames(MagRenderFrameEvent *, uint32_t capacity, uint32_t *count);
uint32_t mag_render_native_abi_version(void);
#ifdef __cplusplus
}
#endif
#endif
