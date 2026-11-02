#ifndef MAG_RENDER_CLIENT_H
#define MAG_RENDER_CLIENT_H
#include <stdint.h>
#include "mag_render_scene.h"
#include "mag_render_host_frame.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Actual scene ingress, not a synthetic probe. Handles are non-reused and
 * confined to their creating thread. At most two pending/undelivered terminal
 * results. All pointers are valid, aligned synchronous ranges; bytes are copied
 * before return. No host pointer/guest memory is retained as scene input.
 * Initial position-only rendering is DIAGNOSTIC, not material/removal/FPS
 * acceptance. Caller supplies an actual shader descriptor and source-qualified
 * native clip positions; no guessed viewport/matrix conventions are applied. */
#define MAG_RENDER_CLIENT_ABI 1u
#define MAG_RENDER_CLIENT_CLIP_FLOAT4 1u
#define MAG_RENDER_CLIENT_MAX_DRAWS 16384u
#define MAG_RENDER_CLIENT_DIAGNOSTIC 1u
#define MAG_RENDER_CLIENT_UNCERTAIN 2u
#define MAG_RENDER_CLIENT_TERMINAL 4u
#define MAG_RENDER_CLIENT_FAILED 8u
typedef struct MagRenderClientSource {
 uint64_t engine_update,cohort,publication,pass_occurrence;
} MagRenderClientSource;
typedef struct MagRenderClientDraw {
 uint32_t abi_version,struct_size;
 uint64_t source_item,source_partition;
 const float (*positions)[4];const uint16_t *indices;
 uint32_t vertex_count,index_count,position_space,reserved;
 uint64_t unresolved_obligations;
} MagRenderClientDraw;
typedef struct MagRenderClientInput {
 uint32_t abi_version,struct_size;MagRenderClientSource source;
 uint64_t intended_items;const MagRenderClientDraw *draws;uint32_t draw_count,reserved;
 const MagRenderHostImage *target;const MagRenderHostFrame *frame;
 float clear_color[4],clear_depth;uint32_t reserved1;
} MagRenderClientInput;
typedef struct MagRenderClientReceipt {
 uint32_t abi_version,struct_size;int32_t status;uint32_t flags;
 MagRenderClientSource source;
 uint64_t ticket,native_generation,native_ticket,intended_items,admitted_items,omitted_items,draws,unresolved_obligations;
} MagRenderClientReceipt;
typedef struct MagRenderClientDrawIdentity {
 uint64_t source_item,source_partition,unresolved_obligations;
} MagRenderClientDrawIdentity;
/* max_bytes includes uploads and attachments, up to64MiB. Native pipelines
 * currently require Float4 vertex input, explicit RGBA8/BGRA8+D32, and no external bindings.
 * This constraint is explicit; omitted source materials are never called parity. */
/* Build-time Naga-translated unlit stages with process-lived pointers. No
 * previous build directory or runtime shader generator is needed. This does not
 * assert platform pipeline creation or game material/lighting parity. */
int mag_render_client_diagnostic_pipeline(uint32_t backend,mag_native_scene_graphics_desc*);
int mag_render_client_create(const MagRenderHostOwner*,const mag_native_scene_graphics_desc*,uint32_t max_draws,uint64_t max_bytes,uint64_t *session);
/* Return0accepted,1indeterminate: never replay either. Negative = definitely
 * uncommitted. A genuine target reservation is mandatory. The caller retains
 * cancellation responsibility for preparation failures before native submit.
 * Source item ordinals index the entire intended population, not filtered draws.
 * Duplicate items/partitions preserve actual input ordering. */
int mag_render_client_submit(uint64_t,const MagRenderClientInput*,MagRenderClientReceipt*);
/* -4 means no terminal receipt yet, never synthetic success. Physical present
 * is a distinct owner event and is not inferred from native completion. */
int mag_render_client_poll(uint64_t,MagRenderClientReceipt*);
/* Query before consuming terminal receipt; no partial copying on small capacity. */
int mag_render_client_draw_identities(uint64_t,uint64_t,MagRenderClientDrawIdentity*,uint32_t,uint32_t*);
/* -4 preserves a live session with pending or undelivered terminal results. */
int mag_render_client_close(uint64_t);
/* Fixed frontend worker for migrating FIFO consumers. Create uses the embedded
 * diagnostic stage provider. All NativeContext/session work stays on one worker
 * thread. All geometry is copied before enqueue; submit waits for CPU queue-
 * commit ACK only, never GPU completion. Host retain/release/try_begin/end and
 * image retain/release must be genuine, process-lived, cross-thread callable,
 * and must not acquire the original FIFO consumer lease or wait for GPU work.
 * Caller owns the real frame reservation through ACK; do not cancel concurrently.
 * Negative ACK leaves cancellation to the caller on its consumer/lifecycle thread
 * (native precommit refusal may already cancel it). Never replay/cancel positive
 * indeterminate ACK. Max2 workers; concurrent operations return BUSY.
 * Poll checks native terminal receipts; physical present uses host frame events
 * separately, outside FIFO callback. Close refuses pending/undelivered results;
 * it never calls SDK shutdown-retry or waits for a GPU fence. */
int mag_render_client_worker_create(const MagRenderHostOwner*,uint32_t max_draws,uint64_t max_bytes,uint64_t *worker);
/* Exact imported host target format:RGBA8=1/BGRA8=3. Existing create usesRGBA8. */
int mag_render_client_worker_create_for_format(const MagRenderHostOwner*,uint32_t target_format,uint32_t max_draws,uint64_t max_bytes,uint64_t *worker);
int mag_render_client_worker_submit(uint64_t,const MagRenderClientInput*,MagRenderClientReceipt*);
int mag_render_client_worker_poll(uint64_t,MagRenderClientReceipt*);
int mag_render_client_worker_close(uint64_t);
#ifdef __cplusplus
}
#endif
#endif
