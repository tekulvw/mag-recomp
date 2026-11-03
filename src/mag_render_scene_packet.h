#ifndef MAG_RENDER_SCENE_PACKET_H
#define MAG_RENDER_SCENE_PACKET_H
/* Program.reserved bit: checked Metal FS injected depth t15 / bounds b2. */
#define MAG_RENDER_PROGRAM_STORED_DEPTH_AUX 1u
#include "mag_render_client.h"
#include "mag_render_viewport.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Production transport. Packet indices are local references, never guest
 * addresses/allocation generations or native handles. Every source/entry/data
 * range is copied synchronously before enqueue. Target/frame remain genuine
 * provider metadata through commit ACK. Source shader code is already lowered;
 * worker owns native PSO/cache/submission only, not a second compiler/context.
 * Unsupported/unknown effective state refuses before queue effects. No scalar
 * default below is permission to infer an unobserved game state. */
#define MAG_RENDER_SCENE_PACKET_ABI 1u
#define MAG_RENDER_PACKET_NONE UINT32_MAX
#define MAG_RENDER_PACKET_DIAGNOSTIC_PARTIAL 1u
#define MAG_RENDER_PACKET_MAX_PROGRAMS 1024u
#define MAG_RENDER_PACKET_MAX_PIPELINE_ENTRIES 1024u
#define MAG_RENDER_PACKET_MAX_CACHED_PIPELINES 256u
#define MAG_RENDER_PACKET_MAX_SHADER_KEY_BYTES (32ull * 1024ull * 1024ull)
#define MAG_RENDER_PACKET_MAX_VIEWS 4096u
#define MAG_RENDER_PACKET_MAX_SAMPLERS 4096u
/* Same fixed source/reflection limits as native graphics. Actual configured
 * packet/image/resource budgets are queried separately; no truncation.
 * Pipeline entries may deduplicate, but a packet needing more distinct PSOs
 * than MAX_CACHED_PIPELINES is refused as a whole. Worker byte limits are lower
 * configurable retained/copy budgets (legacy diagnostic maximum64MiB; scene-only workers permit native
 * max_bytes), not max_bytes returned by the native aggregate-capacity query. */
enum MagRenderPacketStage {
    MAG_RENDER_PACKET_VERTEX = 0,
    MAG_RENDER_PACKET_FRAGMENT = 1,
    MAG_RENDER_PACKET_COMPUTE = 2
};
enum MagRenderPacketDimension {
    MAG_RENDER_PACKET_D1 = 1,
    MAG_RENDER_PACKET_D2 = 2,
    MAG_RENDER_PACKET_D3 = 3,
    MAG_RENDER_PACKET_CUBE = 4
};
enum MagRenderPacketSampleKind {
    MAG_RENDER_PACKET_FLOAT = 0,
    MAG_RENDER_PACKET_SINT = 1,
    MAG_RENDER_PACKET_UINT = 2,
    MAG_RENDER_PACKET_DEPTH = 3
};
typedef struct MagRenderPacketTextureSlot {
    uint32_t slot, dimension, sample_kind, arrayed, multisampled, reserved;
} MagRenderPacketTextureSlot;
typedef struct MagRenderPacketSamplerSlot {
    uint32_t slot, comparison;
} MagRenderPacketSamplerSlot;
typedef struct MagRenderPacketProgram {
    /* reserved is now the named PROGRAM_* flag word; unknown bits reject. */
    uint32_t stage, reserved;
    mag_native_shader_stage shader;
    const mag_native_binding_layout *buffers;
    uint32_t buffer_count, texture_count;
    const MagRenderPacketTextureSlot *textures;
    const MagRenderPacketSamplerSlot *samplers;
    uint32_t sampler_count, vertex_location_count;
    const uint32_t *vertex_locations;
    uint32_t threads[3], reserved1;
} MagRenderPacketProgram;
/* Canonical compare0..7: NEVER,LESS,EQUAL,LEQUAL,GREATER,NOTEQUAL,GEQUAL,ALWAYS.
 * Blend factors0..14: ZERO,ONE,SRC_COLOR,INV_SRC_COLOR,DST_COLOR,INV_DST_COLOR,
 * SRC_ALPHA,INV_SRC_ALPHA,DST_ALPHA,INV_DST_ALPHA,CONST_COLOR,INV_CONST_COLOR,
 * CONST_ALPHA,INV_CONST_ALPHA,SRC_ALPHA_SATURATE. Equations0..4:
 * ADD,SUBTRACT,REVERSE_SUBTRACT,MIN,MAX. Stencil ops0..7:
 * KEEP,ZERO,REPLACE,INCR_CLAMP,DECR_CLAMP,INVERT,INCR_WRAP,DECR_WRAP. */
