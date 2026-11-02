#ifdef NDEBUG
#undef NDEBUG
#endif
#include "mag_render_sampler_registry.h"
#include <assert.h>
#include <stdio.h>
static void destroyed(void *userdata, void *payload) {
    assert(userdata == payload);
    ++*(unsigned *)userdata;
}
int main(void) {
    unsigned freed_a = 0, freed_b = 0;
    mag_native_sampler_registry a, b;
    mag_native_sampler_registry_init(&a, 11, &freed_a, destroyed);
    mag_native_sampler_registry_init(&b, 12, &freed_b, destroyed);
    mag_native_sampler_state s = {0};
    s.known_fields = 63;
    s.max_anisotropy = 1;
    s.lod_max = 13;
    s.wrap_s = 1;
    s.mip_filter = 2;
    mag_native_sampler ah, bh;
    assert(mag_native_sampler_registry_insert(&a, &s, &freed_a, &ah) == 0);
    assert(mag_native_sampler_registry_insert(&b, &s, &freed_b, &bh) == 0);
    assert(ah.id == bh.id);
    assert(!mag_native_sampler_registry_find(&a, bh));
    assert(!mag_native_sampler_registry_find(&b, ah));
    mag_native_sampler_entry *pending = NULL;
    assert(mag_native_sampler_registry_pin(&a, ah, &pending) == 0);
    assert(mag_native_sampler_registry_release(&a, ah) == 0);
    assert(freed_a == 0);
    assert(!mag_native_sampler_registry_find(&a, ah));
    assert(mag_native_sampler_registry_release(&a, ah) == MAG_NATIVE_STALE);
    assert(mag_native_sampler_registry_close(&a) == MAG_NATIVE_BUSY);
    assert(freed_a == 0);
    assert(mag_native_sampler_registry_unpin(&a, pending) == 0);
    assert(freed_a == 1);
    assert(mag_native_sampler_registry_unpin(&a, pending) == MAG_NATIVE_STALE);
    mag_native_sampler anew;
    assert(mag_native_sampler_registry_insert(&a, &s, &freed_a, &anew) == 0);
    assert(anew.id > ah.id);
    assert(!mag_native_sampler_registry_find(&a, ah));
    assert(mag_native_sampler_registry_close(&a) == 0);
    assert(freed_a == 2);
    assert(mag_native_sampler_registry_close(&b) == 0);
    assert(freed_b == 1);
    unsigned before = freed_a;
    for (unsigned i = 0; i < MAG_NATIVE_MAX_SAMPLERS; ++i) {
        mag_native_sampler h;
        assert(mag_native_sampler_registry_insert(&a, &s, &freed_a, &h) == 0);
    }
    mag_native_sampler rejected = {99, 99};
    uint64_t next = a.next_id;
    assert(mag_native_sampler_registry_insert(&a, &s, &freed_a, &rejected) == MAG_NATIVE_ALLOCATION_FAILED);
    assert(!rejected.id && !rejected.device_generation && a.count == MAG_NATIVE_MAX_SAMPLERS &&
           a.next_id == next);
    assert(mag_native_sampler_registry_close(&a) == 0);
    assert(freed_a == before + MAG_NATIVE_MAX_SAMPLERS);
    puts("sampler generation, pending retention, stale/double release and bounded shutdown passed");
    return 0;
}
