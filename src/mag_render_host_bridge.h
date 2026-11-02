#ifndef MAG_RENDER_HOST_BRIDGE_H
#define MAG_RENDER_HOST_BRIDGE_H
#include "mag_render_host_owner.h"
#include "mag_render_host_image.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Narrow original-owner metadata only. No discovery/new device, guest packet
 * construction, or fallback. Caller must retain returned metadata before use. */
int mag_render_frontend_borrow_owner(MagRenderHostOwner *);
int mag_render_frontend_borrow_image(uint64_t owner_lifetime,uint32_t backend_surface,MagRenderHostImage *);
uint32_t mag_render_native_abi_version(void);
#ifdef __cplusplus
}
#endif
#endif