enum MagRenderPacketKnownState {
    MAG_RENDER_PACKET_KNOWN_CULL = 1u,
    MAG_RENDER_PACKET_KNOWN_DEPTH = 2u,
    MAG_RENDER_PACKET_KNOWN_BLEND = 4u,
    MAG_RENDER_PACKET_KNOWN_COLOR_MASK = 8u,
    MAG_RENDER_PACKET_KNOWN_STENCIL = 16u,
    MAG_RENDER_PACKET_KNOWN_DEPTH_BOUNDS = 32u,
    MAG_RENDER_PACKET_KNOWN_BIAS = 64u,
    MAG_RENDER_PACKET_KNOWN_TOPOLOGY = 128u,
    MAG_RENDER_PACKET_KNOWN_SAMPLES = 256u
};
typedef struct MagRenderPacketStencilFace {
    uint32_t compare, fail, depth_fail, pass, reference, read_mask, write_mask, reserved;
} MagRenderPacketStencilFace;
typedef struct MagRenderPacketRasterState {
    uint64_t known_fields, unresolved_obligations;
    uint32_t cull_mode, front_face, depth_enabled, depth_compare, depth_write, blend_enabled;
    uint32_t src_rgb, dst_rgb, rgb_equation, src_alpha, dst_alpha, alpha_equation, color_write_mask,
        stencil_enabled;
    float blend_constant[4];
    MagRenderPacketStencilFace front_stencil, back_stencil;
    uint32_t depth_bounds_enabled, bias_enabled, topology, samples;
    float depth_bounds_min, depth_bounds_max, bias_constant, bias_slope, bias_clamp;
    uint32_t reserved;
    /* cull0NONE/1FRONT/2BACK/3BOTH, front0CCW/1CW. topology0TRIANGLES,
     *1STRIP/2FAN/3LINES/4POINTS. Prepared canonical expansion must be explicit. */
} MagRenderPacketRasterState;
typedef struct MagRenderPacketPipeline {
    uint32_t vertex_program, fragment_program, vertex_stride, attribute_count;
    const mag_native_vertex_attribute *attributes;
    uint32_t color_format, depth_format, binding_model, reserved;
    const mag_native_scene_texture_layout *textures;
    uint32_t texture_count, reserved1;
    MagRenderPacketRasterState state;
} MagRenderPacketPipeline;
enum MagRenderPacketBufferRole { MAG_RENDER_PACKET_UPLOAD = 0, MAG_RENDER_PACKET_PRODUCED = 1 };
typedef struct MagRenderPacketBuffer {
    uint32_t role, reserved;
    const void *bytes;
    uint64_t length;
} MagRenderPacketBuffer;
/* Canonical owned subresources. Format is explicit; compressed bits are not
 * relabelled asRGBA. Unsupported shape/format/mips refuses without normalization.
 * Each mip/face/layer is separately bounded and its byte extent validated. */
typedef struct MagRenderPacketSubresource {
    uint32_t mip, layer, width, height, depth, row_bytes;
    const void *bytes;
    uint64_t length;
} MagRenderPacketSubresource;
typedef struct MagRenderPacketImage {
    uint32_t format, dimension, width, height, depth, layers, mip_count, samples;
    const MagRenderPacketSubresource *subresources;
    uint32_t subresource_count, reserved;
} MagRenderPacketImage;
/* component mapping0ZERO/1ONE/2R/3G/4B/5A. No inferred channel remap. */
typedef struct MagRenderPacketImageView {
    uint32_t image, base_mip, mip_count, base_layer, layer_count, remap[4], reserved;
} MagRenderPacketImageView;
/* Known sampler groups: filters=1, wraps=2, comparison=4, LOD=8,
 * anisotropy=16, border=32. All six groups must be explicitly known. */
