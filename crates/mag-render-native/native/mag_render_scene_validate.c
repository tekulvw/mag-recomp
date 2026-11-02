#include "mag_render_scene_validate.h"
#include <math.h>
#include <string.h>
static int zero_buffer(mag_native_buffer b) { return !b.device_generation && !b.id && !b.version; }
static int same(mag_native_buffer a, mag_native_buffer b) {
    return a.device_generation == b.device_generation && a.id == b.id && a.version == b.version;
}
static int resource(const mag_native_scene *s, mag_native_buffer b) {
    for (uint32_t i = 0; i < s->resource_count; ++i)
        if (same(s->resources[i].buffer, b))
            return (int)i;
    return -1;
}
static int power(uint64_t n) { return n && !(n & (n - 1)); }
static int range(uint64_t offset, uint64_t length, uint64_t total, uint64_t align) {
    return length && power(align) && !(offset % align) && offset <= total && length <= total - offset;
}
static int color_format(uint32_t f){return f==MAG_NATIVE_SCENE_RGBA8 || f==MAG_NATIVE_SCENE_BGRA8;}
static int stage_source(const mag_native_shader_stage *s) {
    return s->source && s->source_length && s->source_length <= 1024 * 1024 && s->entry && s->entry_length &&
           s->entry_length <= 128 && s->language >= MAG_NATIVE_MSL && s->language <= MAG_NATIVE_SPIRV;
}
static int scene_graphics_schema(const mag_native_scene_graphics_desc *d) {
    if (!d || d->struct_size != sizeof(*d) || d->abi_version != MAG_NATIVE_SCENE_ABI ||
        d->vertex_stride < 4 || d->vertex_stride > 2048 || d->vertex_stride % 4 || (d->color_format && !color_format(d->color_format)) ||
        (d->depth_format != 0 && d->depth_format != MAG_NATIVE_SCENE_D32) ||
        (!d->color_format && !d->depth_format) || d->depth_compare > MAG_NATIVE_SCENE_DEPTH_ALWAYS ||
        d->depth_write > 1 || d->blend > MAG_NATIVE_SCENE_ALPHA || (!d->color_format && d->blend) ||
        (!d->depth_format && (d->depth_compare || d->depth_write)) || (!d->depth_compare && d->depth_write) ||
        d->buffer_count > 32 || d->texture_count > 32 || (d->buffer_count && !d->buffers) ||
        (d->texture_count && !d->textures))
        return MAG_NATIVE_INVALID;
    uint32_t slots[2] = {0}, samplers[2] = {0}, comparison[2] = {0};
    for (uint32_t i = 0; i < d->buffer_count; ++i) {
        const mag_native_scene_buffer_layout *r = &d->buffers[i];
        const mag_native_binding_layout *b = &r->binding;
        if (r->stage > 1 || r->reserved || (r->stage == 1 && !d->fragment.source_length) || b->slot >= 16 ||
            (slots[r->stage] & (1u << b->slot)) || b->access != MAG_NATIVE_READ || !b->minimum_bytes ||
            b->minimum_bytes % 4 || !power(b->alignment_bytes) || b->alignment_bytes < 4 ||
            b->alignment_bytes > 256 || b->runtime_array_stride ||
            b->runtime_size_index != MAG_NATIVE_NO_SIZE_INDEX || b->binding_kind > MAG_NATIVE_UNIFORM)
            return MAG_NATIVE_INVALID;
        slots[r->stage] |= 1u << b->slot;
    }
    for (uint32_t i = 0; i < d->texture_count; ++i) {
        const mag_native_scene_texture_layout *t = &d->textures[i];
        if (t->stage > 1 || t->reserved || (t->stage == 1 && !d->fragment.source_length) ||
            t->texture_slot >= 16 || t->sampler_slot >= 16 || t->texture_slot == t->sampler_slot ||
            (slots[t->stage] & (1u << t->texture_slot)) || (!color_format(t->format) && t->format!=MAG_NATIVE_SCENE_D32) ||
            t->comparison > 1 || (t->comparison && t->format != MAG_NATIVE_SCENE_D32))
            return MAG_NATIVE_INVALID;
        slots[t->stage] |= 1u << t->texture_slot;
        uint32_t bit = 1u << t->sampler_slot;
        if ((samplers[t->stage] & bit) && !!(comparison[t->stage] & bit) != !!t->comparison)
            return MAG_NATIVE_INVALID;
        samplers[t->stage] |= bit;
        if (t->comparison)
            comparison[t->stage] |= bit;
    }
    if ((slots[0] & samplers[0]) || (slots[1] & samplers[1]))
        return MAG_NATIVE_INVALID;
    return MAG_NATIVE_OK;
}
int mag_native_scene_graphics_validate(const mag_native_scene_graphics_desc *d) {
    int status = scene_graphics_schema(d);
    if (status || d->vertex_stride != 16)
        return MAG_NATIVE_INVALID;
    if (!stage_source(&d->vertex))
        return MAG_NATIVE_INVALID;
    if (d->color_format || d->fragment.source_length) {
        if (!stage_source(&d->fragment))
            return MAG_NATIVE_INVALID;
    } else if (d->fragment.source || d->fragment.source_length || d->fragment.entry ||
               d->fragment.entry_length || d->fragment.language)
        return MAG_NATIVE_INVALID;
    return MAG_NATIVE_OK;
}
int mag_native_scene_graphics_v2_validate(const mag_native_scene_graphics_v2_desc *v) {
    if(!v || v->struct_size!=sizeof(*v) || v->abi_version!=2 || v->reserved ||
       !v->attributes || !v->attribute_count || v->attribute_count>16)return MAG_NATIVE_INVALID;
    const mag_native_scene_graphics_desc *d=&v->base;
    if(scene_graphics_schema(d) || !stage_source(&d->vertex))return MAG_NATIVE_INVALID;
    if(d->color_format || d->fragment.source_length){if(!stage_source(&d->fragment))return MAG_NATIVE_INVALID;}
    else if(d->fragment.source || d->fragment.entry || d->fragment.entry_length || d->fragment.language)return MAG_NATIVE_INVALID;
    uint32_t seen=0;
    for(uint32_t i=0;i<v->attribute_count;++i){
        const mag_native_vertex_attribute *a=&v->attributes[i];
        uint32_t bytes=a->format>=MAG_NATIVE_VERTEX_FLOAT && a->format<=MAG_NATIVE_VERTEX_FLOAT4?a->format*4u:
            a->format==MAG_NATIVE_VERTEX_UNORM8X4?4u:0u;
        if(a->reserved || a->location>=16 || (seen&(1u<<a->location)) || !bytes ||
           a->offset>d->vertex_stride || bytes>d->vertex_stride-a->offset ||
           (a->format!=MAG_NATIVE_VERTEX_UNORM8X4 && a->offset%4u))return MAG_NATIVE_INVALID;
        for(uint32_t j=0;j<i;++j){const mag_native_vertex_attribute *b=&v->attributes[j];
            uint32_t bbytes=b->format<=MAG_NATIVE_VERTEX_FLOAT4?b->format*4u:4u;
            if(a->offset<b->offset+bbytes && b->offset<a->offset+bytes)return MAG_NATIVE_INVALID;
        }
        seen|=1u<<a->location;
    }
    return MAG_NATIVE_OK;
}
static int read_buffer(const mag_native_scene *s, mag_native_buffer b, uint64_t off, uint64_t len,
                       uint64_t align, const mag_native_scene_buffer_info *infos, const int *writers,
                       mag_native_scene_usage *u) {
    int i = resource(s, b);
    if (i < 0 || s->resources[i].role == MAG_NATIVE_READBACK ||
        (s->resources[i].role == MAG_NATIVE_PRODUCED && writers[i] < 0) ||
        !range(off, len, infos[i].length, align))
        return MAG_NATIVE_INVALID;
    u->read[i] = 1;
    return MAG_NATIVE_OK;
}
static int add_bytes(mag_native_scene_usage *u, uint64_t n) {
    if (n > MAG_NATIVE_BATCH_MAX_BYTES - u->bytes)
        return MAG_NATIVE_ALLOCATION_FAILED;
    u->bytes += n;
    return MAG_NATIVE_OK;
}
int mag_native_scene_validate(const mag_native_scene *s, const mag_native_scene_registry *r,
                              mag_native_scene_usage *u) {
    if (!s || !r || !u || !r->buffer || !r->compute || !r->graphics || s->struct_size != sizeof(*s) ||
        s->abi_version != MAG_NATIVE_SCENE_ABI || s->reserved || (s->resource_count && !s->resources) ||
        s->resource_count > MAG_NATIVE_BATCH_MAX_RESOURCES || !s->images || !s->image_count ||
        s->image_count > MAG_NATIVE_SCENE_MAX_IMAGES || !s->passes || !s->pass_count ||
        s->pass_count > MAG_NATIVE_SCENE_MAX_PASSES || s->dispatch_count > MAG_NATIVE_BATCH_MAX_COMMANDS ||
        (s->dispatch_count && !s->dispatches))
        return MAG_NATIVE_INVALID;
    memset(u, 0, sizeof(*u));
    mag_native_scene_buffer_info infos[MAG_NATIVE_BATCH_MAX_RESOURCES];
    int writers[MAG_NATIVE_BATCH_MAX_RESOURCES];
    memset(writers, -1, sizeof(writers));
    for (uint32_t i = 0; i < s->resource_count; ++i) {
        const mag_native_batch_resource *b = &s->resources[i];
        if (b->reserved || b->role > MAG_NATIVE_READBACK)
            return MAG_NATIVE_INVALID;
        for (uint32_t j = 0; j < i; ++j)
            if (same(b->buffer, s->resources[j].buffer))
                return MAG_NATIVE_INVALID;
        int status = r->buffer(r->context, b->buffer, &infos[i]);
        if (status)
            return status;
        if (infos[i].busy)
            return MAG_NATIVE_BUSY;
        if (b->role == MAG_NATIVE_UPLOAD && infos[i].gpu_written)
            return MAG_NATIVE_INVALID;
        if (b->role != MAG_NATIVE_UPLOAD && (!infos[i].seed_zero || infos[i].gpu_written))
            return MAG_NATIVE_INVALID;
        if (add_bytes(u, infos[i].length))
            return MAG_NATIVE_ALLOCATION_FAILED;
    }
    for (uint32_t node = 0; node < s->dispatch_count; ++node) {
        const mag_native_frame *f = &s->dispatches[node];
        mag_native_scene_compute_info p;
        if (f->struct_size != sizeof(*f) || f->abi_version != MAG_NATIVE_ABI_VERSION || !f->bindings)
            return MAG_NATIVE_INVALID;
        int status = r->compute(r->context, f->pipeline, &p);
        if (status)
            return status;
        if (f->binding_count != p.count || !p.count || p.count > 16)
            return MAG_NATIVE_INVALID;
        uint32_t slots = 0;
        uint8_t accesses[MAG_NATIVE_BATCH_MAX_RESOURCES] = {0};
        for (unsigned k = 0; k < 3; ++k)
            if (!f->groups[k] || f->groups[k] > 65535)
                return MAG_NATIVE_INVALID;
        for (uint32_t j = 0; j < f->binding_count; ++j) {
            const mag_native_binding *b = &f->bindings[j];
            const mag_native_binding_layout *layout = NULL;
            if (b->reserved || b->slot >= 16 || (slots & (1u << b->slot)))
                return MAG_NATIVE_INVALID;
            slots |= 1u << b->slot;
            for (uint32_t k = 0; k < p.count; ++k)
                if (p.bindings[k].slot == b->slot)
                    layout = &p.bindings[k];
            if (!layout || b->byte_length < layout->minimum_bytes)
                return MAG_NATIVE_INVALID;
            uint64_t alignment =
                layout->binding_kind == MAG_NATIVE_UNIFORM ? r->uniform_alignment : r->storage_alignment;
            if (alignment < layout->alignment_bytes)
                alignment = layout->alignment_bytes;
            uint64_t maximum =
                layout->binding_kind == MAG_NATIVE_UNIFORM ? r->max_uniform_bytes : r->max_storage_bytes;
            int i = resource(s, b->buffer);
            if (i < 0 || !range(b->offset, b->byte_length, infos[i].length, alignment) ||
                b->byte_length > maximum)
                return MAG_NATIVE_INVALID;
            if (layout->access == MAG_NATIVE_READ_WRITE) {
                if (s->resources[i].role != MAG_NATIVE_PRODUCED || writers[i] >= 0 || accesses[i])
                    return MAG_NATIVE_INVALID;
                accesses[i] = 2;
            } else {
                if (accesses[i] == 2 ||
                    read_buffer(s, b->buffer, b->offset, b->byte_length, alignment, infos, writers, u))
                    return MAG_NATIVE_INVALID;
                accesses[i] = 1;
            }
        }
        for (uint32_t i = 0; i < s->resource_count; ++i)
            if (accesses[i] == 2) {
                writers[i] = (int)node;
                u->write[i] = 1;
            }
    }
    uint8_t initialized[MAG_NATIVE_SCENE_MAX_IMAGES] = {0};
    for (uint32_t i = 0; i < s->image_count; ++i) {
        const mag_native_scene_image *im = &s->images[i];
        if (im->reserved || !im->width || !im->height || im->width > 8192 || im->height > 8192 ||
            (!color_format(im->format) && im->format!=MAG_NATIVE_SCENE_D32))
            return MAG_NATIVE_INVALID;
        uint64_t bytes = (uint64_t)im->width * im->height * 4;
        if (add_bytes(u, bytes))
            return MAG_NATIVE_ALLOCATION_FAILED;
        if (!zero_buffer(im->upload)) {
            int j = resource(s, im->upload);
            if (j < 0 || !color_format(im->format) || s->resources[j].role != MAG_NATIVE_UPLOAD ||
                infos[j].length != bytes)
                return MAG_NATIVE_INVALID;
            u->read[j] = 1;
            initialized[i] = 1;
        }
    }
    uint32_t total_draws = 0;
    for (uint32_t pass_index = 0; pass_index < s->pass_count; ++pass_index) {
        const mag_native_scene_pass *p = &s->passes[pass_index];
        if (p->reserved || (p->draw_count && !p->draws) ||
            p->draw_count > MAG_NATIVE_SCENE_MAX_DRAWS - total_draws)
            return MAG_NATIVE_INVALID;
        total_draws += p->draw_count;
        const mag_native_scene_attachment *attachments[2] = {&p->color, &p->depth};
        uint32_t width = 0, height = 0;
        for (unsigned k = 0; k < 2; ++k) {
            const mag_native_scene_attachment *a = attachments[k];
            if (a->image == MAG_NATIVE_SCENE_NONE)
                continue;
            if (a->image >= s->image_count || a->reserved || a->load > 1 || a->store > 1 ||
                (k ? s->images[a->image].format != MAG_NATIVE_SCENE_D32 : !color_format(s->images[a->image].format)) ||
                (a->load == MAG_NATIVE_SCENE_LOAD && !initialized[a->image]))
                return MAG_NATIVE_INVALID;
            const mag_native_scene_image *im = &s->images[a->image];
            if (width && (width != im->width || height != im->height))
                return MAG_NATIVE_INVALID;
            width = im->width;
            height = im->height;
            if (a->load == MAG_NATIVE_SCENE_CLEAR)
                for (unsigned j = 0; j < (k ? 1u : 4u); ++j)
                    if (!isfinite(a->clear[j]) || a->clear[j] < 0 || a->clear[j] > 1)
                        return MAG_NATIVE_INVALID;
        }
        if (!width)
            return MAG_NATIVE_INVALID;
        for (uint32_t draw_index = 0; draw_index < p->draw_count; ++draw_index) {
            const mag_native_scene_draw *d = &p->draws[draw_index];
            const mag_native_scene_graphics_desc *g = NULL;
            int status = r->graphics(r->context, d->pipeline, &g);
            if (status)
                return status;
            if (!g || scene_graphics_schema(g) || d->reserved || d->buffer_count != g->buffer_count ||
                d->texture_count != g->texture_count || (d->buffer_count && !d->buffers) ||
                (d->texture_count && !d->textures) || !d->index_count || d->index_count % 3 ||
                d->index_length < (uint64_t)d->index_count * 4 || d->vertex_length % g->vertex_stride ||
                g->color_format != (p->color.image == MAG_NATIVE_SCENE_NONE ? 0 : s->images[p->color.image].format) ||
                g->depth_format != (p->depth.image == MAG_NATIVE_SCENE_NONE ? 0 : MAG_NATIVE_SCENE_D32))
                return MAG_NATIVE_INVALID;
            if (read_buffer(s, d->vertices, d->vertex_offset, d->vertex_length, 4, infos, writers, u) ||
                read_buffer(s, d->indices, d->index_offset, d->index_length, 4, infos, writers, u))
                return MAG_NATIVE_INVALID;
            uint32_t slots[2] = {0}, textures[2] = {0}, samplers[2] = {0}, sampler_kinds[2][16] = {{0}};
            for (uint32_t j = 0; j < d->buffer_count; ++j) {
                const mag_native_scene_buffer_binding *b = &d->buffers[j];
                const mag_native_binding_layout *layout = NULL;
                if (b->stage > 1 || b->reserved || b->binding.reserved || b->binding.slot >= 16 ||
                    (slots[b->stage] & (1u << b->binding.slot)))
                    return MAG_NATIVE_INVALID;
                slots[b->stage] |= 1u << b->binding.slot;
                for (uint32_t k = 0; k < g->buffer_count; ++k)
                    if (g->buffers[k].stage == b->stage && g->buffers[k].binding.slot == b->binding.slot)
                        layout = &g->buffers[k].binding;
                if (!layout || b->binding.byte_length < layout->minimum_bytes)
                    return MAG_NATIVE_INVALID;
                uint64_t align =
                    layout->binding_kind == MAG_NATIVE_UNIFORM ? r->uniform_alignment : r->storage_alignment;
                if (align < layout->alignment_bytes)
                    align = layout->alignment_bytes;
                uint64_t max =
                    layout->binding_kind == MAG_NATIVE_UNIFORM ? r->max_uniform_bytes : r->max_storage_bytes;
                if (b->binding.byte_length > max ||
                    read_buffer(s, b->binding.buffer, b->binding.offset, b->binding.byte_length, align, infos,
                                writers, u))
                    return MAG_NATIVE_INVALID;
            }
            for (uint32_t j = 0; j < d->texture_count; ++j) {
                const mag_native_scene_texture_binding *t = &d->textures[j];
                const mag_native_scene_texture_layout *layout = NULL;
                if (t->stage > 1 || t->reserved || t->texture_slot >= 16 || t->sampler_slot >= 16 ||
                    t->sampler > 2 || t->image >= s->image_count || !initialized[t->image] ||
                    t->image == p->color.image || t->image == p->depth.image ||
                    (textures[t->stage] & (1u << t->texture_slot)))
                    return MAG_NATIVE_INVALID;
                textures[t->stage] |= 1u << t->texture_slot;
                for (uint32_t k = 0; k < g->texture_count; ++k)
                    if (g->textures[k].stage == t->stage && g->textures[k].texture_slot == t->texture_slot &&
                        g->textures[k].sampler_slot == t->sampler_slot)
                        layout = &g->textures[k];
                if (!layout || layout->format != s->images[t->image].format ||
                    !!layout->comparison != (t->sampler == MAG_NATIVE_SCENE_COMPARE_LEQUAL))
                    return MAG_NATIVE_INVALID;
                uint32_t bit = 1u << t->sampler_slot;
                if ((samplers[t->stage] & bit) && sampler_kinds[t->stage][t->sampler_slot] != t->sampler)
                    return MAG_NATIVE_INVALID;
                samplers[t->stage] |= bit;
                sampler_kinds[t->stage][t->sampler_slot] = t->sampler;
            }
        }
        for (unsigned k = 0; k < 2; ++k)
            if (attachments[k]->image != MAG_NATIVE_SCENE_NONE)
                initialized[attachments[k]->image] = attachments[k]->store == MAG_NATIVE_SCENE_STORE;
    }
    if (s->output_image != MAG_NATIVE_SCENE_NONE) {
        if (s->output_image >= s->image_count || !initialized[s->output_image] ||
            !color_format(s->images[s->output_image].format))
            return MAG_NATIVE_INVALID;
    }
    if (!zero_buffer(s->diagnostic_readback)) {
        int i = resource(s, s->diagnostic_readback);
        if (i < 0 || s->output_image == MAG_NATIVE_SCENE_NONE || s->resources[i].role != MAG_NATIVE_READBACK)
            return MAG_NATIVE_INVALID;
        const mag_native_scene_image *im = &s->images[s->output_image];
        if (infos[i].length != (uint64_t)im->width * im->height * 4)
            return MAG_NATIVE_INVALID;
        uint64_t pitch = ((uint64_t)im->width * 4 + 255) & ~UINT64_C(255);
        if (add_bytes(u, pitch * im->height))
            return MAG_NATIVE_ALLOCATION_FAILED;
        u->write[i] = 1;
    }
    for (uint32_t i = 0; i < s->resource_count; ++i)
        if (!u->read[i] && !u->write[i])
            return MAG_NATIVE_INVALID;
    return MAG_NATIVE_OK;
}
