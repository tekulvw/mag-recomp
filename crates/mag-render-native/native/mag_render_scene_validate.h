#ifndef MAG_RENDER_SCENE_VALIDATE_H
#define MAG_RENDER_SCENE_VALIDATE_H
#include "mag_render_scene.h"
typedef struct mag_native_scene_buffer_info {
    uint64_t length;
    uint32_t busy, gpu_written, seed_zero;
} mag_native_scene_buffer_info;
typedef struct mag_native_scene_compute_info {
    const mag_native_binding_layout *bindings;
    uint32_t count, threads[3];
} mag_native_scene_compute_info;
typedef struct mag_native_scene_registry {
    void *context;
    int (*buffer)(void *, mag_native_buffer, mag_native_scene_buffer_info *);
    int (*compute)(void *, mag_native_pipeline, mag_native_scene_compute_info *);
    int (*graphics)(void *, mag_native_pipeline, const mag_native_scene_graphics_desc **,uint32_t *binding_model);
    uint64_t uniform_alignment, storage_alignment, max_uniform_bytes, max_storage_bytes;
} mag_native_scene_registry;
typedef struct mag_native_scene_usage {
    uint8_t read[MAG_NATIVE_SCENE_MAX_RESOURCES], write[MAG_NATIVE_SCENE_MAX_RESOURCES];
    uint64_t bytes;
    uint8_t samplers[MAG_NATIVE_MAX_SAMPLERS];
} mag_native_scene_usage;
typedef struct mag_native_scene_registry_v2 {
    const mag_native_scene_registry *base;
    const mag_native_sampler *samplers;
    uint32_t sampler_count;
    int (*sampler)(void *,mag_native_sampler,const mag_native_sampler_state **);
} mag_native_scene_registry_v2;
int mag_native_scene_validate_v2(const mag_native_scene *,const mag_native_scene_registry_v2 *,mag_native_scene_usage *);
/* Pure admission checks used by every native encoder; callbacks only inspect
 * actual generation-qualified native registry entries. No GPU effects. */
int mag_native_scene_graphics_validate(const mag_native_scene_graphics_desc *);
int mag_native_scene_graphics_v2_validate(const mag_native_scene_graphics_v2_desc *);
int mag_native_scene_graphics_v3_validate(const mag_native_scene_graphics_v3_desc *);
void mag_native_scene_graphics_v3_schema(const mag_native_scene_graphics_v3_desc *,mag_native_scene_graphics_desc *);
int mag_native_scene_validate(const mag_native_scene *, const mag_native_scene_registry *,
                              mag_native_scene_usage *);
#endif
