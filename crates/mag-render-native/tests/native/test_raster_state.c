/* Actual shared V3 admission/classification; no device or shader compiler. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "mag_render_scene_validate.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
    const char shader[] = "authored-stage";
    const mag_native_vertex_attribute attribute = {0, MAG_NATIVE_VERTEX_FLOAT4, 0, 0};
    mag_native_scene_graphics_v3_desc d = {0};
    d.struct_size = sizeof(d);
    d.abi_version = 3;
    d.base.struct_size = sizeof(d.base);
    d.base.abi_version = 2;
    d.base.base.struct_size = sizeof(d.base.base);
    d.base.base.abi_version = 1;
    d.base.base.vertex = (mag_native_shader_stage){shader, sizeof(shader) - 1, "main", 4, MAG_NATIVE_MSL};
    d.base.base.fragment = d.base.base.vertex;
    d.base.base.vertex_stride = 16;
    d.base.base.color_format = MAG_NATIVE_SCENE_BGRA8;
    d.base.base.depth_format = MAG_NATIVE_SCENE_D32;
    d.base.attributes = &attribute;
    d.base.attribute_count = 1;
    d.base.binding_model = 1;
    d.raster.known_fields = MAG_NATIVE_RASTER_KNOWN_ALL;
    d.raster.samples = 1;
    d.raster.color_write_mask = 15;
    d.raster.cull_mode = MAG_NATIVE_CULL_BACK;
    d.raster.front_face = MAG_NATIVE_FRONT_CW;
    d.raster.depth_enabled = 1;
    d.raster.depth_write = 1;
    d.raster.depth_compare = MAG_NATIVE_COMPARE_GEQUAL;
    d.raster.blend_enabled = 1;
    d.raster.src_rgb = MAG_NATIVE_BLEND_SRC_ALPHA;
    d.raster.dst_rgb = MAG_NATIVE_BLEND_INV_SRC_ALPHA;
    d.raster.src_alpha = MAG_NATIVE_BLEND_SRC_ALPHA;
    d.raster.dst_alpha = MAG_NATIVE_BLEND_INV_SRC_ALPHA;
    assert(mag_native_scene_graphics_v3_validate(&d) == MAG_NATIVE_OK);
    mag_native_scene_graphics_desc schema;
    mag_native_scene_graphics_v3_schema(&d, &schema);
    assert(schema.depth_write == 1 && schema.depth_compare == MAG_NATIVE_SCENE_DEPTH_ALWAYS);
    assert(schema.blend ==
           MAG_NATIVE_SCENE_ALPHA); /* admission classification only; V3 retains exact factors */
    assert(d.raster.src_alpha == MAG_NATIVE_BLEND_SRC_ALPHA);
    d.base.base.blend = 1;
    assert(mag_native_scene_graphics_v3_validate(&d) == MAG_NATIVE_INVALID);
    d.base.base.blend = 0;
    d.raster.known_fields = 0;
    assert(mag_native_scene_graphics_v3_validate(&d) == MAG_NATIVE_UNSUPPORTED);
    d.raster.known_fields = MAG_NATIVE_RASTER_KNOWN_ALL;
    d.raster.stencil_enabled = 1;
    assert(mag_native_scene_graphics_v3_validate(&d) == MAG_NATIVE_UNSUPPORTED);
    d.raster.stencil_enabled = 0;
    d.raster.color_write_mask = 16;
    assert(mag_native_scene_graphics_v3_validate(&d) == MAG_NATIVE_INVALID);
    d.raster.color_write_mask = 7;
    d.raster.blend_constant[0] = NAN;
    assert(mag_native_scene_graphics_v3_validate(&d) == MAG_NATIVE_INVALID);
    d.raster.blend_constant[0] = 0;
    d.raster.depth_enabled = 0;
    mag_native_scene_graphics_v3_schema(&d, &schema);
    assert(schema.depth_compare == 0 && schema.depth_write == 0);
    puts("explicit raster admission passed; GPU execution pending");
    return 0;
}
