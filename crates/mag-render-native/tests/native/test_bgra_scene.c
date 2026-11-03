/* Actual shared scene admission must distinguish BGRA from RGBA; an imported
 * original BGRA target is never silently rendered through an RGBA PSO. */
#include "mag_render_scene_validate.h"
#include <assert.h>
#include <stdio.h>
static mag_native_scene_graphics_desc graphics;
static uint32_t graphics_model;
static int buffer_info(void *c, mag_native_buffer b, mag_native_scene_buffer_info *out) {
    (void)c;
    if (b.device_generation != 7 || b.version != 1 || b.id < 1 || b.id > 2)
        return MAG_NATIVE_STALE;
    *out = (mag_native_scene_buffer_info){b.id == 1 ? 48 : 12, 0, 0, 0};
    return 0;
}
static int compute_info(void *c, mag_native_pipeline p, mag_native_scene_compute_info *out) {
    (void)c;
    (void)p;
    (void)out;
    return MAG_NATIVE_STALE;
}
static int graphics_info(void *c, mag_native_pipeline p, const mag_native_scene_graphics_desc **out,uint32_t *model) {
    (void)c;
    if (p.device_generation != 7 || p.id != 1)
        return MAG_NATIVE_STALE;
    *out = &graphics;*model=graphics_model;
    return 0;
}
int main(void) {
    graphics = (mag_native_scene_graphics_desc){.struct_size = sizeof(graphics),
                                                .abi_version = 1,
                                                .vertex_stride = 16,
                                                .color_format = MAG_NATIVE_SCENE_BGRA8};
    const mag_native_buffer vertices = {7, 1, 1}, indices = {7, 2, 1};
    mag_native_batch_resource resources[] = {{vertices, MAG_NATIVE_UPLOAD, 0},
                                             {indices, MAG_NATIVE_UPLOAD, 0}};
    mag_native_scene_image image = {MAG_NATIVE_SCENE_BGRA8, 8, 8, 0, {0}};
    mag_native_scene_draw draw = {.pipeline = {7, 1},
                                  .vertices = vertices,
                                  .indices = indices,
                                  .vertex_length = 48,
                                  .index_length = 12,
                                  .index_count = 3};
    mag_native_scene_pass pass = {
        .color = {0, MAG_NATIVE_SCENE_CLEAR, MAG_NATIVE_SCENE_STORE, 0, {1, 0, 0, 1}},
        .depth = {.image = MAG_NATIVE_SCENE_NONE},
        .draws = &draw,
        .draw_count = 1};
    mag_native_scene scene = {.struct_size = sizeof(scene),
                              .abi_version = 1,
                              .resources = resources,
                              .resource_count = 2,
                              .image_count = 1,
                              .images = &image,
                              .pass_count = 1,
                              .passes = &pass,
                              .output_image = 0};
    mag_native_scene_registry registry = {NULL, buffer_info, compute_info, graphics_info,
                                          4,    4,           UINT32_MAX,   UINT32_MAX};
    mag_native_scene_usage usage;
    assert(mag_native_scene_validate(&scene, &registry, &usage) == 0);
    graphics.color_format = MAG_NATIVE_SCENE_RGBA8;
    assert(mag_native_scene_validate(&scene, &registry, &usage) == MAG_NATIVE_INVALID);
    graphics.color_format = MAG_NATIVE_SCENE_BGRA8;
    pass.draws = NULL;
    pass.draw_count = 0;
    scene.resources = NULL;
    scene.resource_count = 0;
    assert(mag_native_scene_validate(&scene, &registry, &usage) == 0); /* genuine BGRA clear-only effects */
    puts("PASS: actual BGRA scene, no-readback output, wrong-PSO refusal and clear-only initialization");
    return 0;
}
