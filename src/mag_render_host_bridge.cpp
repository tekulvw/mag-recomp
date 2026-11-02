#include "mag_render_host_bridge.h"
#include <cstddef>
#include <cstring>
#if defined(__APPLE__)
#include "rsx_metal_backend.h"
static_assert(sizeof(MagRenderHostOwner) == sizeof(rsx_native_owner));
static_assert(alignof(MagRenderHostOwner) == alignof(rsx_native_owner));
static_assert(offsetof(MagRenderHostOwner, lifetime) == offsetof(rsx_native_owner, lifetime));
static_assert(offsetof(MagRenderHostOwner, retain) == offsetof(rsx_native_owner, retain));
static_assert(offsetof(MagRenderHostOwner, try_begin) == offsetof(rsx_native_owner, try_begin));
static_assert(sizeof(MagRenderHostImage) == sizeof(rsx_native_image));
static_assert(alignof(MagRenderHostImage) == alignof(rsx_native_image));
static_assert(offsetof(MagRenderHostImage, resource_generation) ==
              offsetof(rsx_native_image, resource_generation));
static_assert(offsetof(MagRenderHostImage, image) == offsetof(rsx_native_image, image));
static_assert(offsetof(MagRenderHostImage, retain) == offsetof(rsx_native_image, retain));
static_assert(offsetof(MagRenderHostImage, validate) == offsetof(rsx_native_image, validate));
static_assert(sizeof(MagRenderFrameSource) == sizeof(rsx_native_frame_source));
static_assert(sizeof(MagRenderHostFrame) == sizeof(rsx_native_frame));
static_assert(alignof(MagRenderHostFrame) == alignof(rsx_native_frame));
static_assert(offsetof(MagRenderHostFrame, preflight) == offsetof(rsx_native_frame, preflight));
static_assert(offsetof(MagRenderHostFrame, committed) == offsetof(rsx_native_frame, committed));
static_assert(sizeof(MagRenderFrameEvent) == sizeof(rsx_native_frame_event));
static_assert(offsetof(MagRenderFrameEvent, source) == offsetof(rsx_native_frame_event, source));
static_assert(offsetof(MagRenderFrameEvent, target_generation) ==
              offsetof(rsx_native_frame_event, target_generation));
static_assert(offsetof(MagRenderFrameEvent, presented_time) ==
              offsetof(rsx_native_frame_event, presented_time));
#endif
extern "C" int mag_render_frontend_borrow_owner(MagRenderHostOwner *out) {
    if (!out || out->abi_version != MAG_RENDER_HOST_OWNER_ABI || out->struct_size != sizeof(*out))
        return -1;
    if (mag_render_native_abi_version() != 1)
        return -2;
#if defined(__APPLE__)
    rsx_native_owner owner = {};
    owner.abi_version = 1;
    owner.struct_size = sizeof(owner);
    int status = rsx_metal_backend_borrow_owner(&owner);
    if (!status)
        std::memcpy(out, &owner, sizeof(owner));
    return status;
#else
    return -2;
#endif
}
extern "C" int mag_render_frontend_borrow_image(uint64_t life, uint32_t surface, MagRenderHostImage *out) {
    if (!out || out->abi_version != MAG_RENDER_HOST_IMAGE_ABI || out->struct_size != sizeof(*out))
        return -1;
#if defined(__APPLE__)
    rsx_native_image image = {};
    image.abi_version = 1;
    image.struct_size = sizeof(image);
    int status = rsx_metal_backend_borrow_image(life, surface, &image);
    if (!status)
        std::memcpy(out, &image, sizeof(image));
    return status;
#else
    (void)life;
    (void)surface;
    return -2;
#endif
}

extern "C" int mag_render_frontend_reserve_frame(const MagRenderHostImage *image,
                                                 const MagRenderFrameSource *source,
                                                 MagRenderHostFrame *out) {
    if (!image || !source || !out || image->abi_version != 1 || image->struct_size != sizeof(*image) ||
        source->abi_version != 1 || source->struct_size != sizeof(*source) || out->abi_version != 1 ||
        out->struct_size != sizeof(*out))
        return -1;
#if defined(__APPLE__)
    rsx_native_image native_image;
    rsx_native_frame_source native_source;
    rsx_native_frame frame = {};
    frame.abi_version = 1;
    frame.struct_size = sizeof(frame);
    std::memcpy(&native_image, image, sizeof(native_image));
    std::memcpy(&native_source, source, sizeof(native_source));
    int status = rsx_metal_backend_reserve_frame(&native_image, &native_source, &frame);
    if (!status)
        std::memcpy(out, &frame, sizeof(frame));
    return status;
#else
    return -2;
#endif
}
extern "C" int mag_render_frontend_cancel_frame(uint64_t life, uint64_t reservation) {
#if defined(__APPLE__)
    return rsx_metal_backend_cancel_frame(life, reservation);
#else
    (void)life;
    (void)reservation;
    return -2;
#endif
}
extern "C" int mag_render_frontend_poll_frames(MagRenderFrameEvent *out, uint32_t capacity, uint32_t *count) {
    if (count)
        *count = 0;
    if (!out || !count || !capacity || capacity > 64)
        return -1;
#if defined(__APPLE__)
    rsx_native_frame_event events[64] = {};
    uint32_t native_count = 0;
    int status = rsx_metal_backend_poll_frames(events, capacity, &native_count);
    if (native_count > capacity)
        return -6;
    if (native_count)
        std::memcpy(out, events, native_count * sizeof(*out));
    *count = native_count;
    return status;
#else
    return -2;
#endif
}
