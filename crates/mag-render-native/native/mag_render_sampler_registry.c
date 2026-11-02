#include "mag_render_sampler_registry.h"
#include <stdlib.h>
#include <string.h>
void mag_native_sampler_registry_init(mag_native_sampler_registry *r, uint64_t generation, void *userdata,
                                      void (*destroy)(void *, void *)) {
    memset(r, 0, sizeof(*r));
    r->generation = generation;
    r->userdata = userdata;
    r->destroy = destroy;
}
static mag_native_sampler_entry **locate(mag_native_sampler_registry *r, mag_native_sampler h) {
    if (!r || h.device_generation != r->generation || !h.id)
        return NULL;
    mag_native_sampler_entry **p = &r->entries;
    while (*p) {
        if ((*p)->handle.id == h.id)
            return p;
        p = &(*p)->next;
    }
    return NULL;
}
mag_native_sampler_entry *mag_native_sampler_registry_find(mag_native_sampler_registry *r,
                                                           mag_native_sampler h) {
    mag_native_sampler_entry **p = locate(r, h);
    return p && (*p)->public_ref ? *p : NULL;
}
static void collect(mag_native_sampler_registry *r, mag_native_sampler_entry **p) {
    mag_native_sampler_entry *e = *p;
    if (e->public_ref || e->pending_refs)
        return;
    *p = e->next;
    if (r->destroy)
        r->destroy(r->userdata, e->payload);
    free(e);
    --r->count;
}
int mag_native_sampler_registry_insert(mag_native_sampler_registry *r, const mag_native_sampler_state *s,
                                       void *payload, mag_native_sampler *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!r || !r->generation || !r->destroy || !payload || !out)
        return MAG_NATIVE_INVALID;
    int status = mag_native_sampler_state_validate(s);
    if (status)
        return status;
    if (r->count >= MAG_NATIVE_MAX_SAMPLERS || r->next_id == UINT64_MAX)
        return MAG_NATIVE_ALLOCATION_FAILED;
    mag_native_sampler_entry *e = calloc(1, sizeof(*e));
    if (!e)
        return MAG_NATIVE_ALLOCATION_FAILED;
    e->handle = (mag_native_sampler){r->generation, ++r->next_id};
    e->state = *s;
    e->payload = payload;
    e->public_ref = 1;
    e->next = r->entries;
    r->entries = e;
    ++r->count;
    *out = e->handle;
    return MAG_NATIVE_OK;
}
int mag_native_sampler_registry_release(mag_native_sampler_registry *r, mag_native_sampler h) {
    mag_native_sampler_entry **p = locate(r, h);
    if (!p || !(*p)->public_ref)
        return MAG_NATIVE_STALE;
    (*p)->public_ref = 0;
    collect(r, p);
    return MAG_NATIVE_OK;
}
int mag_native_sampler_registry_pin(mag_native_sampler_registry *r, mag_native_sampler h,
                                    mag_native_sampler_entry **out) {
    if (!out)
        return MAG_NATIVE_INVALID;
    *out = NULL;
    mag_native_sampler_entry *e = mag_native_sampler_registry_find(r, h);
    if (!e)
        return MAG_NATIVE_STALE;
    if (e->pending_refs == UINT32_MAX)
        return MAG_NATIVE_ALLOCATION_FAILED;
    ++e->pending_refs;
    *out = e;
    return MAG_NATIVE_OK;
}
int mag_native_sampler_registry_unpin(mag_native_sampler_registry *r, mag_native_sampler_entry *e) {
    if (!r || !e)
        return MAG_NATIVE_INVALID;
    mag_native_sampler_entry **p = &r->entries;
    while (*p && *p != e)
        p = &(*p)->next;
    if (!*p || !(*p)->pending_refs)
        return MAG_NATIVE_STALE;
    --(*p)->pending_refs;
    collect(r, p);
    return MAG_NATIVE_OK;
}
int mag_native_sampler_registry_close(mag_native_sampler_registry *r) {
    if (!r)
        return MAG_NATIVE_INVALID;
    for (mag_native_sampler_entry *e = r->entries; e; e = e->next)
        if (e->pending_refs)
            return MAG_NATIVE_BUSY;
    while (r->entries) {
        r->entries->public_ref = 0;
        collect(r, &r->entries);
    }
    return MAG_NATIVE_OK;
}
