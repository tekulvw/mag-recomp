#ifndef MAG_RENDER_BATCH_H
#define MAG_RENDER_BATCH_H
#include "mag_render_native.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MAG_NATIVE_BATCH_ABI 1u
#define MAG_NATIVE_BATCH_MAX_RESOURCES 64u
#define MAG_NATIVE_BATCH_MAX_COMMANDS 32u
#define MAG_NATIVE_BATCH_MAX_BYTES (64u * 1024u * 1024u)
/* Separate CPU seed identity from produced data. Uploads cannot be written.
 * Produced allocations have one writer in this ordered batch; reading before
 * that writer, repeated writer or any same-node read/write alias is invalid.
 * READBACK only receives the final initialized attachment copy. */
enum mag_native_resource_role { MAG_NATIVE_UPLOAD = 0, MAG_NATIVE_PRODUCED = 1, MAG_NATIVE_READBACK = 2 };
typedef struct mag_native_batch_resource {
    mag_native_buffer buffer;
    uint32_t role, reserved;
} mag_native_batch_resource;
typedef struct mag_native_shader_stage {
    const void *source;
    uint64_t source_length;
    const char *entry;
    uint32_t entry_length, language;
} mag_native_shader_stage;
/* First opaque path: Float32x4 at location0, stride16; uint32 triangle indices;
 * RGBA8Unorm and D32Float, one sample, depth LESS/write, no blending/culling.
 * Unsupported layouts/stages/formats are rejected rather than guessed. */
typedef struct mag_native_graphics_desc {
    uint32_t struct_size, abi_version;
    mag_native_shader_stage vertex, fragment;
    uint32_t vertex_stride, reserved;
} mag_native_graphics_desc;
int mag_native_batch_layout(uint32_t *out, uint32_t words);
int mag_native_graphics_create(mag_native_device *, const mag_native_graphics_desc *, mag_native_pipeline *);
typedef struct mag_native_indexed_draw {
    mag_native_pipeline pipeline;
    mag_native_buffer vertices, indices, color_readback; /* all-zero = no diagnostic copy/staging */
    uint64_t vertex_offset, vertex_length, index_offset, index_length;
    uint32_t index_count, width, height, reserved;
    float clear_color[4], clear_depth;
    uint32_t reserved1;
} mag_native_indexed_draw;
enum mag_native_command_kind { MAG_NATIVE_DISPATCH = 1, MAG_NATIVE_INDEXED_DRAW = 2 };
typedef struct mag_native_batch_command {
    uint32_t kind, reserved;
    const mag_native_frame *dispatch;
    const mag_native_indexed_draw *draw;
} mag_native_batch_command;
typedef struct mag_native_batch {
    uint32_t struct_size, abi_version;
    const mag_native_batch_resource *resources;
    uint32_t resource_count, command_count;
    const mag_native_batch_command *commands;
} mag_native_batch;
/* Synchronously validates and reserves the whole batch before any commit.
 * One command buffer/list on the retained owner's queue, at most2 pending.
 * No host polling/readback between nodes; dependency barriers are native.
 * Negative = definitely uncommitted. 0/1 retain ticket until genuine terminal. */
int mag_native_submit_batch(mag_native_device *, const mag_native_batch *, mag_native_ticket *);
#ifdef __cplusplus
}
#endif
#endif
