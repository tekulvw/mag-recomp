/* Real pure preflight: an invalid vertex view must fail before PSO creation. */
#include "mag_render_scene_validate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    const char shader[] = "authored-shader";
    mag_native_vertex_attribute attributes[] = {{0, MAG_NATIVE_VERTEX_FLOAT3, 0, 0},
                                                {3, MAG_NATIVE_VERTEX_FLOAT2, 12, 0},
                                                {6, MAG_NATIVE_VERTEX_UNORM8X4, 20, 0}};
    mag_native_scene_graphics_v2_desc d = {0};
    d.struct_size = sizeof(d);
    d.abi_version = 2;
    d.base.struct_size = sizeof(d.base);
    d.base.abi_version = 1;
    d.base.vertex = (mag_native_shader_stage){shader, sizeof(shader) - 1, "main", 4, MAG_NATIVE_MSL};
    d.base.fragment = d.base.vertex;
    d.base.vertex_stride = 24;
    d.base.color_format = MAG_NATIVE_SCENE_RGBA8;
    d.base.depth_format = MAG_NATIVE_SCENE_D32;
    d.base.depth_compare = MAG_NATIVE_SCENE_DEPTH_LEQUAL;
    d.base.depth_write = 1;
    d.attributes = attributes;
    d.attribute_count = 3;
    assert(mag_native_scene_graphics_v2_validate(&d) == 0);
    d.base.color_format=MAG_NATIVE_SCENE_BGRA8;assert(mag_native_scene_graphics_v2_validate(&d)==0);
    d.base.color_format=MAG_NATIVE_SCENE_RGBA8;
    assert(mag_native_scene_graphics_validate(&d.base) ==
           MAG_NATIVE_INVALID); /* ABI1 defaults not silently widened */
    attributes[1].location = 0;
    assert(mag_native_scene_graphics_v2_validate(&d) == MAG_NATIVE_INVALID);
    attributes[1].location = 3;
    attributes[1].offset = 8;
    assert(mag_native_scene_graphics_v2_validate(&d) == MAG_NATIVE_INVALID);
    attributes[1].offset = 12; /* overlap */
    attributes[0].offset = 2;
    assert(mag_native_scene_graphics_v2_validate(&d) == MAG_NATIVE_INVALID);
    attributes[0].offset = 0;
    d.base.vertex_stride = 20;
    assert(mag_native_scene_graphics_v2_validate(&d) == MAG_NATIVE_INVALID);
    d.base.vertex_stride = 24;
    attributes[2].format = 99;
    assert(mag_native_scene_graphics_v2_validate(&d) == MAG_NATIVE_INVALID);
    attributes[2].format = MAG_NATIVE_VERTEX_UNORM8X4;
    d.attribute_count = 0;
    assert(mag_native_scene_graphics_v2_validate(&d) == MAG_NATIVE_INVALID);
    d.attribute_count = 3;
    mag_native_scene_buffer_layout buffer={.stage=MAG_NATIVE_SCENE_FRAGMENT,.binding={.slot=0,.access=MAG_NATIVE_READ,.minimum_bytes=16,.runtime_size_index=MAG_NATIVE_NO_SIZE_INDEX,.alignment_bytes=4,.binding_kind=MAG_NATIVE_UNIFORM}};
    mag_native_scene_texture_layout texture={MAG_NATIVE_SCENE_FRAGMENT,0,0,MAG_NATIVE_SCENE_RGBA8,0,0};
    d.base.buffers=&buffer;d.base.buffer_count=1;d.base.textures=&texture;d.base.texture_count=1;
    d.binding_model=1;assert(mag_native_scene_graphics_v2_validate(&d)==0); /* independent b0/t0/s0 */
    d.binding_model=0;assert(mag_native_scene_graphics_v2_validate(&d)==MAG_NATIVE_INVALID);
    d.binding_model=99;assert(mag_native_scene_graphics_v2_validate(&d)==MAG_NATIVE_INVALID);
    puts("PASS: explicit Float3/UV/color layout; duplicate location, overlapping fields, bad alignment, "
         "short stride and unknown formats refuse before PSO creation");
    return 0;
}
