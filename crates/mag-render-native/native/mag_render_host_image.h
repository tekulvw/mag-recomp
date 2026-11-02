#ifndef MAG_RENDER_HOST_IMAGE_H
#define MAG_RENDER_HOST_IMAGE_H
#include "mag_render_host_owner.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MAG_RENDER_HOST_IMAGE_ABI 1u
/* Existing same-device image, explicitly selected by the host. Metadata is
 * unretained. retain must succeed before dereferencing image; callbacks/userdata
 * are process-lived. Resource generation never repeats on slot reuse.
 * validate is called only under this owner's native queue lease before encoding;
 * it rejects retired/replaced targets even while an older GPU lease retains it.
 * release never waits for GPU work or calls original shutdown. */
typedef struct MagRenderHostImage {
    uint32_t abi_version, struct_size, backend, format, width, height, reserved, reserved1;
    uint64_t owner_lifetime, resource_id, resource_generation;
    void *image, *userdata;
    int (*retain)(void *, uint64_t, uint64_t, uint64_t);
    void (*release)(void *, uint64_t, uint64_t, uint64_t);
    int (*validate)(void *, uint64_t, uint64_t, uint64_t);
} MagRenderHostImage;
typedef struct mag_native_image {
    uint64_t device_generation, id;
} mag_native_image;
#ifdef __cplusplus
}
#endif
#endif
