#ifndef MAG_RENDER_SAMPLER_H
#define MAG_RENDER_SAMPLER_H
#include "mag_render_native.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MAG_NATIVE_SAMPLER_ABI 1u
#define MAG_NATIVE_MAX_SAMPLERS 1024u
#define MAG_NATIVE_SAMPLER_KNOWN_ALL 63ull
/* Immutable registry identity. Every use carries both fields; a local sampler
 * ID from another context must never alias an equal numeric ID in this one. */
typedef struct mag_native_sampler {
    uint64_t device_generation, id;
} mag_native_sampler;
enum mag_native_sampler_filter { MAG_NATIVE_FILTER_NEAREST = 0, MAG_NATIVE_FILTER_LINEAR = 1 };
enum mag_native_sampler_mip_filter {
    MAG_NATIVE_MIP_NONE = 0,
    MAG_NATIVE_MIP_NEAREST = 1,
    MAG_NATIVE_MIP_LINEAR = 2
};
enum mag_native_sampler_wrap {
    MAG_NATIVE_WRAP_CLAMP_EDGE = 0,
    MAG_NATIVE_WRAP_REPEAT = 1,
    MAG_NATIVE_WRAP_MIRROR = 2,
    MAG_NATIVE_WRAP_BORDER = 3
};
/* Known groups: filters1, wraps2, comparison4, LOD8, anisotropy16, border32.
 * Compare uses mag_native_raster_compare. LOD bias is preserved but rejected
 * where the selected backend cannot represent it without shader integration. */
typedef struct mag_native_sampler_state {
    uint64_t known_fields;
    uint32_t min_filter, mag_filter, mip_filter, wrap_s, wrap_t, wrap_r, comparison_enabled, comparison;
    float lod_min, lod_max, lod_bias, max_anisotropy, border[4];
} mag_native_sampler_state;
typedef struct mag_native_sampler_desc {
    uint32_t struct_size, abi_version;
    mag_native_sampler_state state;
} mag_native_sampler_desc;
int mag_native_sampler_state_validate(const mag_native_sampler_state *);
int mag_native_sampler_create(mag_native_device *, const mag_native_sampler_desc *, mag_native_sampler *);
int mag_native_sampler_release(mag_native_device *, mag_native_sampler);
int mag_native_sampler_layout(uint32_t *, uint32_t);
#ifdef __cplusplus
}
#endif
#endif