#define MAG_RENDER_PACKET_SAMPLER_KNOWN_ALL 63ull
typedef struct MagRenderPacketSampler {
    uint64_t known_fields;
    uint32_t min_filter, mag_filter, mip_filter, wrap_s, wrap_t, wrap_r, comparison_enabled, comparison;
    float lod_min, lod_max, lod_bias, max_anisotropy, border[4];
    /* filters0NEAREST/1LINEAR, mip0NONE/1NEAREST/2LINEAR;
     * wrap0CLAMP_EDGE/1REPEAT/2MIRROR_REPEAT/3CLAMP_BORDER. */
} MagRenderPacketSampler;
typedef struct MagRenderPacketBufferUse {
    uint32_t buffer, reserved;
    uint64_t offset, length;
} MagRenderPacketBufferUse;
typedef struct MagRenderPacketBinding {
    uint32_t stage, slot;
    MagRenderPacketBufferUse view;
} MagRenderPacketBinding;
typedef struct MagRenderPacketTextureUse {
    uint32_t stage, texture_slot, sampler_slot, view, sampler, reserved;
} MagRenderPacketTextureUse;
/* Index bytes are canonical native-endian uint32, never raw guest uint16.
 * source_item is the source occurrence ordinal; repeated underlying objects
 * keep distinct ordinals. No population truncation or partition deduplication. */
typedef struct MagRenderPacketDraw {
    uint64_t source_item, source_partition, unresolved_obligations;
    uint32_t pipeline, index_count;
    MagRenderPacketBufferUse vertices, indices;
    const MagRenderPacketBinding *buffers;
    uint32_t buffer_count, texture_count;
    const MagRenderPacketTextureUse *textures;
} MagRenderPacketDraw;
typedef struct MagRenderPacketDispatch {
    uint32_t program, binding_count;
    const MagRenderPacketBinding *bindings;
    uint32_t groups[3], reserved;
} MagRenderPacketDispatch;
typedef struct MagRenderPacketPass {
    MagRenderClientSource source;
    mag_native_scene_attachment color, depth;
    const MagRenderPacketDraw *draws;
    uint32_t draw_count, reserved;
} MagRenderPacketPass;
/* One entry for every source item/pass occurrence, in actual source order.
 * Explicit omissions remain represented and cannot qualify as removed work. */
typedef struct MagRenderPacketItem {
    uint32_t pass, reserved;
    uint64_t source_item, unresolved_obligations;
} MagRenderPacketItem;
typedef struct MagRenderScenePacketDesc {
    uint32_t abi_version, struct_size, flags, reserved;
    MagRenderClientSource source;
    uint64_t intended_items;
    const MagRenderPacketProgram *programs;
    uint32_t program_count, pipeline_count;
    const MagRenderPacketPipeline *pipelines;
    const MagRenderPacketBuffer *buffers;
    uint32_t buffer_count, image_count;
    const MagRenderPacketImage *images;
    const MagRenderPacketImageView *views;
    uint32_t view_count, sampler_count;
    const MagRenderPacketSampler *samplers;
    const MagRenderPacketDispatch *dispatches;
    uint32_t dispatch_count, pass_count;
    const MagRenderPacketPass *passes;
    const MagRenderPacketItem *items;
    uint32_t item_count, output_image;
    const MagRenderHostImage *target;
    const MagRenderHostFrame *frame;
} MagRenderScenePacketDesc;
/* Additive V2 supplies one explicit source viewport per pass and one explicit
 * view dimension (2=2D,4=cube) per base view. Base records remain ABI1-sized.
 * View remap numbering remains packet ZERO0/ONE1/R2/G3/B4/A5. */
typedef struct MagRenderScenePacketV2Desc {
    uint32_t abi_version, struct_size;
    MagRenderScenePacketDesc base;
    const mag_native_viewport *viewports;
    uint32_t viewport_count, reserved;
    const uint32_t *view_dimensions;
    uint32_t view_dimension_count, reserved1;
} MagRenderScenePacketV2Desc;
/* ABI3 adds a complete per-pass independent stencil plane table. Existing
 * color/depth load2 means Discard only through this entry. Discard+store never
 * implies initialized contents. FULL_WRITE requires actual producer coverage
 * qualification, plus native necessary raster and viewport checks. */
#define MAG_RENDER_PACKET_STENCIL_SOURCE_FULL_WRITE 1u
typedef struct MagRenderPacketStencilAttachment {uint32_t image,load,store,clear_value,flags,reserved;} MagRenderPacketStencilAttachment;
typedef struct MagRenderScenePacketV3Desc {uint32_t abi_version,struct_size;MagRenderScenePacketV2Desc base;const MagRenderPacketStencilAttachment *stencils;uint32_t stencil_count,reserved;} MagRenderScenePacketV3Desc;
int mag_render_client_worker_submit_scene_v3(uint64_t,const MagRenderScenePacketV3Desc*,MagRenderClientReceipt*);
/* Target-independent CPU preparation. A token pins owned typed commands,
 * input snapshots and native PSOs on this worker. It is NOT a GPU ticket,
 * original-job completion publication or permission to suppress any source job.
 * Prepare rejects partial-diagnostic packets. target/frame in the input must be
 * NULL; later commit supplies genuine metadata and the exact source tuple.
 * Prepared+pending+unread-terminal records share the existing bound of two. */
