/* Extracted from ps3recomp libs/video/rsx_frontend_internal.h at 6e0fcc2e026cddc02fb94f23050ac8429a03306a.
 * Copyright (c) 2026 Ned Heller / sp00nznet. MIT; see LICENSE.ps3recomp.
 * Frontend-owned implementation; no SDK runtime/header dependency. */
#ifndef MAG_RENDER_NATIVE_INTERNAL_H
#define MAG_RENDER_NATIVE_INTERNAL_H
#include "mag_render_native.h"
#include "mag_render_batch.h"
#include "mag_render_scene_extended.h"
#include "mag_render_stencil.h"
#include "mag_render_imported_image.h"
typedef struct mag_native_ops {
    int (*caps)(void *, mag_native_caps_desc *);
    int (*buffer_create)(void *, mag_native_buffer, const mag_native_buffer_desc *, mag_native_buffer *);
    int (*buffer_release)(void *, mag_native_buffer);
    int (*buffer_read)(void *, mag_native_buffer, uint64_t, void *, uint64_t);
    int (*pipeline_create)(void *, const mag_native_pipeline_desc *, mag_native_pipeline *);
    int (*pipeline_release)(void *, mag_native_pipeline);
    int (*submit)(void *, const mag_native_frame *, mag_native_ticket *);
    int (*poll)(void *, mag_native_event *, uint32_t, uint32_t *);
    int (*close)(void *);
    int (*graphics_create)(void *,const mag_native_graphics_desc *,mag_native_pipeline *);
    int (*submit_batch)(void *,const mag_native_batch *,mag_native_ticket *);
    int (*scene_graphics_create)(void *,const mag_native_scene_graphics_desc *,mag_native_pipeline *);
    int (*submit_scene)(void *,const mag_native_scene *,mag_native_ticket *);
    int (*image_import)(void *,const MagRenderHostImage *,mag_native_image *);
    int (*image_release)(void *,mag_native_image);
    int (*submit_scene_target)(void *,const mag_native_scene *,mag_native_image,mag_native_ticket *);
    int (*submit_scene_frame)(void *,const mag_native_scene *,mag_native_image,const MagRenderHostFrame *,mag_native_ticket *);
    int (*scene_graphics_create_v2)(void *,const mag_native_scene_graphics_v2_desc *,mag_native_pipeline *);
    int (*scene_graphics_create_v3)(void *,const mag_native_scene_graphics_v3_desc *,mag_native_pipeline *);
    int (*sampler_create)(void *,const mag_native_sampler_desc *,mag_native_sampler *);
    int (*sampler_release)(void *,mag_native_sampler);
    int (*submit_scene_v2)(void *,const mag_native_scene_v2 *,const mag_native_image *,const MagRenderHostFrame *,mag_native_ticket *);
    int (*submit_scene_v3)(void *,const mag_native_scene_v3 *,const mag_native_image *,const MagRenderHostFrame *,mag_native_ticket *);
    int (*submit_scene_v4)(void *,const mag_native_scene_v4 *,const mag_native_image *,const MagRenderHostFrame *,mag_native_ticket *);
    int (*image_import_certified)(void *,const mag_native_image_certificate *,mag_native_image *);
    int (*scene_graphics_create_aux)(void *,const mag_native_scene_graphics_v3_desc *,uint32_t,mag_native_pipeline *);
    int (*submit_scene_v5)(void *,const mag_native_scene_v5 *,const mag_native_image *,const MagRenderHostFrame *,mag_native_ticket *);
} mag_native_ops;
mag_native_device *mag_native_wrap(const mag_native_ops *, void *);
int mag_native_close(mag_native_device *);
uint64_t mag_native_next_generation(void);
int mag_native_metal_create(const MagRenderHostOwner *,mag_native_device **);
int mag_native_vulkan_create(const MagRenderHostOwner *,mag_native_device **);
int mag_native_vulkan_create_v2(const MagRenderHostOwnerV2 *,mag_native_device **);
int mag_native_d3d12_create(const MagRenderHostOwner *,mag_native_device **);
#endif
