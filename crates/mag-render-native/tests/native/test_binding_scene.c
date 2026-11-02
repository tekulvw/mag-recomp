/* A source material may legitimately use b0/t0/s0 in one shader stage. Its
 * actual stored pipeline model must survive into complete scene admission. */
#define main bgra_fixture_main
#include "test_bgra_scene.c"
#undef main
int main(void) {
    mag_native_scene_buffer_layout uniform = {.stage = MAG_NATIVE_SCENE_FRAGMENT,
                                              .binding = {.slot = 0,
                                                          .access = MAG_NATIVE_READ,
                                                          .minimum_bytes = 16,
                                                          .runtime_size_index = MAG_NATIVE_NO_SIZE_INDEX,
                                                          .alignment_bytes = 4,
                                                          .binding_kind = MAG_NATIVE_UNIFORM}};
    mag_native_scene_texture_layout texture = {MAG_NATIVE_SCENE_FRAGMENT, 0, 0, MAG_NATIVE_SCENE_RGBA8, 0, 0};
    graphics = (mag_native_scene_graphics_desc){.struct_size = sizeof(graphics),
                                                .abi_version = 1,
                                                .vertex_stride = 16,
                                                .color_format = MAG_NATIVE_SCENE_BGRA8,
                                                .fragment = {.source_length = 1},
                                                .buffers = &uniform,
                                                .buffer_count = 1,
                                                .texture_count = 1,
                                                .textures = &texture};
    const mag_native_buffer vertices = {7, 1, 1}, indices = {7, 2, 1};
    mag_native_batch_resource resources[] = {{vertices, MAG_NATIVE_UPLOAD, 0},
                                             {indices, MAG_NATIVE_UPLOAD, 0}};
    mag_native_scene_image images[] = {{MAG_NATIVE_SCENE_BGRA8, 8, 8, 0, {0}},
                                       {MAG_NATIVE_SCENE_RGBA8, 8, 8, 0, {0}}};
    mag_native_scene_buffer_binding buffer = {.stage = MAG_NATIVE_SCENE_FRAGMENT,
                                              .binding = {.slot = 0, .buffer = vertices, .byte_length = 16}};
    mag_native_scene_texture_binding sampled = {MAG_NATIVE_SCENE_FRAGMENT, 0, 0, 1,
                                                MAG_NATIVE_SCENE_NEAREST,  0};
    mag_native_scene_draw draw = {.pipeline = {7, 1},
                                  .vertices = vertices,
                                  .indices = indices,
                                  .vertex_length = 48,
                                  .index_length = 12,
                                  .index_count = 3,
                                  .buffer_count = 1,
                                  .texture_count = 1,
                                  .buffers = &buffer,
                                  .textures = &sampled};
    mag_native_scene_pass passes[] = {
        {.color = {1, MAG_NATIVE_SCENE_CLEAR, MAG_NATIVE_SCENE_STORE, 0, {1, 0, 0, 1}},
         .depth = {.image = MAG_NATIVE_SCENE_NONE}},
        {.color = {0, MAG_NATIVE_SCENE_CLEAR, MAG_NATIVE_SCENE_STORE, 0, {0, 0, 0, 1}},
         .depth = {.image = MAG_NATIVE_SCENE_NONE},
         .draws = &draw,
         .draw_count = 1}};
    mag_native_scene scene = {.struct_size = sizeof(scene),
                              .abi_version = 1,
                              .resources = resources,
                              .resource_count = 2,
                              .image_count = 2,
                              .images = images,
                              .pass_count = 2,
                              .passes = passes,
                              .output_image = 0};
    mag_native_scene_registry registry = {NULL, buffer_info, compute_info, graphics_info,
                                          4,    4,           UINT32_MAX,   UINT32_MAX};
    mag_native_scene_usage usage;
    graphics_model = 1;
    assert(mag_native_scene_validate(&scene, &registry, &usage) == 0);
    graphics_model = 0;
    assert(mag_native_scene_validate(&scene, &registry, &usage) == MAG_NATIVE_INVALID);
    graphics_model = 2;
    assert(mag_native_scene_validate(&scene, &registry, &usage) == MAG_NATIVE_INVALID);
    puts("PASS: actual stored namespaces permit material b0/t0/s0; legacy/unknown model refuses");
    return 0;
}
