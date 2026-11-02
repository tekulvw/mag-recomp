#ifndef MAG_RENDER_SAMPLER_REGISTRY_H
#define MAG_RENDER_SAMPLER_REGISTRY_H
#include "mag_render_sampler.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Context-local, caller-serialized lifetime registry. IDs never reuse, while
 * payload destruction waits for public release and all genuine pending pins. */
typedef struct mag_native_sampler_entry {
    mag_native_sampler handle;
    mag_native_sampler_state state;
    void *payload;
    uint32_t public_ref, pending_refs;
    struct mag_native_sampler_entry *next;
} mag_native_sampler_entry;
typedef struct mag_native_sampler_registry {
    uint64_t generation, next_id;
    uint32_t count;
    mag_native_sampler_entry *entries;
    void *userdata;
    void (*destroy)(void *, void *);
} mag_native_sampler_registry;
void mag_native_sampler_registry_init(mag_native_sampler_registry *, uint64_t, void *,
                                      void (*)(void *, void *));
int mag_native_sampler_registry_insert(mag_native_sampler_registry *, const mag_native_sampler_state *,
                                       void *, mag_native_sampler *);
mag_native_sampler_entry *mag_native_sampler_registry_find(mag_native_sampler_registry *, mag_native_sampler);
int mag_native_sampler_registry_release(mag_native_sampler_registry *, mag_native_sampler);
int mag_native_sampler_registry_pin(mag_native_sampler_registry *, mag_native_sampler,
                                    mag_native_sampler_entry **);
int mag_native_sampler_registry_unpin(mag_native_sampler_registry *, mag_native_sampler_entry *);
int mag_native_sampler_registry_close(mag_native_sampler_registry *);
#ifdef __cplusplus
}
#endif
#endif
