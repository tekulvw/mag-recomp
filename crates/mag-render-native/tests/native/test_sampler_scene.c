#ifdef NDEBUG
#undef NDEBUG
#endif
/* Actual full-scene admission resolves complete sampler generations, including
 * unused table entries, before any backend allocation or queue operation. */
#define main sampler_fixture_main
#include "test_bgra_scene.c"
#undef main
#include "mag_render_sampler_registry.h"
static void discard(void *u, void *p) {
    (void)u;
    (void)p;
}
static int sampler_info(void *context, mag_native_sampler h, const mag_native_sampler_state **out) {
    mag_native_sampler_entry *e = mag_native_sampler_registry_find(context, h);
    if (!e)
        return MAG_NATIVE_STALE;
    *out = &e->state;
    return 0;
}
int main(void) {
    mag_native_sampler_registry a, b;
    mag_native_sampler_registry_init(&a, 7, NULL, discard);
    mag_native_sampler_registry_init(&b, 8, NULL, discard);
    mag_native_sampler_state state = {0};
    state.known_fields = 63;
    state.max_anisotropy = 1;
    state.lod_max = 13;
    state.wrap_s = 1;
    state.mip_filter = 2;
    mag_native_sampler handles[2], foreign;
    assert(mag_native_sampler_registry_insert(&a, &state, &a, &handles[0]) == 0);
    assert(mag_native_sampler_registry_insert(&b, &state, &b, &foreign) == 0);
    assert(handles[0].id == foreign.id);
    mag_native_scene_texture_layout texture = {1, 0, 0, 1, 0, 0};
    graphics = (mag_native_scene_graphics_desc){.struct_size = sizeof(graphics),
                                                .abi_version = 1,
                                                .vertex_stride = 16,
                                                .color_format = 3,
                                                .fragment = {.source_length = 1},
                                                .textures = &texture,
                                                .texture_count = 1};
    graphics_model = 1;
    const mag_native_buffer vertices = {7, 1, 1}, indices = {7, 2, 1};
    mag_native_batch_resource resources[] = {{vertices, MAG_NATIVE_UPLOAD, 0},
                                             {indices, MAG_NATIVE_UPLOAD, 0}};
    mag_native_scene_image images[] = {{3, 8, 8, 0, {0}}, {1, 8, 8, 0, {0}}};
    mag_native_scene_texture_binding binding = {1, 0, 0, 1, MAG_NATIVE_SCENE_SAMPLER_TABLE_BIT, 0};
    mag_native_scene_draw draw = {.pipeline = {7, 1},
                                  .vertices = vertices,
                                  .indices = indices,
                                  .vertex_length = 48,
                                  .index_length = 12,
                                  .index_count = 3,
                                  .texture_count = 1,
                                  .textures = &binding};
    mag_native_scene_pass passes[] = {
        {.color = {1, 0, 0, 0, {1, 0, 0, 1}}, .depth = {.image = MAG_NATIVE_SCENE_NONE}},
        {.color = {0, 0, 0, 0, {0, 0, 0, 1}},
         .depth = {.image = MAG_NATIVE_SCENE_NONE},
         .draws = &draw,
         .draw_count = 1}};
    mag_native_scene scene = {.struct_size = sizeof(scene),
                              .abi_version = 1,
                              .resources = resources,
                              .resource_count = 2,
                              .image_count = 2,
                              .images = images,
                              .passes = passes,
                              .pass_count = 2,
                              .output_image = 0};
    mag_native_scene_registry registry = {&a, buffer_info, compute_info, graphics_info,
                                          4,  4,           UINT32_MAX,   UINT32_MAX};
    mag_native_scene_registry_v2 v2 = {&registry, handles, 1, sampler_info};
    mag_native_scene_usage usage;
    assert(mag_native_scene_validate_v2(&scene, &v2, &usage) == 0);
    assert(usage.samplers[0]);
    assert(mag_native_scene_validate(&scene, &registry, &usage) == MAG_NATIVE_INVALID);
    handles[1] = handles[0];
    handles[0] = foreign;
    assert(mag_native_scene_validate_v2(&scene, &v2, &usage) == MAG_NATIVE_STALE);
    handles[0] = handles[1];
    handles[1] = foreign;
    v2.sampler_count = 2;
    assert(mag_native_scene_validate_v2(&scene, &v2, &usage) ==
           MAG_NATIVE_STALE); /* unused foreign ref still invalid */
    v2.sampler_count = 1;
    binding.sampler = MAG_NATIVE_SCENE_SAMPLER_TABLE_BIT | 1;
    assert(mag_native_scene_validate_v2(&scene, &v2, &usage) == MAG_NATIVE_INVALID);
    binding.sampler = MAG_NATIVE_SCENE_SAMPLER_TABLE_BIT;
    assert(mag_native_sampler_registry_release(&a, handles[0]) == 0);
    assert(mag_native_scene_validate_v2(&scene, &v2, &usage) == MAG_NATIVE_STALE);
    assert(mag_native_sampler_registry_close(&a) == 0 && mag_native_sampler_registry_close(&b) == 0);
    puts("PASS: complete-scene sampler tables reject foreign, stale, missing and unused foreign handles");
    return 0;
}
