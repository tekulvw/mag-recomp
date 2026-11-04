#include "mag_render_host_bridge.h"
#include <cstddef>
#include <cstring>
#if defined(__APPLE__)
#include "rsx_metal_backend.h"
#elif defined(_WIN32)
#include "rsx_live_draw_owner.h"
#else
#include "rsx_vulkan_backend.h"
#endif
#if !defined(__APPLE__) && !defined(_WIN32) && defined(RSX_NATIVE_VULKAN_IMAGE_ABI)
#include "mag_render_vulkan_image.h"
static_assert(MAG_RENDER_VULKAN_IMAGE_ABI == RSX_NATIVE_VULKAN_IMAGE_ABI);
static_assert(sizeof(MagRenderHostImage) == sizeof(rsx_native_image));
static_assert(alignof(MagRenderHostImage) == alignof(rsx_native_image));
static_assert(offsetof(MagRenderHostImage, resource_generation) == offsetof(rsx_native_image, resource_generation));
static_assert(offsetof(MagRenderHostImage, image) == offsetof(rsx_native_image, image));
static_assert(offsetof(MagRenderHostImage, retain) == offsetof(rsx_native_image, retain));
static_assert(offsetof(MagRenderHostImage, validate) == offsetof(rsx_native_image, validate));
static_assert(sizeof(MagRenderVulkanImage) == sizeof(rsx_native_vulkan_image));
static_assert(alignof(MagRenderVulkanImage) == alignof(rsx_native_vulkan_image));
#define VULKAN_IMAGE_FIELD(field) static_assert(offsetof(MagRenderVulkanImage, field) == offsetof(rsx_native_vulkan_image, field))
VULKAN_IMAGE_FIELD(abi_version);VULKAN_IMAGE_FIELD(struct_size);VULKAN_IMAGE_FIELD(image_bits);
VULKAN_IMAGE_FIELD(format);VULKAN_IMAGE_FIELD(usage);VULKAN_IMAGE_FIELD(aspect);
VULKAN_IMAGE_FIELD(mip_count);VULKAN_IMAGE_FIELD(layers);VULKAN_IMAGE_FIELD(samples);
VULKAN_IMAGE_FIELD(queue_family);VULKAN_IMAGE_FIELD(layout);
#undef VULKAN_IMAGE_FIELD
#endif
#if defined(_WIN32) && defined(RSX_NATIVE_D3D12_IMAGE_ABI)
#include "mag_render_d3d12_image.h"
static_assert(MAG_RENDER_D3D12_IMAGE_ABI==RSX_NATIVE_D3D12_IMAGE_ABI);
static_assert(sizeof(MagRenderD3D12Image)==sizeof(rsx_native_d3d12_image));
static_assert(alignof(MagRenderD3D12Image)==alignof(rsx_native_d3d12_image));
static_assert(sizeof(MagRenderHostImage)==sizeof(rsx_native_image));
#define D3D_IMAGE_FIELD(field) static_assert(offsetof(MagRenderD3D12Image,field)==offsetof(rsx_native_d3d12_image,field))
D3D_IMAGE_FIELD(abi_version);D3D_IMAGE_FIELD(struct_size);D3D_IMAGE_FIELD(resource);D3D_IMAGE_FIELD(acquire_state);D3D_IMAGE_FIELD(handback_state);
#undef D3D_IMAGE_FIELD
static_assert(offsetof(MagRenderHostImage,image)==offsetof(rsx_native_image,image));
static_assert(offsetof(MagRenderHostImage,retain)==offsetof(rsx_native_image,retain));
static_assert(offsetof(MagRenderHostImage,validate)==offsetof(rsx_native_image,validate));
#endif
#if !defined(_WIN32)
static_assert(sizeof(MagRenderHostOwner) == sizeof(rsx_native_owner));
static_assert(alignof(MagRenderHostOwner) == alignof(rsx_native_owner));
static_assert(offsetof(MagRenderHostOwner, lifetime) == offsetof(rsx_native_owner, lifetime));
static_assert(offsetof(MagRenderHostOwner, retain) == offsetof(rsx_native_owner, retain));
static_assert(offsetof(MagRenderHostOwner, try_begin) == offsetof(rsx_native_owner, try_begin));
#endif
#if defined(__APPLE__)
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
#if !defined(_WIN32)
    rsx_native_owner owner = {};
    owner.abi_version = 1;
    owner.struct_size = sizeof(owner);
#if defined(__APPLE__)
    int status = rsx_metal_backend_borrow_owner(&owner);
#else
    int status = rsx_vulkan_backend_borrow_owner(&owner);
#endif
    if (!status)
        std::memcpy(out, &owner, sizeof(owner));
    return status;
#else
    rsx_live_draw_owner owner = {};
    owner.abi_version = RSX_LIVE_DRAW_OWNER_ABI;
    owner.struct_size = sizeof(owner);
    const int status = rsx_live_draw_borrow_owner(&owner);
    if (!status) {
        // The Windows provider has its own ABI and field order. Copy named
        // metadata only; retention still precedes every native dereference.
        MagRenderHostOwner result = {};
        result.abi_version = MAG_RENDER_HOST_OWNER_ABI;
        result.struct_size = sizeof(result);
        result.backend = MAG_RENDER_HOST_D3D12;
        result.lifetime = owner.lifetime;
        result.device = owner.device;
        result.queue = owner.queue;
        result.userdata = owner.userdata;
        result.retain = owner.retain;
        result.release = owner.release;
        result.try_begin = owner.try_begin;
        result.end = owner.end;
        *out = result;
    }
    return status;
