/* Resident uploaded images (opt-in, MAG_RENDER_VULKAN_RESIDENT_IMAGES=1).
 *
 * Without it every scene image with an upload is a new VkImage, a new
 * allocation and a buffer-to-image copy on every submission, and is
 * destroyed when that submission retires.
 *
 * An uploaded image that no pass of its scene attaches (writes), and that is
 * not the diagnostic output, is only sampled: its contents are exactly its
 * upload bytes. Such an image is kept after its submission and reused, with
 * no allocation and no copy, by a later scene whose image has the same shape
 * and byte-identical upload contents:
 *   - identity: the same upload buffer (id, version), never GPU-written, needs
 *     no comparison at all, since a buffer version's bytes never change;
 *   - content: otherwise a hash picks candidates and the bytes are compared
 *     in full.
 * A resident image is never written again. It is inserted only after the
 * submission that uploaded it was accepted by the queue, and each submission
 * that samples it holds a reference until it retires; an image evicted while
 * referenced is destroyed at its last release. Its layout after the last
 * encoding that used it is remembered, so the next one transitions from the
 * right state; uses on the one queue are ordered by those transitions'
 * barriers. Bounded at 4096 entries and 256 MiB of upload bytes, oldest use
 * evicted first. */
typedef struct frontend_vk_resident {
    VkImage image;
    VkDeviceMemory memory;
    VkDeviceSize memory_bytes;
    VkImageLayout layout;
    VkImageAspectFlags aspect;
    uint32_t mip_count, layers, refs;
    int evicted;
    uint8_t *key; size_t key_length;       /* format, extent, upload length, shape */
    uint8_t *bytes; uint64_t length;        /* the upload contents */
    uint64_t hash, upload_id, upload_version, used;
    struct frontend_vk_resident *next;
} frontend_vk_resident;
static const uint64_t resident_max_bytes = 256ull * 1024 * 1024;
static const uint32_t resident_max_entries = 4096;

static int resident_attached(const mag_native_scene *scene, uint32_t image) {
    if (image == scene->output_image) return 1;
    for (uint32_t n = 0; n < scene->pass_count; ++n) {
        const mag_native_scene_pass *p = &scene->passes[n];
        if (p->color.image == image || p->depth.image == image) return 1;
    }
    return 0;
}
/* Everything that makes two images with equal bytes interchangeable. */
static uint8_t *resident_key(const mag_native_scene_image *im, const mag_native_scene_image_layout *shape,
                             uint64_t length, size_t *out_length) {
    const size_t subresources = (size_t)shape->subresource_count * sizeof(*shape->subresources);
    const size_t n = 4 * sizeof(uint32_t) + sizeof(uint64_t) + 6 * sizeof(uint32_t) + subresources;
    uint8_t *k = malloc(n);
    if (!k) return NULL;
    const uint32_t head[4] = {im->format, im->width, im->height, 1u};
    const uint32_t s[6] = {shape->dimension, shape->depth, shape->layers, shape->mip_count, shape->samples,
                           shape->subresource_count};
    uint8_t *p = k;
    memcpy(p, head, sizeof head); p += sizeof head;
    memcpy(p, &length, sizeof length); p += sizeof length;
    memcpy(p, s, sizeof s); p += sizeof s;
    if (subresources) memcpy(p, shape->subresources, subresources);
    *out_length = n;
    return k;
}
static uint64_t resident_hash(const uint8_t *key, size_t key_length, const uint8_t *bytes, uint64_t length) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < key_length; ++i) { h ^= key[i]; h *= 1099511628211ull; }
    /* A bounded sample of the contents; equality is always a full compare. */
    const uint64_t words = length / 8, step = words > 512 ? words / 512 : 1;
    for (uint64_t w = 0; w < words; w += step) { uint64_t v; memcpy(&v, bytes + w * 8, 8); h ^= v; h *= 1099511628211ull; }
    for (uint64_t i = words * 8; i < length; ++i) { h ^= bytes[i]; h *= 1099511628211ull; }
    return h;
}
static void resident_destroy(frontend_vk *c, frontend_vk_resident *e) {
    if (e->image) c->vkDestroyImage(c->owner.device, e->image, NULL);
    native_memory_free(c, e->memory, &e->memory_bytes);
    free(e->key); free(e->bytes); free(e);
}
static void resident_unlink(frontend_vk *c, frontend_vk_resident *e) {
    frontend_vk_resident **link = &c->residents;
    while (*link && *link != e) link = &(*link)->next;
    if (*link) *link = e->next;
    c->resident_bytes -= e->length; --c->resident_entries; ++c->resident_evictions;
}
static void resident_unref(frontend_vk *c, frontend_vk_resident *e) {
    if (e->refs) --e->refs;
    if (!e->refs && e->evicted) resident_destroy(c, e);
}
static void resident_close(frontend_vk *c) {
    while (c->residents) {
        frontend_vk_resident *e = c->residents;
        c->residents = e->next;
        resident_destroy(c, e);
    }
    c->resident_entries = 0; c->resident_bytes = 0;
}
/* A resident image for scene image `i`, or NULL. Takes a reference. */
static frontend_vk_resident *resident_find(frontend_vk *c, const mag_native_scene_image *im,
                                           const mag_native_scene_image_layout *shape) {
    const frontend_vk_buffer *b = buffer_find(c, im->upload);
    if (!b) return NULL;
    size_t key_length = 0;
    uint8_t *key = resident_key(im, shape, b->length, &key_length);
    if (!key) return NULL;
    frontend_vk_resident *hit = NULL;
    if (!b->gpu_written)
        for (frontend_vk_resident *e = c->residents; e && !hit; e = e->next)
            if (e->upload_id == b->id && e->upload_version == b->version && e->key_length == key_length &&
                !memcmp(e->key, key, key_length)) {
                hit = e; ++c->resident_identity_hits;
            }
    if (!hit) {
        const uint64_t h = resident_hash(key, key_length, b->mapped, b->length);
        for (frontend_vk_resident *e = c->residents; e && !hit; e = e->next)
            if (e->hash == h && e->length == b->length && e->key_length == key_length &&
                !memcmp(e->key, key, key_length) && !memcmp(e->bytes, b->mapped, (size_t)b->length)) {
                hit = e;
                /* Proven equal: later submissions of this immutable buffer take the identity path. */
                if (!b->gpu_written) { e->upload_id = b->id; e->upload_version = b->version; }
            }
    }
    free(key);
    if (hit) { ++hit->refs; hit->used = c->next_submission + 1; ++c->resident_hits; }
    else ++c->resident_misses;
    return hit;
}
/* After its submission was accepted: keep slot image `im` (scene image `i`),
 * made from its upload, resident. The slot then holds a reference. */
