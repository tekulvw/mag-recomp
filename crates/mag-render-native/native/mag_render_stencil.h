#ifndef MAG_RENDER_STENCIL_H
#define MAG_RENDER_STENCIL_H
#include "mag_render_scene_extended.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Explicit independent plane operations. Discard is accepted only by V4;
 * storing discarded contents never itself proves initialized pixels. */
enum mag_native_stencil_load {
  MAG_NATIVE_STENCIL_CLEAR = 0,
  MAG_NATIVE_STENCIL_LOAD = 1,
  MAG_NATIVE_STENCIL_DISCARD_LOAD = 2
};
#define MAG_NATIVE_STENCIL_SOURCE_FULL_WRITE 1u
typedef struct mag_native_stencil_attachment {
  uint32_t image, load, store, clear_value, flags, reserved;
} mag_native_stencil_attachment;
typedef struct mag_native_scene_v4 {
  uint32_t struct_size, abi_version;
  mag_native_scene_v3 base;
  const mag_native_stencil_attachment *stencils;
  uint32_t stencil_count, reserved;
} mag_native_scene_v4;
/* SOURCE_FULL_WRITE is a source-qualified producer assertion, never inferred
 * from arbitrary draws. Native admission also checks full viewport/scissor,
 * full-byte writes and non-prior-dependent ZERO/REPLACE operations. This flag
 * is not original-work removal or visual-acceptance evidence by itself. */
int mag_native_submit_scene_v4(mag_native_device *, const mag_native_scene_v4 *,
                               const mag_native_image *,
                               const MagRenderHostFrame *, mag_native_ticket *);
int mag_native_scene_v4_layout(uint32_t *, uint32_t);
#ifdef __cplusplus
}
#endif
#endif
