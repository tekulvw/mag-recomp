/* Real shared V4 admission: independent plane history and source assertions. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "mag_render_scene_validate.h"
#include <assert.h>
#include <stdio.h>
static mag_native_raster_state raster;
static mag_native_scene_graphics_desc graphics;
static int buffer(void *c, mag_native_buffer b,
                  mag_native_scene_buffer_info *o) {
  (void)c;
  if (b.device_generation != 7)
    return MAG_NATIVE_STALE;
  *o = (mag_native_scene_buffer_info){48, 0, 0, 0};
  return 0;
}
static int compute(void *c, mag_native_pipeline p,
                   mag_native_scene_compute_info *o) {
  (void)c;
  (void)p;
  (void)o;
  return MAG_NATIVE_STALE;
}
static int graphic(void *c, mag_native_pipeline p,
                   const mag_native_scene_graphics_desc **o, uint32_t *m) {
  (void)c;
  if (p.device_generation != 7)
    return MAG_NATIVE_STALE;
  *o = &graphics;
  *m = 1;
  return 0;
}
static int state(void *c, mag_native_pipeline p,
                 const mag_native_raster_state **o) {
  (void)c;
  if (p.device_generation != 7)
    return MAG_NATIVE_STALE;
  *o = &raster;
  return 0;
}
int main(void) {
  const char code[] = "authored";
  graphics = (mag_native_scene_graphics_desc){
      .struct_size = sizeof(graphics),
      .abi_version = 1,
      .vertex = {code, 8, "main", 4, MAG_NATIVE_MSL},
      .vertex_stride = 16,
      .depth_format = MAG_NATIVE_SCENE_D24S8};
  raster.known_fields = MAG_NATIVE_RASTER_KNOWN_ALL;
  raster.samples = 1;
  raster.stencil_enabled = 1;
  raster.front_stencil = raster.back_stencil =
      (mag_native_stencil_face){.compare = MAG_NATIVE_COMPARE_ALWAYS,
                                .fail = 2,
                                .depth_fail = 2,
                                .pass = 2,
                                .reference = 0x81,
                                .write_mask = 255};
  mag_native_batch_resource resource = {{7, 1, 1}, MAG_NATIVE_UPLOAD, 0};
  mag_native_scene_draw draw = {.pipeline = {7, 1},
                                .vertices = {7, 1, 1},
                                .vertex_length = 48,
                                .indices = {7, 1, 1},
                                .index_length = 12,
                                .index_count = 3};
  mag_native_scene_image image = {MAG_NATIVE_SCENE_D24S8, 4, 4, 0, {0}};
  mag_native_scene_image_layout layout = {2, 1, 1, 1, 1, 0, NULL, 0, 0};
  mag_native_scene_pass passes[2] = {0};
  mag_native_stencil_attachment stencils[2] = {0};
  mag_native_viewport viewports[2] = {{0, 0, 4, 4, 0, 1, 0, 0, 4, 4, 7, 0},
                                      {0, 0, 4, 4, 0, 1, 0, 0, 4, 4, 7, 0}};
  for (unsigned i = 0; i < 2; ++i) {
    passes[i].color.image = MAG_NATIVE_SCENE_NONE;
    passes[i].depth = (mag_native_scene_attachment){
        0, MAG_NATIVE_SCENE_CLEAR, MAG_NATIVE_SCENE_STORE, 0, {1, 0, 0, 0}};
    stencils[i] = (mag_native_stencil_attachment){
        0, MAG_NATIVE_STENCIL_CLEAR, MAG_NATIVE_SCENE_STORE, 3, 0, 0};
  }
  passes[1].draws = &draw;
  passes[1].draw_count = 1;
  mag_native_scene scene = {.struct_size = sizeof(scene),
                            .abi_version = 1,
                            .resources = &resource,
                            .resource_count = 1,
                            .images = &image,
                            .image_count = 1,
                            .passes = passes,
                            .pass_count = 2,
                            .output_image = MAG_NATIVE_SCENE_NONE};
  mag_native_scene_v4 v = {.struct_size = sizeof(v),
                           .abi_version = 4,
                           .base = {.struct_size = sizeof(v.base),
                                    .abi_version = 3,
                                    .base = {.struct_size = sizeof(v.base.base),
                                             .abi_version = 2,
                                             .scene = &scene},
                                    .image_layouts = &layout,
                                    .image_layout_count = 1,
                                    .viewports = viewports,
                                    .viewport_count = 2},
                           .stencils = stencils,
                           .stencil_count = 2};
  mag_native_scene_registry registry = {NULL, buffer, compute,    graphic,
                                        4,    4,      UINT32_MAX, UINT32_MAX};
  mag_native_scene_registry_v2 registry2 = {.base = &registry};
  mag_native_scene_registry_v4 registry4 = {&registry2, state};
  mag_native_scene_usage usage;
  assert(mag_native_scene_validate_v4(&v, &registry4, &usage) ==
         0); /* genuine clear-only planes */
  stencils[1].load = MAG_NATIVE_STENCIL_LOAD;
  passes[1].depth.load = MAG_NATIVE_SCENE_LOAD;
  assert(mag_native_scene_validate_v4(&v, &registry4, &usage) == 0);
  stencils[0].load = MAG_NATIVE_STENCIL_DISCARD_LOAD;
  assert(mag_native_scene_validate_v4(&v, &registry4, &usage) ==
         MAG_NATIVE_INVALID); /* discard/store is not initialization */
  passes[0].draws = &draw;
  passes[0].draw_count = 1;
  assert(mag_native_scene_validate_v4(&v, &registry4, &usage) ==
         MAG_NATIVE_INVALID); /* triangle is not full coverage proof */
  stencils[0].flags = MAG_NATIVE_STENCIL_SOURCE_FULL_WRITE;
  assert(mag_native_scene_validate_v4(&v, &registry4, &usage) == 0);
  raster.front_stencil.write_mask = 127;
  assert(mag_native_scene_validate_v4(&v, &registry4, &usage) ==
         MAG_NATIVE_INVALID);
  raster.front_stencil.write_mask = 255;
  viewports[0].scissor_width = 3;
  assert(mag_native_scene_validate_v4(&v, &registry4, &usage) ==
         MAG_NATIVE_INVALID);
  viewports[0].scissor_width = 4;
  raster.front_stencil.pass = 3;
  assert(mag_native_scene_validate_v4(&v, &registry4, &usage) ==
         MAG_NATIVE_INVALID);
  raster.front_stencil.pass = 2;
  stencils[0].flags = 0;
  stencils[0].load = MAG_NATIVE_STENCIL_CLEAR;
  passes[0].draw_count = 0;
  passes[0].depth.load = MAG_NATIVE_SCENE_DISCARD_LOAD;
  assert(mag_native_scene_validate_v4(&v, &registry4, &usage) ==
         MAG_NATIVE_INVALID); /* stencil clear must not initialize depth */
  passes[1].depth.load = MAG_NATIVE_SCENE_DISCARD_LOAD;
  passes[1].draws = &draw;
  passes[1].draw_count = 1;
  raster.depth_enabled = 1;
  raster.depth_compare = MAG_NATIVE_COMPARE_LESS;
  assert(mag_native_scene_validate_v4(&v, &registry4, &usage) ==
         MAG_NATIVE_INVALID); /* read discarded depth in same pass */
  raster.depth_enabled = 0;
  assert(mag_native_scene_validate_v4(&v, &registry4, &usage) == 0);
  assert(mag_native_scene_validate_v3(&v.base, &registry2, &usage) ==
         MAG_NATIVE_UNSUPPORTED);
  image.upload = resource.buffer;
  layout.subresource_count = 1;
  mag_native_image_subresource sub = {0, 0, 4, 4, 1, 16, 0, 64};
  layout.subresources = &sub;
  assert(mag_native_scene_validate_v4(&v, &registry4, &usage) ==
         MAG_NATIVE_UNSUPPORTED);
  puts("PASS: independent stencil/depth history, source-qualified full writes, "
       "legacy/upload refusal");
}
