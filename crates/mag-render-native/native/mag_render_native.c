/* Extracted from ps3recomp libs/video/rsx_frontend.c at 6e0fcc2e026cddc02fb94f23050ac8429a03306a.
 * Copyright (c) 2026 Ned Heller / sp00nznet. MIT; see LICENSE.ps3recomp.
 * Frontend-owned implementation; no SDK runtime/header dependency. */
#include "mag_render_native_internal.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct mag_native_device { const mag_native_ops *ops; void *context; };
static _Atomic uint64_t s_generation = 1;
uint64_t mag_native_next_generation(void) {
    uint64_t current = atomic_load(&s_generation);
    while (current && current != UINT64_MAX) {
        if (atomic_compare_exchange_weak(&s_generation, &current, current + 1)) return current;
    }
    return 0;
}
mag_native_device *mag_native_wrap(const mag_native_ops *ops, void *context) {
    if (!ops || !context) return NULL;
    mag_native_device *device = malloc(sizeof(*device));
    if (device) *device = (mag_native_device){ops, context};
    return device;
}
int mag_native_close(mag_native_device *device) {
    if (!device) return MAG_NATIVE_INVALID;
    int status=device->ops->close(device->context);
    if (status==MAG_NATIVE_OK) free(device);
    return status;
}
int mag_native_create(const MagRenderHostOwner *owner,mag_native_device **out) {
    if (out) *out=NULL;
    if (!owner || !out || owner->abi_version!=MAG_RENDER_HOST_OWNER_ABI || owner->struct_size!=sizeof(*owner) || owner->reserved || owner->reserved1 || !owner->lifetime || !owner->retain || !owner->release || !owner->try_begin || !owner->end) return MAG_NATIVE_INVALID;
    switch(owner->backend) {
#ifdef MAG_RENDER_NATIVE_METAL
        case MAG_RENDER_HOST_METAL:return mag_native_metal_create(owner,out);
#endif
#ifdef MAG_RENDER_NATIVE_VULKAN
        case MAG_RENDER_HOST_VULKAN:return mag_native_vulkan_create(owner,out);
#endif
#ifdef MAG_RENDER_NATIVE_D3D12
        case MAG_RENDER_HOST_D3D12:return mag_native_d3d12_create(owner,out);
#endif
        default:return MAG_NATIVE_UNSUPPORTED;
    }
}
#define VALID(d) ((d) && (d)->struct_size == sizeof(*(d)) && (d)->abi_version == MAG_NATIVE_ABI_VERSION)
int mag_native_caps(mag_native_device *device, mag_native_caps_desc *desc) {
    return device && VALID(desc) ? device->ops->caps(device->context, desc) : MAG_NATIVE_INVALID;
}
int mag_native_buffer_create(mag_native_device *device, const mag_native_buffer_desc *desc, mag_native_buffer *out) {
    if (out) memset(out, 0, sizeof(*out));
    return device && VALID(desc) && out ? device->ops->buffer_create(device->context,
        (mag_native_buffer){0}, desc, out) : MAG_NATIVE_INVALID;
}
int mag_native_buffer_update(mag_native_device *device, mag_native_buffer previous,
    const mag_native_buffer_desc *desc, mag_native_buffer *out) {
    if (out) memset(out, 0, sizeof(*out));
    return device && VALID(desc) && out && previous.id ? device->ops->buffer_create(device->context,
        previous, desc, out) : MAG_NATIVE_INVALID;
}
int mag_native_buffer_release(mag_native_device *device, mag_native_buffer ref) {
    return device ? device->ops->buffer_release(device->context, ref) : MAG_NATIVE_INVALID;
}
int mag_native_buffer_read(mag_native_device *device, mag_native_buffer ref,
    uint64_t offset, void *out, uint64_t length) {
    return device && out && length ? device->ops->buffer_read(device->context, ref, offset, out, length) : MAG_NATIVE_INVALID;
}
int mag_native_pipeline_create(mag_native_device *device, const mag_native_pipeline_desc *desc, mag_native_pipeline *out) {
    if (out) memset(out, 0, sizeof(*out));
    return device && VALID(desc) && out ? device->ops->pipeline_create(device->context, desc, out) : MAG_NATIVE_INVALID;
}
int mag_native_pipeline_release(mag_native_device *device, mag_native_pipeline ref) {
    return device ? device->ops->pipeline_release(device->context, ref) : MAG_NATIVE_INVALID;
}
int mag_native_submit(mag_native_device *device, const mag_native_frame *frame, mag_native_ticket *out) {
    if (out) memset(out, 0, sizeof(*out));
    return device && VALID(frame) && out ? device->ops->submit(device->context, frame, out) : MAG_NATIVE_INVALID;
}
int mag_native_poll(mag_native_device *device, mag_native_event *events, uint32_t capacity, uint32_t *count) {
    if (count) *count = 0;
    return device && events && capacity && capacity <= 64 && count ?
        device->ops->poll(device->context, events, capacity, count) : MAG_NATIVE_INVALID;
}

