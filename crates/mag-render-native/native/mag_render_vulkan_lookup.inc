/* Context-local optional indices. The ordinary lists remain the sole disposal
 * inventory, including publicly released objects pinned by unfinished work.
 * No index allocation, eviction, changed token, or changed lifetime. */
static uint32_t buffer_lookup_bucket(uint64_t id, uint64_t version) {
    return (uint32_t)((id ^ (version * UINT64_C(0x9e3779b97f4a7c15))) & (FRONTEND_VK_LOOKUP_BUCKETS - 1u));
}
static uint32_t pipeline_lookup_bucket(uint64_t id) {
    return (uint32_t)(id & (FRONTEND_VK_LOOKUP_BUCKETS - 1u));
}
static void lookup_configure(frontend_vk *c) {
    if (c->lookup.configured) return;
    c->lookup.configured = 1;
    const char *index = getenv("MAG_RENDER_VULKAN_LOOKUP_INDEX");
    const char *probes = getenv("MAG_RENDER_VULKAN_LOOKUP_PROBES");
    c->lookup.index_enabled = index && !strcmp(index, "1");
    c->lookup.probes_enabled = probes && !strcmp(probes, "1");
}
static void buffer_publish(frontend_vk *c, frontend_vk_buffer *b) {
    lookup_configure(c);
    b->next = c->buffers; c->buffers = b;
    if (c->lookup.index_enabled) {
        uint32_t bucket = buffer_lookup_bucket(b->id, b->version);
        b->lookup_next = c->lookup.buffers[bucket]; c->lookup.buffers[bucket] = b;
    }
}
static void pipeline_publish(frontend_vk *c, frontend_vk_pipeline *p) {
    lookup_configure(c);
    p->next = c->pipelines; c->pipelines = p;
    if (c->lookup.index_enabled) {
        uint32_t bucket = pipeline_lookup_bucket(p->id);
        p->lookup_next = c->lookup.pipelines[bucket]; c->lookup.pipelines[bucket] = p;
    }
}
static void buffer_unindex(frontend_vk *c, frontend_vk_buffer *b) {
    if (!c->lookup.index_enabled || !b->id || !b->version) return;
    frontend_vk_buffer **link = &c->lookup.buffers[buffer_lookup_bucket(b->id, b->version)];
    while (*link && *link != b) link = &(*link)->lookup_next;
    if (*link) *link = b->lookup_next;
    b->lookup_next = NULL;
}
static void pipeline_unindex(frontend_vk *c, frontend_vk_pipeline *p) {
    if (!c->lookup.index_enabled || !p->id) return;
    frontend_vk_pipeline **link = &c->lookup.pipelines[pipeline_lookup_bucket(p->id)];
    while (*link && *link != p) link = &(*link)->lookup_next;
    if (*link) *link = p->lookup_next;
    p->lookup_next = NULL;
}
static void lookup_record(frontend_vk_lookup_counts *counts, uint64_t probes, int found) {
    ++counts->calls; counts->probes += probes; counts->misses += !found;
    if (probes > counts->maximum) counts->maximum = probes;
}
/* Diagnostic branches are separate so a normal lookup has no per-node counter
 * increment or diagnostic condition. All entries run on the client thread. */
