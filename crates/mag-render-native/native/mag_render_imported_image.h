#ifndef MAG_RENDER_IMPORTED_IMAGE_H
#define MAG_RENDER_IMPORTED_IMAGE_H
#include "mag_render_stencil.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MAG_NATIVE_PLANE_COLOR 1u
#define MAG_NATIVE_PLANE_DEPTH 2u
#define MAG_NATIVE_PLANE_STENCIL 4u
typedef int (*mag_native_validate_planes_fn)(void *,uint64_t,uint64_t,uint64_t,uint32_t);
/* Existing HostImage lease owns all metadata/callback lifetime. No second
 * retain/release mechanism is introduced. Validation is under the genuine
 * native owner lease, after prior original producer queue ordering. Plane
 * facts prove initialization only, not source epoch or GPU completion. */
typedef struct mag_native_image_certificate {MagRenderHostImage host;mag_native_validate_planes_fn validate_planes;} mag_native_image_certificate;
typedef struct mag_native_scene_import {uint32_t image,required_initialized_planes;mag_native_image ref;} mag_native_scene_import;
typedef struct mag_native_depth_bounds {uint32_t enabled,reserved;float minimum,maximum;} mag_native_depth_bounds;
typedef struct mag_native_scene_v5 {
 uint32_t struct_size,abi_version;mag_native_scene_v4 base;
 const mag_native_scene_import *imports;uint32_t import_count,reserved;
 /* Optional complete flattened draw table. Min/max are dynamic draw values,
  * never immutable PSO keys. No table means every draw has bounds disabled. */
 const mag_native_depth_bounds *bounds;uint32_t bounds_count,reserved1;
} mag_native_scene_v5;
#define MAG_NATIVE_PROGRAM_STORED_DEPTH_AUX 1u
/* Checked compiler mode, with actual reflected Metal t15 depth + b2 float4.
 * Old V3 constructors cannot infer or activate this behavior. */
int mag_native_scene_graphics_create_aux(mag_native_device *,const mag_native_scene_graphics_v3_desc *,uint32_t fragment_flags,mag_native_pipeline *);
int mag_native_image_import_certified(mag_native_device *,const mag_native_image_certificate *,mag_native_image *);
int mag_native_submit_scene_v5(mag_native_device *,const mag_native_scene_v5 *,const mag_native_image *,const MagRenderHostFrame *,mag_native_ticket *);
/* Reserve the queue position and ticket of the NEXT imported (no target/frame)
 * scene submission under the owner lease, validating exactly these imports
 * there. The next submit_scene_v5 then encodes into that reserved position
 * without the lease, using those validated imports; a failed or abandoned
 * reservation still releases its queue position (never blocks the queue).
 * Backends without reservations return UNSUPPORTED. */
int mag_native_reserve_scene_v5(mag_native_device *,const mag_native_scene_import *,uint32_t,mag_native_ticket *);
int mag_native_abandon_scene_reservation(mag_native_device *);
/* Diagnostic for the opt-in chunked reserved encoding: out[0] chunked
 * submissions so far, out[1] command buffers reserved for the last one and
 * out[2] how many of those carried work. count must be 3. */
int mag_native_scene_chunk_stats(mag_native_device *,uint64_t *out,uint32_t count);
/* Diagnostic GPU memory census; count must be MAG_NATIVE_MEMORY_STATS:
 * out[0] device allocated bytes (Metal currentAllocatedSize), out[1] live
 * buffer versions, out[2] pipelines, out[3] imported images, out[4] resident
 * texture entries, out[5] pending submissions, out[6] samplers, out[7]
 * retired command buffers still alive (finished work something still
 * retains; process-wide scene command buffers alive minus this device's
 * pending ones), out[8] scene submissions. */
#define MAG_NATIVE_MEMORY_STATS 9u
int mag_native_memory_stats(mag_native_device *,uint64_t *out,uint32_t count);
int mag_native_scene_v5_layout(uint32_t *,uint32_t);
#ifdef __cplusplus
}
#endif
#endif
