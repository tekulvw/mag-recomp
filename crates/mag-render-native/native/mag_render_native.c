/* Extracted from ps3recomp libs/video/rsx_frontend.c at 6e0fcc2e026cddc02fb94f23050ac8429a03306a.
 * Copyright (c) 2026 Ned Heller / sp00nznet. MIT; see LICENSE.ps3recomp.
 * Frontend-owned implementation; no SDK runtime/header dependency. */
#include "mag_render_native_internal.h"
#include "mag_render_scene_validate.h"
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
int mag_native_create_v2(const MagRenderHostOwnerV2 *owner,mag_native_device **out) {
    if(out)*out=NULL;
    if(!owner||!out||owner->abi_version!=2||owner->struct_size!=sizeof(*owner)||
       (owner->known_features&~MAG_RENDER_HOST_FEATURE_ALL)||
       (owner->enabled_features&~owner->known_features))return MAG_NATIVE_INVALID;
    const MagRenderHostOwner *base=&owner->base;
    if(base->abi_version!=1||base->struct_size!=sizeof(*base)||base->reserved||base->reserved1||
       !base->lifetime||!base->retain||!base->release||!base->try_begin||!base->end)return MAG_NATIVE_INVALID;
#ifdef MAG_RENDER_NATIVE_VULKAN
    if(base->backend==MAG_RENDER_HOST_VULKAN)return mag_native_vulkan_create_v2(owner,out);
#endif
    return mag_native_create(base,out);
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
int mag_native_buffer_write(mag_native_device *device, mag_native_buffer ref,
    uint64_t offset, const void *source, uint64_t length) {
    if (!device || !source || !length) return MAG_NATIVE_INVALID;
    return device->ops->buffer_write ? device->ops->buffer_write(device->context, ref, offset, source, length)
                                     : MAG_NATIVE_UNSUPPORTED;
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

int mag_native_submit_scene_frame(mag_native_device *device,const mag_native_scene *scene,mag_native_image ref,const MagRenderHostFrame *frame,mag_native_ticket *out){
 if(out)memset(out,0,sizeof(*out));
 if(!device||!scene||!frame||!out||scene->struct_size!=sizeof(*scene)||scene->abi_version!=MAG_NATIVE_SCENE_ABI||!ref.device_generation||!ref.id||frame->abi_version!=MAG_RENDER_HOST_FRAME_ABI||frame->struct_size!=sizeof(*frame)||!frame->owner_lifetime||!frame->reservation||!frame->preflight||!frame->committed||!frame->cancel)return MAG_NATIVE_INVALID;
 return device->ops->submit_scene_frame?device->ops->submit_scene_frame(device->context,scene,ref,frame,out):MAG_NATIVE_UNSUPPORTED;
}

int mag_native_scene_graphics_create_v2(mag_native_device *device,const mag_native_scene_graphics_v2_desc *desc,mag_native_pipeline *out){
 if(out)memset(out,0,sizeof(*out));
 if(!device||!out)return MAG_NATIVE_INVALID;
 int status=mag_native_scene_graphics_v2_validate(desc);if(status)return status;
 return device->ops->scene_graphics_create_v2?device->ops->scene_graphics_create_v2(device->context,desc,out):MAG_NATIVE_UNSUPPORTED;
}

int mag_native_scene_graphics_create_v3(mag_native_device *device,const mag_native_scene_graphics_v3_desc *desc,mag_native_pipeline *out){
 if(out)memset(out,0,sizeof(*out));
 if(!device||!out)return MAG_NATIVE_INVALID;
 int status=mag_native_scene_graphics_v3_validate(desc);if(status)return status;
 return device->ops->scene_graphics_create_v3?device->ops->scene_graphics_create_v3(device->context,desc,out):MAG_NATIVE_UNSUPPORTED;
}

int mag_native_sampler_create(mag_native_device *device,const mag_native_sampler_desc *desc,mag_native_sampler *out){
 if(out)memset(out,0,sizeof(*out));
 if(!device||!desc||!out||desc->struct_size!=sizeof(*desc)||desc->abi_version!=MAG_NATIVE_SAMPLER_ABI)return MAG_NATIVE_INVALID;
 int status=mag_native_sampler_state_validate(&desc->state);if(status)return status;
 return device->ops->sampler_create?device->ops->sampler_create(device->context,desc,out):MAG_NATIVE_UNSUPPORTED;
}
int mag_native_sampler_release(mag_native_device *device,mag_native_sampler sampler){
 if(!device||!sampler.device_generation||!sampler.id)return MAG_NATIVE_INVALID;
 return device->ops->sampler_release?device->ops->sampler_release(device->context,sampler):MAG_NATIVE_UNSUPPORTED;
}
int mag_native_submit_scene_v2(mag_native_device *device,const mag_native_scene_v2 *desc,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
 if(out)memset(out,0,sizeof(*out));
 if(!device||!desc||!out||desc->struct_size!=sizeof(*desc)||desc->abi_version!=2||desc->reserved||!desc->scene
    ||desc->scene->struct_size!=sizeof(*desc->scene)||desc->scene->abi_version!=1||desc->sampler_count>MAG_NATIVE_MAX_SAMPLERS
    ||(desc->sampler_count&&!desc->samplers)||(target&&(!target->device_generation||!target->id))
    ||(frame&&(!target||frame->struct_size!=sizeof(*frame)||frame->abi_version!=1||!frame->owner_lifetime||!frame->reservation||!frame->preflight||!frame->committed||!frame->cancel)))return MAG_NATIVE_INVALID;
 return device->ops->submit_scene_v2?device->ops->submit_scene_v2(device->context,desc,target,frame,out):MAG_NATIVE_UNSUPPORTED;
}

int mag_native_submit_scene_v3(mag_native_device *device,const mag_native_scene_v3 *desc,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
 if(out)memset(out,0,sizeof(*out));
 if(!device||!desc||!out||desc->struct_size!=sizeof(*desc)||desc->abi_version!=3||desc->reserved||
    desc->base.struct_size!=sizeof(desc->base)||desc->base.abi_version!=2||desc->base.reserved||!desc->base.scene||
    desc->base.scene->struct_size!=sizeof(*desc->base.scene)||desc->base.scene->abi_version!=1||
    desc->base.sampler_count>MAG_NATIVE_MAX_SAMPLERS||(desc->base.sampler_count&&!desc->base.samplers)||
    (target&&(!target->device_generation||!target->id))||
    (frame&&(!target||frame->struct_size!=sizeof(*frame)||frame->abi_version!=1||!frame->owner_lifetime||!frame->reservation||!frame->preflight||!frame->committed||!frame->cancel)))return MAG_NATIVE_INVALID;
 return device->ops->submit_scene_v3?device->ops->submit_scene_v3(device->context,desc,target,frame,out):MAG_NATIVE_UNSUPPORTED;
}

int mag_native_submit_scene_v4(mag_native_device *device,const mag_native_scene_v4 *desc,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
 if(out)memset(out,0,sizeof(*out));
 if(!device||!desc||!out||desc->struct_size!=sizeof(*desc)||desc->abi_version!=4||desc->reserved||desc->base.struct_size!=sizeof(desc->base)||desc->base.abi_version!=3||desc->base.reserved||
 desc->base.base.struct_size!=sizeof(desc->base.base)||desc->base.base.abi_version!=2||desc->base.base.reserved||!desc->base.base.scene||
 desc->base.base.scene->struct_size!=sizeof(*desc->base.base.scene)||desc->base.base.scene->abi_version!=1||
 desc->base.base.sampler_count>MAG_NATIVE_MAX_SAMPLERS||(desc->base.base.sampler_count&&!desc->base.base.samplers)||desc->stencil_count!=desc->base.base.scene->pass_count||!desc->stencils
 ||(target&&(!target->device_generation||!target->id))||(frame&&(!target||frame->abi_version!=1||frame->struct_size!=sizeof(*frame)||!frame->owner_lifetime||!frame->reservation||!frame->preflight||!frame->committed||!frame->cancel)))return MAG_NATIVE_INVALID;
 return device->ops->submit_scene_v4?device->ops->submit_scene_v4(device->context,desc,target,frame,out):MAG_NATIVE_UNSUPPORTED;
}

int mag_native_image_import_certified(mag_native_device *device,const mag_native_image_certificate *cert,mag_native_image *out){
 if(out){memset(out,0,sizeof(*out));}
 if(!device||!cert||!out)return MAG_NATIVE_INVALID;
 const MagRenderHostImage *h=&cert->host;
 if(h->abi_version!=1||h->struct_size!=sizeof(*h)||h->reserved||h->reserved1||!h->owner_lifetime||!h->resource_id||!h->resource_generation||!h->retain||!h->release||!h->validate)return MAG_NATIVE_INVALID;
 return device->ops->image_import_certified?device->ops->image_import_certified(device->context,cert,out):MAG_NATIVE_UNSUPPORTED;
}
int mag_native_submit_scene_v5(mag_native_device *device,const mag_native_scene_v5 *desc,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
 if(out)memset(out,0,sizeof(*out));
 if(!device||!desc||!out||desc->abi_version!=5||desc->struct_size!=sizeof(*desc)||desc->reserved||desc->reserved1||(target&&(!target->device_generation||!target->id))||(frame&&(!target||!frame->preflight||!frame->committed||!frame->cancel||!frame->owner_lifetime||!frame->reservation||frame->abi_version!=1||frame->struct_size!=sizeof(*frame))))return MAG_NATIVE_INVALID;
 return device->ops->submit_scene_v5?device->ops->submit_scene_v5(device->context,desc,target,frame,out):MAG_NATIVE_UNSUPPORTED;
}

int mag_native_reserve_scene_v5(mag_native_device *device,const mag_native_scene_import *imports,uint32_t count,mag_native_ticket *out){
 if(out)memset(out,0,sizeof(*out));
 if(!device||!out||!imports||!count||count>MAG_NATIVE_SCENE_MAX_IMAGES)return MAG_NATIVE_INVALID;
 return device->ops->reserve_scene?device->ops->reserve_scene(device->context,imports,count,out):MAG_NATIVE_UNSUPPORTED;
}
int mag_native_abandon_scene_reservation(mag_native_device *device){
 if(!device)return MAG_NATIVE_INVALID;
 return device->ops->abandon_scene_reservation?device->ops->abandon_scene_reservation(device->context):MAG_NATIVE_UNSUPPORTED;
}
int mag_native_scene_chunk_stats(mag_native_device *device,uint64_t *out,uint32_t count){
 if(out)memset(out,0,(size_t)count*sizeof(*out));
 if(!device||!out||count!=3)return MAG_NATIVE_INVALID;
 return device->ops->scene_chunk_stats?device->ops->scene_chunk_stats(device->context,out,count):MAG_NATIVE_UNSUPPORTED;
}
int mag_native_scene_graphics_create_aux(mag_native_device *device,const mag_native_scene_graphics_v3_desc *desc,uint32_t flags,mag_native_pipeline *out){
 if(out){*out=(mag_native_pipeline){0};}
 if(!device||!desc||!out||flags!=MAG_NATIVE_PROGRAM_STORED_DEPTH_AUX)return MAG_NATIVE_INVALID;
 int status=mag_native_scene_graphics_v3_validate(desc);if(status)return status;
 if(!desc->raster.depth_bounds_enabled||desc->base.binding_model!=1||!desc->base.base.fragment.source_length)return MAG_NATIVE_INVALID;
 for(uint32_t i=0;i<desc->base.base.buffer_count;++i){const mag_native_scene_buffer_layout *b=&desc->base.base.buffers[i];if(b->stage==MAG_NATIVE_SCENE_FRAGMENT&&b->binding.slot==2)return MAG_NATIVE_INVALID;}
 for(uint32_t i=0;i<desc->base.base.texture_count;++i){const mag_native_scene_texture_layout *t=&desc->base.base.textures[i];if(t->stage==MAG_NATIVE_SCENE_FRAGMENT&&t->texture_slot==15)return MAG_NATIVE_INVALID;}
 return device->ops->scene_graphics_create_aux?device->ops->scene_graphics_create_aux(device->context,desc,flags,out):MAG_NATIVE_UNSUPPORTED;
}