static frontend_vk_buffer *buffer_find_probed(frontend_vk *c, mag_native_buffer ref) {
    frontend_vk_buffer *found = NULL; uint64_t probes = 0;
    if (ref.device_generation == c->generation && ref.id && ref.version) {
        frontend_vk_buffer *b = c->lookup.index_enabled
            ? c->lookup.buffers[buffer_lookup_bucket(ref.id, ref.version)] : c->buffers;
        for (; b; b = c->lookup.index_enabled ? b->lookup_next : b->next) {
            ++probes;
            if (b->id == ref.id && b->version == ref.version && b->public_ref) { found = b; break; }
        }
    }
    lookup_record(&c->lookup.buffer_counts, probes, found != NULL); return found;
}
static frontend_vk_pipeline *pipeline_find_probed(frontend_vk *c, mag_native_pipeline ref) {
    frontend_vk_pipeline *found = NULL; uint64_t probes = 0;
    if (ref.device_generation == c->generation && ref.id) {
        frontend_vk_pipeline *p = c->lookup.index_enabled
            ? c->lookup.pipelines[pipeline_lookup_bucket(ref.id)] : c->pipelines;
        for (; p; p = c->lookup.index_enabled ? p->lookup_next : p->next) {
            ++probes;
            if (p->id == ref.id && p->public_ref) { found = p; break; }
        }
    }
    lookup_record(&c->lookup.pipeline_counts, probes, found != NULL); return found;
}
static frontend_vk_buffer *buffer_find(frontend_vk *c, mag_native_buffer ref) {
    if (c->lookup.probes_enabled) return buffer_find_probed(c, ref);
    if (ref.device_generation != c->generation || !ref.id || !ref.version) return NULL;
    if (c->lookup.index_enabled) {
        for (frontend_vk_buffer *b = c->lookup.buffers[buffer_lookup_bucket(ref.id, ref.version)]; b; b = b->lookup_next)
            if (b->id == ref.id && b->version == ref.version && b->public_ref) return b;
        return NULL;
    }
    for (frontend_vk_buffer *b = c->buffers; b; b = b->next)
        if (b->id == ref.id && b->version == ref.version && b->public_ref) return b;
    return NULL;
}
static frontend_vk_pipeline *pipeline_find(frontend_vk *c, mag_native_pipeline ref) {
    if (c->lookup.probes_enabled) return pipeline_find_probed(c, ref);
    if (ref.device_generation != c->generation || !ref.id) return NULL;
    if (c->lookup.index_enabled) {
        for (frontend_vk_pipeline *p = c->lookup.pipelines[pipeline_lookup_bucket(ref.id)]; p; p = p->lookup_next)
            if (p->id == ref.id && p->public_ref) return p;
        return NULL;
    }
    for (frontend_vk_pipeline *p = c->pipelines; p; p = p->next)
        if (p->id == ref.id && p->public_ref) return p;
    return NULL;
}
/* Cumulative scalar evidence only: first two accepted scenes, every 120th,
 * and close. Counts are not elapsed time or frame-rate measurements. */
static void lookup_report(frontend_vk *c, int final) {
    if (!c->lookup.probes_enabled) return;
    uint64_t n = final ? c->lookup.submissions : ++c->lookup.submissions;
    if (!final && n > 2 && n % 120) return;
    uint64_t buffers = 0, pipelines = 0, public_buffers = 0, public_pipelines = 0;
    for (frontend_vk_buffer *b = c->buffers; b; b = b->next) { ++buffers; public_buffers += !!b->public_ref; }
    for (frontend_vk_pipeline *p = c->pipelines; p; p = p->next) { ++pipelines; public_pipelines += !!p->public_ref; }
    const frontend_vk_lookup_counts *b = &c->lookup.buffer_counts, *p = &c->lookup.pipeline_counts;
    fprintf(stderr, "[mag native lookup probes] generation=%llu index=%d scenes=%llu ticket=%llu final=%d buffers=%llu public_buffers=%llu pipelines=%llu public_pipelines=%llu buffer_calls=%llu buffer_probes=%llu buffer_max=%llu buffer_misses=%llu pipeline_calls=%llu pipeline_probes=%llu pipeline_max=%llu pipeline_misses=%llu backend=vulkan\n",
        (unsigned long long)c->generation, c->lookup.index_enabled, (unsigned long long)n, (unsigned long long)c->next_submission, final,
        (unsigned long long)buffers, (unsigned long long)public_buffers, (unsigned long long)pipelines, (unsigned long long)public_pipelines,
        (unsigned long long)b->calls, (unsigned long long)b->probes, (unsigned long long)b->maximum, (unsigned long long)b->misses,
        (unsigned long long)p->calls, (unsigned long long)p->probes, (unsigned long long)p->maximum, (unsigned long long)p->misses);
}