int mag_native_graphics_create(mag_native_device *device,const mag_native_graphics_desc *desc,mag_native_pipeline *out){
    if(out)memset(out,0,sizeof(*out));
    if(!device||!VALID(desc)||!out)return MAG_NATIVE_INVALID;
    return device->ops->graphics_create?device->ops->graphics_create(device->context,desc,out):MAG_NATIVE_UNSUPPORTED;
}
int mag_native_submit_batch(mag_native_device *device,const mag_native_batch *batch,mag_native_ticket *out){
    if(out)memset(out,0,sizeof(*out));
    if(!device||!VALID(batch)||!out)return MAG_NATIVE_INVALID;
    return device->ops->submit_batch?device->ops->submit_batch(device->context,batch,out):MAG_NATIVE_UNSUPPORTED;
}

int mag_native_scene_graphics_create(mag_native_device *device,const mag_native_scene_graphics_desc *desc,mag_native_pipeline *out){
 if(out)memset(out,0,sizeof(*out));
 if(!device||!desc||desc->struct_size!=sizeof(*desc)||desc->abi_version!=MAG_NATIVE_SCENE_ABI||!out)return MAG_NATIVE_INVALID;
 return device->ops->scene_graphics_create?device->ops->scene_graphics_create(device->context,desc,out):MAG_NATIVE_UNSUPPORTED;
}
int mag_native_submit_scene(mag_native_device *device,const mag_native_scene *scene,mag_native_ticket *out){
 if(out)memset(out,0,sizeof(*out));
 if(!device||!scene||scene->struct_size!=sizeof(*scene)||scene->abi_version!=MAG_NATIVE_SCENE_ABI||!out)return MAG_NATIVE_INVALID;
 return device->ops->submit_scene?device->ops->submit_scene(device->context,scene,out):MAG_NATIVE_UNSUPPORTED;
}

int mag_native_image_import(mag_native_device *device,const MagRenderHostImage *desc,mag_native_image *out){
 if(out)memset(out,0,sizeof(*out));
 if(!device||!desc||!out||desc->abi_version!=MAG_RENDER_HOST_IMAGE_ABI||desc->struct_size!=sizeof(*desc)||desc->reserved||desc->reserved1||!desc->owner_lifetime||!desc->resource_id||!desc->resource_generation||!desc->retain||!desc->release||!desc->validate)return MAG_NATIVE_INVALID;
 return device->ops->image_import?device->ops->image_import(device->context,desc,out):MAG_NATIVE_UNSUPPORTED;
}
int mag_native_image_release(mag_native_device *device,mag_native_image ref){
 if(!device){return MAG_NATIVE_INVALID;}return device->ops->image_release?device->ops->image_release(device->context,ref):MAG_NATIVE_UNSUPPORTED;
}
int mag_native_submit_scene_target(mag_native_device *device,const mag_native_scene *scene,mag_native_image ref,mag_native_ticket *out){
 if(out)memset(out,0,sizeof(*out));
 if(!device||!scene||!out||scene->struct_size!=sizeof(*scene)||scene->abi_version!=MAG_NATIVE_SCENE_ABI||!ref.device_generation||!ref.id)return MAG_NATIVE_INVALID;
 return device->ops->submit_scene_target?device->ops->submit_scene_target(device->context,scene,ref,out):MAG_NATIVE_UNSUPPORTED;
}
