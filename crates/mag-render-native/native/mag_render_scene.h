#ifndef MAG_RENDER_SCENE_H
#define MAG_RENDER_SCENE_H
#include "mag_render_batch.h"
#include "mag_render_raster.h"
#include "mag_render_sampler.h"
#include "mag_render_host_image.h"
#include "mag_render_host_frame.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Additive scene ABI. Batch ABI1 remains independent and supported. */
#define MAG_NATIVE_SCENE_ABI 1u
#define MAG_NATIVE_SCENE_MAX_RESOURCES 4096u
#define MAG_NATIVE_SCENE_MAX_BYTES (1024ull * 1024ull * 1024ull)
#define MAG_NATIVE_SCENE_MAX_IMAGES 1024u
#define MAG_NATIVE_SCENE_MAX_PASSES 16u
#define MAG_NATIVE_SCENE_MAX_DRAWS 16384u
#define MAG_NATIVE_SCENE_NONE UINT32_MAX
/* Formats are explicit. Texture uploads are tightly packed RGBA8/BGRA8 as declared; no guessed
 * guest layout, implicit mip chain, color conversion, or original GPU state. */
enum mag_native_scene_format {
    MAG_NATIVE_SCENE_NO_FORMAT = 0,
    MAG_NATIVE_SCENE_RGBA8 = 1,
    MAG_NATIVE_SCENE_D32 = 2,
    MAG_NATIVE_SCENE_BGRA8 = 3
};
enum mag_native_scene_stage { MAG_NATIVE_SCENE_VERTEX = 0, MAG_NATIVE_SCENE_FRAGMENT = 1 };
enum mag_native_scene_compare {
    MAG_NATIVE_SCENE_DEPTH_DISABLED = 0,
    MAG_NATIVE_SCENE_DEPTH_LESS = 1,
    MAG_NATIVE_SCENE_DEPTH_LEQUAL = 2,
    MAG_NATIVE_SCENE_DEPTH_ALWAYS = 3
};
enum mag_native_scene_blend { MAG_NATIVE_SCENE_OPAQUE = 0, MAG_NATIVE_SCENE_ALPHA = 1 };
enum mag_native_scene_sampler {
    MAG_NATIVE_SCENE_NEAREST = 0,
    MAG_NATIVE_SCENE_LINEAR = 1,
    MAG_NATIVE_SCENE_COMPARE_LEQUAL = 2
};
typedef struct mag_native_scene_buffer_layout {
    uint32_t stage, reserved;
    mag_native_binding_layout binding;
} mag_native_scene_buffer_layout;
typedef struct mag_native_scene_texture_layout {
    uint32_t stage, texture_slot, sampler_slot, format, comparison, reserved;
} mag_native_scene_texture_layout;
typedef struct mag_native_scene_graphics_desc {
    uint32_t struct_size, abi_version;
    mag_native_shader_stage vertex, fragment; /* fragment optional for depth-only; cutout may use it */
    uint32_t vertex_stride, color_format, depth_format, depth_compare, depth_write, blend;
    const mag_native_scene_buffer_layout *buffers;
    uint32_t buffer_count, texture_count;
    const mag_native_scene_texture_layout *textures;
} mag_native_scene_graphics_desc;
/* Explicit prepared vertex bytes; no guest packing or attribute defaults are
 * inferred. ABI1 remains Float4 LOC0/stride16. V2 accepts one interleaved stream
 * (Metal slot16, Vulkan/D3D stream0); shader locations map to MSL attributes/SPIR-V
 * locations/HLSL LOC semantic indices identically. */
enum mag_native_scene_binding_model { MAG_NATIVE_SCENE_BINDINGS_COMBINED=0, MAG_NATIVE_SCENE_BINDINGS_NAMESPACED=1 };
enum mag_native_vertex_format {
    MAG_NATIVE_VERTEX_FLOAT=1, MAG_NATIVE_VERTEX_FLOAT2=2,
    MAG_NATIVE_VERTEX_FLOAT3=3, MAG_NATIVE_VERTEX_FLOAT4=4,
    MAG_NATIVE_VERTEX_UNORM8X4=5
};
typedef struct mag_native_vertex_attribute {
    uint32_t location,format,offset,reserved;
} mag_native_vertex_attribute;
typedef struct mag_native_scene_graphics_v2_desc {
    uint32_t struct_size,abi_version;
    mag_native_scene_graphics_desc base;
    const mag_native_vertex_attribute *attributes;
    uint32_t attribute_count,binding_model;
    /* model0: legacy combined slots. model1: independent bN/tN/sN (0..15).
     * Vulkan set0vertex/set1fragment, bufferN/texture16+N/sampler32+N;
     * Metal indices remain N. D3D model1: uniforms bN, readonly storage
     * buffers t32+N, textures tN and samplers sN; register space and visibility
     * are stage-qualified (space0 vertex, space1 fragment). */
} mag_native_scene_graphics_v2_desc;
/* V3 keeps explicit raster state authoritative. The legacy depth_compare,
 * depth_write and blend fields in base.base must be zero. The native registry
 * derives only the legacy admission classification; actual PSOs use raster. */