static void resident_insert(frontend_vk *c, frontend_vk_scene_image *im, const mag_native_scene_image *desc,
                            const mag_native_scene_image_layout *shape) {
    const frontend_vk_buffer *b = buffer_find(c, desc->upload);
    if (!b || b->length > resident_max_bytes / 4) return;
    while (c->resident_entries && (c->resident_entries >= resident_max_entries ||
                                   c->resident_bytes + b->length > resident_max_bytes)) {
        frontend_vk_resident *oldest = NULL;
        for (frontend_vk_resident *e = c->residents; e; e = e->next)
            if (!oldest || e->used < oldest->used) oldest = e;
        if (!oldest) break;
        resident_unlink(c, oldest);
        if (oldest->refs) oldest->evicted = 1;
        else resident_destroy(c, oldest);
    }
    frontend_vk_resident *e = calloc(1, sizeof(*e));
    if (!e) return;
    e->key = resident_key(desc, shape, b->length, &e->key_length);
    e->bytes = malloc((size_t)b->length ? (size_t)b->length : 1);
    if (!e->key || !e->bytes) { free(e->key); free(e->bytes); free(e); return; }
    memcpy(e->bytes, b->mapped, (size_t)b->length);
    e->length = b->length;
    e->hash = resident_hash(e->key, e->key_length, e->bytes, e->length);
    e->upload_id = b->gpu_written ? 0 : b->id; e->upload_version = b->version;
    /* Ownership of the image moves from the slot to the cache. */
    e->image = im->image; e->memory = im->memory; e->memory_bytes = im->memory_bytes;
    e->layout = im->layout; e->aspect = im->aspect; e->mip_count = im->mip_count; e->layers = im->layers;
    e->refs = 1; e->used = c->next_submission;
    im->memory = VK_NULL_HANDLE; im->memory_bytes = 0; im->resident = e;
    e->next = c->residents; c->residents = e;
    c->resident_bytes += e->length; ++c->resident_entries;
}