#endif
}
extern "C" int mag_render_frontend_borrow_owner_v2(MagRenderHostOwnerV2 *out) {
    if(!out || out->abi_version!=2 || out->struct_size!=sizeof(*out))return -1;
#if !defined(__APPLE__) && !defined(_WIN32) && defined(RSX_NATIVE_OWNER_FEATURE_BC)
    static_assert(sizeof(MagRenderHostOwnerV2)==sizeof(rsx_native_owner_v2));
    static_assert(offsetof(MagRenderHostOwnerV2,base)==offsetof(rsx_native_owner_v2,base));
    static_assert(offsetof(MagRenderHostOwnerV2,known_features)==offsetof(rsx_native_owner_v2,known_features));
    static_assert(offsetof(MagRenderHostOwnerV2,enabled_features)==offsetof(rsx_native_owner_v2,enabled_features));
    rsx_native_owner_v2 owner={};owner.abi_version=2;owner.struct_size=sizeof(owner);
    int status=rsx_vulkan_backend_borrow_owner_v2(&owner);
    if(!status)std::memcpy(out,&owner,sizeof(owner));
    return status;
#else
    MagRenderHostOwner owner={};owner.abi_version=1;owner.struct_size=sizeof(owner);
    int status=mag_render_frontend_borrow_owner(&owner);
    if(!status){out->base=owner;out->known_features=out->enabled_features=0;}
    return status;
#endif
}
/* The SDK's reserved queue order for the Vulkan native backend, which looks
 * this up (weakly) when a device is created. Hosts without it report
 * UNSUPPORTED and the native backend keeps synchronous commits. */
extern "C" int mag_render_host_vulkan_queue_order(MagRenderVulkanQueueOrder *out) {
    if (!out || out->abi_version != MAG_RENDER_HOST_QUEUE_ORDER_ABI || out->struct_size != sizeof(*out))
        return -1;
#if !defined(__APPLE__) && !defined(_WIN32) && defined(RSX_NATIVE_QUEUE_ORDER_ABI)
    static_assert(sizeof(MagRenderVulkanQueueOrder) == sizeof(rsx_native_queue_order));
    static_assert(offsetof(MagRenderVulkanQueueOrder, lifetime) == offsetof(rsx_native_queue_order, lifetime));
    static_assert(offsetof(MagRenderVulkanQueueOrder, userdata) == offsetof(rsx_native_queue_order, userdata));
    static_assert(offsetof(MagRenderVulkanQueueOrder, reserve) == offsetof(rsx_native_queue_order, reserve));
    static_assert(offsetof(MagRenderVulkanQueueOrder, submit) == offsetof(rsx_native_queue_order, submit));
    rsx_native_queue_order order = {};
    order.abi_version = RSX_NATIVE_QUEUE_ORDER_ABI;
    order.struct_size = sizeof(order);
    const int status = rsx_vulkan_backend_queue_order(&order);
    if (!status)
        std::memcpy(out, &order, sizeof(order));
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
#elif defined(_WIN32) && defined(RSX_NATIVE_D3D12_IMAGE_ABI)
    rsx_native_image image={};image.abi_version=1;image.struct_size=sizeof(image);
    int status=rsx_live_draw_borrow_image(life,surface,&image);
    if(!status)std::memcpy(out,&image,sizeof(image));
    return status;
#elif !defined(_WIN32) && defined(RSX_NATIVE_VULKAN_IMAGE_ABI)
    rsx_native_image image = {};
    image.abi_version = 1;
    image.struct_size = sizeof(image);
    int status = rsx_vulkan_backend_borrow_image(life, surface, &image);
    if (!status)
        std::memcpy(out, &image, sizeof(image));
    return status;
#else
    (void)life;
    (void)surface;
    return -2;
#endif
}

extern "C" int mag_render_frontend_validate_image_planes(void *user, uint64_t life,
    uint64_t resource, uint64_t generation, uint32_t required) {
#if defined(__APPLE__)
    return rsx_metal_backend_validate_image_planes(user, life, resource, generation, required);
#elif defined(_WIN32) && defined(RSX_NATIVE_D3D12_IMAGE_ABI)
    return rsx_live_draw_validate_image_planes(user,life,resource,generation,required);
#elif !defined(_WIN32) && defined(RSX_NATIVE_VULKAN_IMAGE_ABI)
    return rsx_vulkan_backend_validate_image_planes(user, life, resource, generation, required);
#else
    (void)user; (void)life; (void)resource; (void)generation; (void)required;
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

extern "C" int mag_render_frontend_native_boundary(uint64_t *ticket) {
    if (!ticket) return -1;
    *ticket = 0;
#if defined(__APPLE__)
    return rsx_metal_backend_native_boundary(ticket);
#elif defined(_WIN32) && defined(RSX_NATIVE_D3D12_IMAGE_ABI)
    return rsx_live_draw_native_boundary(ticket);
#elif !defined(_WIN32) && defined(RSX_NATIVE_VULKAN_IMAGE_ABI)
    return rsx_vulkan_backend_native_boundary(ticket);
#else
    return -2;
#endif
}

extern "C" int mag_render_frontend_shutdown_requested(uint64_t lifetime) {
#if defined(__APPLE__)
    return rsx_metal_backend_owner_shutdown_requested(lifetime);
#elif defined(_WIN32)
    return rsx_live_draw_owner_shutdown_requested(lifetime);
#else
    return rsx_vulkan_backend_owner_shutdown_requested(lifetime);
#endif
}
extern "C" int mag_render_frontend_retry_shutdown(uint64_t lifetime) {
#if defined(__APPLE__)
    return rsx_metal_backend_owner_retry_shutdown(lifetime);
#elif defined(_WIN32)
    return rsx_live_draw_owner_retry_shutdown(lifetime);
#else
    return rsx_vulkan_backend_owner_retry_shutdown(lifetime);
#endif
}