typedef struct mag_native_scene_graphics_v3_desc {
    uint32_t struct_size,abi_version;
    mag_native_scene_graphics_v2_desc base;
    mag_native_raster_state raster;
} mag_native_scene_graphics_v3_desc;
int mag_native_scene_graphics_create_v3(mag_native_device *,const mag_native_scene_graphics_v3_desc *,mag_native_pipeline *);
int mag_native_scene_graphics_v3_layout(uint32_t *,uint32_t);
int mag_native_scene_graphics_create_v2(mag_native_device *,const mag_native_scene_graphics_v2_desc *,mag_native_pipeline *);
int mag_native_scene_graphics_v2_layout(uint32_t *,uint32_t);
typedef struct mag_native_scene_image {
    uint32_t format, width, height, reserved;
    mag_native_buffer upload; /* zero=fresh attachment; nonzero=immutable CPU RGBA seed */
} mag_native_scene_image;
typedef struct mag_native_scene_buffer_binding {
    uint32_t stage, reserved;
    mag_native_binding binding;
} mag_native_scene_buffer_binding;
typedef struct mag_native_scene_texture_binding {
    uint32_t stage, texture_slot, sampler_slot, image, sampler, reserved;
} mag_native_scene_texture_binding;
typedef struct mag_native_scene_draw {
    mag_native_pipeline pipeline;
    mag_native_buffer vertices, indices;
    uint64_t vertex_offset, vertex_length, index_offset, index_length;
    uint32_t index_count, buffer_count, texture_count, reserved;
    const mag_native_scene_buffer_binding *buffers;
    const mag_native_scene_texture_binding *textures;
} mag_native_scene_draw;
/* Load reads the immediately preceding stored contents of this same image.
 * Clear initializes all pixels even when no primitive covers them. Discard
 * invalidates all future reads/loads until a later clear. */
enum mag_native_scene_load { MAG_NATIVE_SCENE_CLEAR = 0, MAG_NATIVE_SCENE_LOAD = 1, MAG_NATIVE_SCENE_DISCARD_LOAD = 2 };
enum mag_native_scene_store { MAG_NATIVE_SCENE_STORE = 0, MAG_NATIVE_SCENE_DISCARD = 1 };
typedef struct mag_native_scene_attachment {
    uint32_t image, load, store, reserved;
    float clear[4];
} mag_native_scene_attachment;
typedef struct mag_native_scene_pass {
    mag_native_scene_attachment color, depth;
    const mag_native_scene_draw *draws;
    uint32_t draw_count, reserved;
} mag_native_scene_pass;
typedef struct mag_native_scene {
    uint32_t struct_size, abi_version;
    const mag_native_batch_resource *resources;
    uint32_t resource_count, image_count;
    const mag_native_scene_image *images;
    const mag_native_frame *dispatches;
    uint32_t dispatch_count, pass_count;
    const mag_native_scene_pass *passes;
    uint32_t output_image, reserved;
    mag_native_buffer diagnostic_readback; /* zero=no copy; otherwise tight bytes in output format (RGBA/BGRA) */
} mag_native_scene;
/* Shallow V2 submission envelope. Old texture.sampler values0..2 remain the
 * exact presets. High-bit codes index this full generation-qualified table;
 * they are never interpreted as bare registry IDs. All table handles are
 * validated before any queue effects, including otherwise unused entries. */
#define MAG_NATIVE_SCENE_SAMPLER_TABLE_BIT 0x80000000u
#define MAG_NATIVE_SCENE_SAMPLER_TABLE_INDEX(code) ((code)&0x7fffffffu)
typedef struct mag_native_scene_v2 {
    uint32_t struct_size,abi_version;
    const mag_native_scene *scene;
    const mag_native_sampler *samplers;
    uint32_t sampler_count,reserved;
} mag_native_scene_v2;
/* target==NULL for offscreen; frame requires a genuine nonnull target. */
int mag_native_submit_scene_v2(mag_native_device *,const mag_native_scene_v2 *,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *);
/* Synchronous complete validation + reservation before queue effects. Ordered
 * compute prefix then passes share one native submission/terminal ticket.
 * These methods do not assert that an offscreen image was physically presented. */
int mag_native_image_import(mag_native_device *,const MagRenderHostImage *,mag_native_image *);
int mag_native_image_release(mag_native_device *,mag_native_image);
int mag_native_submit_scene_target(mag_native_device *,const mag_native_scene *,mag_native_image,mag_native_ticket *);
int mag_native_submit_scene_frame(mag_native_device *,const mag_native_scene *,mag_native_image,const MagRenderHostFrame *,mag_native_ticket *);
int mag_native_scene_layout(uint32_t *, uint32_t);
int mag_native_scene_graphics_create(mag_native_device *, const mag_native_scene_graphics_desc *,
                                     mag_native_pipeline *);
int mag_native_submit_scene(mag_native_device *, const mag_native_scene *, mag_native_ticket *);
#ifdef __cplusplus
}
#endif
#endif