#define MAG_RENDER_SCENE_ADMISSION_CPU_PREPARED 1u
typedef struct MagRenderSceneAdmission {
 uint32_t abi_version,struct_size;uint64_t worker,token,native_generation;
 MagRenderClientSource source;uint64_t intended_items,draws;uint32_t flags,reserved;
} MagRenderSceneAdmission;
int mag_render_client_worker_prepare_scene_v3(uint64_t,const MagRenderScenePacketV3Desc*,MagRenderSceneAdmission*);
/* Definite rejection leaves preparation cancellable/retryable. Accepted or
 * indeterminate consumes the token; never replay originals after uncertainty.
 * Native/GPU allocation failure remains possible at commit. */
int mag_render_client_worker_commit_scene(uint64_t,const MagRenderSceneAdmission*,const MagRenderClientSource*,const MagRenderHostImage*,const MagRenderHostFrame*,MagRenderClientReceipt*);
/* Precommit only. Unknown/consumed/foreign/stale tokens return STALE. */
int mag_render_client_worker_cancel_scene(uint64_t,const MagRenderSceneAdmission*);
/* Late-bound existing GPU allocations. source_key is opaque and scoped by the
 * Admission Source tuple; it is not an EA, native handle or invented guest
 * generation. Base image format/extents are exact physical requirements.
 * No retained HostImage is needed at prepare. Planes are COLOR1/DEPTH2/STENCIL4;
 * zero means allocation-only (write destination), not initialized contents. */
typedef struct MagRenderPacketImageRequirement {uint32_t image,required_initialized_planes;uint64_t source_key;} MagRenderPacketImageRequirement;
typedef int (*MagRenderImageValidatePlanes)(void*,uint64_t,uint64_t,uint64_t,uint32_t);
typedef struct MagRenderPacketImageBinding {
 uint32_t image,reserved;uint64_t source_key;MagRenderHostImage host;
 /* Called only under actual native owner lease; reuses host userdata/identity.
  * Must validate real allocation and source-qualified ordered initialization.
  * HostImage's existing retain/release lifetime covers this callback metadata. */
 MagRenderImageValidatePlanes validate_planes;
} MagRenderPacketImageBinding;
int mag_render_client_worker_prepare_scene_imported_v3(uint64_t,const MagRenderScenePacketV3Desc*,const MagRenderPacketImageRequirement*,uint32_t,MagRenderSceneAdmission*);
/* Genuine queue subgraph commit. No final target/frame/presentation assertion:
 * source-selected read/write surfaces are all explicit late-bound imports.
 * Original prior producer work must be ordered at the genuine native boundary.
 * Exact table/source matching precedes GPU commitment. */
int mag_render_client_worker_commit_scene_subgraph(uint64_t,const MagRenderSceneAdmission*,const MagRenderClientSource*,const MagRenderPacketImageBinding*,uint32_t,MagRenderClientReceipt*);
/* Uses the SAME persistent worker/session/SceneDevice and genuine ticket path.
 * Negative is definitely uncommitted; caller owns early frame cancellation.
 * Positive indeterminate must not be replayed. Physical present remains separate.
 * Old diagnostic submit API is unchanged. */
typedef struct MagRenderScenePacketLimits {
    uint64_t max_resources, max_images, max_passes, max_draws, max_bytes;
} MagRenderScenePacketLimits;
/* Frontend aggregate budgets, not hardware maxima; workers can configure lower
 * retained byte/draw budgets. Native definitions are the source of truth. */
int mag_render_scene_packet_limits(MagRenderScenePacketLimits *);
/* Scene-only actor creation does not compile a diagnostic bootstrap pipeline or
 * assume RGBA/BGRA target formats. Enabled-device facts require genuine V2 owner. */
int mag_render_client_worker_create_scene(const MagRenderHostOwner *, uint32_t max_draws, uint64_t max_bytes,
                                          uint64_t *);
int mag_render_client_worker_create_scene_v2(const MagRenderHostOwnerV2 *, uint32_t max_draws,
                                             uint64_t max_bytes, uint64_t *);
int mag_render_client_worker_submit_scene_v2(uint64_t, const MagRenderScenePacketV2Desc *,
                                             MagRenderClientReceipt *);
int mag_render_client_worker_submit_scene(uint64_t worker, const MagRenderScenePacketDesc *,
                                          MagRenderClientReceipt *);
#ifdef __cplusplus
}
#endif
#endif
