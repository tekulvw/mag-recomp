#ifndef MAG_RENDER_RASTER_H
#define MAG_RENDER_RASTER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Canonical effective state, supplied explicitly by the source adapter. Native
 * descriptors do not recover missing inherited state or infer material modes. */
enum mag_native_raster_compare {
    MAG_NATIVE_COMPARE_NEVER = 0,
    MAG_NATIVE_COMPARE_LESS = 1,
    MAG_NATIVE_COMPARE_EQUAL = 2,
    MAG_NATIVE_COMPARE_LEQUAL = 3,
    MAG_NATIVE_COMPARE_GREATER = 4,
    MAG_NATIVE_COMPARE_NOTEQUAL = 5,
    MAG_NATIVE_COMPARE_GEQUAL = 6,
    MAG_NATIVE_COMPARE_ALWAYS = 7
};
enum mag_native_raster_blend_factor {
    MAG_NATIVE_BLEND_ZERO = 0,
    MAG_NATIVE_BLEND_ONE = 1,
    MAG_NATIVE_BLEND_SRC_COLOR = 2,
    MAG_NATIVE_BLEND_INV_SRC_COLOR = 3,
    MAG_NATIVE_BLEND_DST_COLOR = 4,
    MAG_NATIVE_BLEND_INV_DST_COLOR = 5,
    MAG_NATIVE_BLEND_SRC_ALPHA = 6,
    MAG_NATIVE_BLEND_INV_SRC_ALPHA = 7,
    MAG_NATIVE_BLEND_DST_ALPHA = 8,
    MAG_NATIVE_BLEND_INV_DST_ALPHA = 9,
    MAG_NATIVE_BLEND_CONST_COLOR = 10,
    MAG_NATIVE_BLEND_INV_CONST_COLOR = 11,
    MAG_NATIVE_BLEND_CONST_ALPHA = 12,
    MAG_NATIVE_BLEND_INV_CONST_ALPHA = 13,
    MAG_NATIVE_BLEND_SRC_ALPHA_SAT = 14
};
enum mag_native_raster_blend_equation {
    MAG_NATIVE_BLEND_ADD = 0,
    MAG_NATIVE_BLEND_SUBTRACT = 1,
    MAG_NATIVE_BLEND_REVERSE_SUBTRACT = 2,
    MAG_NATIVE_BLEND_MIN = 3,
    MAG_NATIVE_BLEND_MAX = 4
};
enum mag_native_raster_cull {
    MAG_NATIVE_CULL_NONE = 0,
    MAG_NATIVE_CULL_FRONT = 1,
    MAG_NATIVE_CULL_BACK = 2,
    MAG_NATIVE_CULL_BOTH = 3
};
enum mag_native_raster_winding { MAG_NATIVE_FRONT_CCW = 0, MAG_NATIVE_FRONT_CW = 1 };
#define MAG_NATIVE_RASTER_KNOWN_ALL 511ull
/* Stencil fields remain explicit even when unavailable on the selected depth
 * format. Unsupported state is rejected, not discarded. */
typedef struct mag_native_stencil_face {
    uint32_t compare, fail, depth_fail, pass, reference, read_mask, write_mask, reserved;
} mag_native_stencil_face;
typedef struct mag_native_raster_state {
    uint64_t known_fields, unresolved_obligations;
    uint32_t cull_mode, front_face, depth_enabled, depth_compare, depth_write, blend_enabled;
    uint32_t src_rgb, dst_rgb, rgb_equation, src_alpha, dst_alpha, alpha_equation, color_write_mask,
        stencil_enabled;
    float blend_constant[4];
    mag_native_stencil_face front_stencil, back_stencil;
    uint32_t depth_bounds_enabled, bias_enabled, topology, samples;
    float depth_bounds_min, depth_bounds_max, bias_constant, bias_slope, bias_clamp;
    uint32_t reserved;
} mag_native_raster_state;
/* Pure range/known-state admission. Current cross-backend subset supports all
 * compare operations, independent RGB/alpha factors/equations, color masks,
 * front/back cull, explicit winding and range-checked stored-depth bounds.
 * Bounds require an explicit backend execution mode. Depth bias (constant
 * plus slope, no clamp) is supported; a nonzero bias clamp, nontriangle
 * topology, multisample and cull-both remain explicit UNSUPPORTED. */
int mag_native_raster_validate(const mag_native_raster_state *);
#ifdef __cplusplus
}
#endif
#endif
