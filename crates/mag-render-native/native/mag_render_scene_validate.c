#include "mag_render_scene_validate.h"
#include "mag_render_stencil_validate.h"
#include "mag_render_image_validate.h"
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
static int depth_format(uint32_t f){mag_native_image_format info;return mag_native_image_format_info(f,&info)&&info.depth_attachment;}
static int color_format(uint32_t f){mag_native_image_format info;return mag_native_image_format_info(f,&info)&&info.color_attachment;}
static int sample_format(uint32_t f){mag_native_image_format info;return mag_native_image_format_info(f,&info);}
static int stage_source(const mag_native_shader_stage *s) {
    return s->source && s->source_length && s->source_length <= 1024 * 1024 && s->entry && s->entry_length &&
           s->entry_length <= 128 && s->language >= MAG_NATIVE_MSL && s->language <= MAG_NATIVE_SPIRV;
}
static int scene_graphics_schema(const mag_native_scene_graphics_desc *d,uint32_t model) {
    if (model>1 || !d || d->struct_size != sizeof(*d) || d->abi_version != MAG_NATIVE_SCENE_ABI ||
        d->vertex_stride < 4 || d->vertex_stride > 2048 || d->vertex_stride % 4 || (d->color_format && !color_format(d->color_format)) ||
        (d->depth_format != 0 && !depth_format(d->depth_format)) ||
        (!d->color_format && !d->depth_format) || d->depth_compare > MAG_NATIVE_SCENE_DEPTH_ALWAYS ||
        d->depth_write > 1 || d->blend > MAG_NATIVE_SCENE_ALPHA || (!d->color_format && d->blend) ||
        (!d->depth_format && (d->depth_compare || d->depth_write)) || (!d->depth_compare && d->depth_write) ||
        d->buffer_count > 32 || d->texture_count > 32 || (d->buffer_count && !d->buffers) ||
        (d->texture_count && !d->textures))
        return MAG_NATIVE_INVALID;
    uint32_t slots[2] = {0}, textures[2]={0}, samplers[2] = {0}, comparison[2] = {0};
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
            t->texture_slot >= 16 || t->sampler_slot >= 16 || (!model && t->texture_slot == t->sampler_slot) ||
            ((model?textures[t->stage]:slots[t->stage]) & (1u << t->texture_slot)) || !sample_format(t->format) ||
            t->comparison > 1 || (t->comparison && !depth_format(t->format)))
            return MAG_NATIVE_INVALID;
        textures[t->stage] |= 1u << t->texture_slot;
        if(!model)slots[t->stage] |= 1u << t->texture_slot;
        uint32_t bit = 1u << t->sampler_slot;
        if ((samplers[t->stage] & bit) && !!(comparison[t->stage] & bit) != !!t->comparison)
            return MAG_NATIVE_INVALID;
        samplers[t->stage] |= bit;
        if (t->comparison)
            comparison[t->stage] |= bit;
    }
    if (!model && ((slots[0] & samplers[0]) || (slots[1] & samplers[1])))
        return MAG_NATIVE_INVALID;
    return MAG_NATIVE_OK;
}
int mag_native_scene_graphics_validate(const mag_native_scene_graphics_desc *d) {
    int status = scene_graphics_schema(d,0);
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
    if(!v || v->struct_size!=sizeof(*v) || v->abi_version!=2 || v->binding_model>1 ||
       !v->attributes || !v->attribute_count || v->attribute_count>16)return MAG_NATIVE_INVALID;
    const mag_native_scene_graphics_desc *d=&v->base;
    if(scene_graphics_schema(d,v->binding_model) || !stage_source(&d->vertex))return MAG_NATIVE_INVALID;
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
    if (n > MAG_NATIVE_SCENE_MAX_BYTES - u->bytes)
        return MAG_NATIVE_ALLOCATION_FAILED;
    u->bytes += n;
    return MAG_NATIVE_OK;
}
static int scene_validate(const mag_native_scene *s, const mag_native_scene_registry *r,
                              mag_native_scene_usage *u,const mag_native_scene_registry_v2 *v2,const mag_native_scene_v3 *v3,const mag_native_scene_v4 *v4,const mag_native_scene_registry_v4 *registry4,const mag_native_scene_v5 *v5,const mag_native_scene_registry_v5 *registry5) {
    if (!s || !r || !u || !r->buffer || !r->compute || !r->graphics || s->struct_size != sizeof(*s) ||
        s->abi_version != MAG_NATIVE_SCENE_ABI || s->reserved || (s->resource_count && !s->resources) ||
        s->resource_count > MAG_NATIVE_SCENE_MAX_RESOURCES || !s->images || !s->image_count ||
        s->image_count > MAG_NATIVE_SCENE_MAX_IMAGES || !s->passes || !s->pass_count ||
        s->pass_count > MAG_NATIVE_SCENE_MAX_PASSES || s->dispatch_count > MAG_NATIVE_BATCH_MAX_COMMANDS ||
        (s->dispatch_count && !s->dispatches))
        return MAG_NATIVE_INVALID;
    memset(u, 0, sizeof(*u));
    mag_native_scene_buffer_info infos[MAG_NATIVE_SCENE_MAX_RESOURCES];
    int writers[MAG_NATIVE_SCENE_MAX_RESOURCES];
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
        /* An initialized upload (created with bytes, never GPU-written) can
         * only ever be read: no role may write it. Concurrent read-only use by
         * another pending submission is therefore not a hazard. Any other
         * busy buffer still refuses. */
        if (infos[i].busy &&
            !(b->role == MAG_NATIVE_UPLOAD && !infos[i].seed_zero && !infos[i].gpu_written))
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
        uint8_t accesses[MAG_NATIVE_SCENE_MAX_RESOURCES] = {0};
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
    uint8_t imported[MAG_NATIVE_SCENE_MAX_IMAGES]={0};
    mag_native_import_info import_info[MAG_NATIVE_SCENE_MAX_IMAGES];memset(import_info,0,sizeof(import_info));
    if(v5)for(uint32_t i=0;i<v5->import_count;++i){
        const mag_native_scene_import *im=&v5->imports[i];
        if(im->image>=s->image_count||imported[im->image]||!im->ref.device_generation||!im->ref.id)return MAG_NATIVE_INVALID;
        const mag_native_scene_image *d=&s->images[im->image];mag_native_image_format f;
        if(!mag_native_image_format_info(d->format,&f)||!zero_buffer(d->upload))return MAG_NATIVE_INVALID;
        uint32_t planes=f.depth_attachment?(mag_native_image_has_stencil(d->format)?6u:2u):1u;
        if(im->required_initialized_planes&~planes)return MAG_NATIVE_INVALID;
        int status=registry5->image(r->context,im->ref,im->required_initialized_planes,&import_info[im->image]);if(status)return status;
        const mag_native_import_info *actual=&import_info[im->image];
        if(actual->reserved||actual->format!=d->format||actual->width!=d->width||actual->height!=d->height||!actual->owner_lifetime||!actual->resource_id||!actual->resource_generation)return MAG_NATIVE_STALE;
        imported[im->image]=1;
    }
    uint8_t initialized[MAG_NATIVE_SCENE_MAX_IMAGES] = {0},stencil_initialized[MAG_NATIVE_SCENE_MAX_IMAGES]={0};
    for (uint32_t i = 0; i < s->image_count; ++i) {
        const mag_native_scene_image *im = &s->images[i];
        if(mag_native_image_has_stencil(im->format)&&!v4)return MAG_NATIVE_UNSUPPORTED;
        uint64_t bytes,upload_extent;
        if(v3&&v3->image_layout_count){
            int status=mag_native_image_layout_validate(im,&v3->image_layouts[i],&bytes,&upload_extent);
            if(status)return status;
        }else{
            if(im->reserved||!im->width||!im->height||im->width>8192||im->height>8192||
               (im->format!=MAG_NATIVE_SCENE_RGBA8&&im->format!=MAG_NATIVE_SCENE_BGRA8&&im->format!=MAG_NATIVE_SCENE_D32))return MAG_NATIVE_INVALID;
            bytes=(uint64_t)im->width*im->height*4;upload_extent=bytes;
        }
        if(add_bytes(u,bytes))return MAG_NATIVE_ALLOCATION_FAILED;
        if(imported[i]){for(uint32_t j=0;j<v5->import_count;++j)if(v5->imports[j].image==i){
            uint32_t planes=v5->imports[j].required_initialized_planes;
            initialized[i]=(planes&(depth_format(im->format)?2u:1u))!=0;stencil_initialized[i]=(planes&4u)!=0;
        }}

        if(!zero_buffer(im->upload)){
            int j=resource(s,im->upload);
            if(j<0||s->resources[j].role!=MAG_NATIVE_UPLOAD||(v3&&v3->image_layout_count?infos[j].length<upload_extent:infos[j].length!=upload_extent)||
               ((!v3||!v3->image_layout_count)&&im->format==MAG_NATIVE_SCENE_D32))return MAG_NATIVE_INVALID;
            u->read[j]=1;initialized[i]=1;
        }
    }
    if(v3){
        for(uint32_t i=0;i<v3->view_count;++i){
            const mag_native_image_view *view=&v3->views[i];
            if(view->image>=s->image_count)return MAG_NATIVE_INVALID;
            mag_native_scene_image_layout legacy={2,1,1,1,1,0,NULL,0,0};
            int status=mag_native_image_view_validate(&s->images[view->image],v3->image_layout_count?&v3->image_layouts[view->image]:&legacy,view);
            if(status)return status;
        }
    }
    uint32_t bound_index=0;
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
            if (a->image >= s->image_count || a->reserved || a->load > (v4?2u:1u) || a->store > 1 ||
                (v3&&v3->image_layout_count&&(v3->image_layouts[a->image].dimension!=2||v3->image_layouts[a->image].mip_count!=1)) ||
                (k ? !depth_format(s->images[a->image].format) : !color_format(s->images[a->image].format)) ||
                (a->load == MAG_NATIVE_SCENE_LOAD && !initialized[a->image]))
                return MAG_NATIVE_INVALID;
            const mag_native_scene_image *im = &s->images[a->image];
            if(width){
                if(im->width<width||im->height<height)return MAG_NATIVE_INVALID;
                if((im->width!=width||im->height!=height)&&
                   (a->load==MAG_NATIVE_SCENE_CLEAR||(v4&&v4->stencils[pass_index].load==MAG_NATIVE_STENCIL_CLEAR&&mag_native_image_has_stencil(im->format))))return MAG_NATIVE_UNSUPPORTED;
            }else{width=im->width;height=im->height;}
            if (a->load == MAG_NATIVE_SCENE_CLEAR)
                for (unsigned j = 0; j < (k ? 1u : 4u); ++j)
                    if (!isfinite(a->clear[j]) || ((k || im->format<11 || im->format>14) && (a->clear[j] < 0 || a->clear[j] > 1)))
                        return MAG_NATIVE_INVALID;
        }
        if (!width)
            return MAG_NATIVE_INVALID;
        if(v3&&v3->viewport_count&&!mag_native_viewport_valid(&v3->viewports[pass_index],width,height))return MAG_NATIVE_INVALID;
        if(v4){int stencil_status=mag_native_stencil_pass_validate(v4,registry4,pass_index,stencil_initialized);if(stencil_status)return stencil_status;}
        int bounds_snapshot=0;
        for (uint32_t draw_index = 0; draw_index < p->draw_count; ++draw_index) {
            const mag_native_scene_draw *d = &p->draws[draw_index];
            const mag_native_depth_bounds *bounds=v5&&v5->bounds_count?&v5->bounds[bound_index]:NULL;++bound_index;
            if(bounds&&(bounds->reserved||bounds->enabled>1||!isfinite(bounds->minimum)||!isfinite(bounds->maximum)||
                (bounds->enabled&&(bounds->minimum<0||bounds->maximum>1||bounds->minimum>bounds->maximum))))return MAG_NATIVE_INVALID;
            if(registry4){
                const mag_native_raster_state *state=NULL;int status=registry4->raster(r->context,d->pipeline,&state);if(status)return status;
                if(!!(state&&state->depth_bounds_enabled)!=!!(bounds&&bounds->enabled))return MAG_NATIVE_INVALID;
                if(bounds&&bounds->enabled){
                    if(p->depth.image==MAG_NATIVE_SCENE_NONE||p->depth.load!=MAG_NATIVE_SCENE_LOAD||(state->stencil_enabled&&((state->front_stencil.write_mask|state->back_stencil.write_mask)&255u))||(v4&&v4->stencils[pass_index].flags))return MAG_NATIVE_UNSUPPORTED;
                    if(add_bytes(u,16)){return MAG_NATIVE_ALLOCATION_FAILED;}
                    bounds_snapshot=1;
                }
            }

            const mag_native_scene_graphics_desc *g = NULL;
            uint32_t binding_model=0;
            int status = r->graphics(r->context, d->pipeline, &g,&binding_model);
            if (status)
                return status;
            if(v4&&p->depth.image!=MAG_NATIVE_SCENE_NONE&&p->depth.load==MAG_NATIVE_SCENE_DISCARD_LOAD){
                const mag_native_raster_state *state=NULL;
                int raster_status=registry4->raster(r->context,d->pipeline,&state);if(raster_status)return raster_status;
                if(state?(state->depth_enabled&&state->depth_compare!=MAG_NATIVE_COMPARE_ALWAYS&&state->depth_compare!=MAG_NATIVE_COMPARE_NEVER):
                    (g&&g->depth_compare!=MAG_NATIVE_SCENE_DEPTH_DISABLED&&g->depth_compare!=MAG_NATIVE_SCENE_DEPTH_ALWAYS))return MAG_NATIVE_INVALID;
            }
            if (!g || scene_graphics_schema(g,binding_model) || d->reserved || d->buffer_count != g->buffer_count ||
                d->texture_count != g->texture_count || (d->buffer_count && !d->buffers) ||
                (d->texture_count && !d->textures) || !d->index_count || d->index_count % 3 ||
                d->index_length < (uint64_t)d->index_count * 4 || d->vertex_length % g->vertex_stride ||
                g->color_format != (p->color.image == MAG_NATIVE_SCENE_NONE ? 0 : s->images[p->color.image].format) ||
                g->depth_format != (p->depth.image == MAG_NATIVE_SCENE_NONE ? 0 : s->images[p->depth.image].format))
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
                uint32_t image_index=mag_native_scene_image_index(v3,t->image);
                if(image_index<s->image_count&&imported[image_index]){
                    uint32_t targets[2]={p->color.image,p->depth.image};
                    for(unsigned k=0;k<2;++k)if(targets[k]<s->image_count&&imported[targets[k]]){
                        const mag_native_import_info *a=&import_info[image_index],*b=&import_info[targets[k]];
                        if(a->owner_lifetime==b->owner_lifetime&&a->resource_id==b->resource_id&&a->resource_generation==b->resource_generation)return MAG_NATIVE_INVALID;
                    }
                }
                if (t->stage > 1 || t->reserved || t->texture_slot >= 16 || t->sampler_slot >= 16 ||
                    image_index >= s->image_count || !initialized[image_index] ||
                    image_index == p->color.image || image_index == p->depth.image ||
                    (textures[t->stage] & (1u << t->texture_slot)))
                    return MAG_NATIVE_INVALID;
                textures[t->stage] |= 1u << t->texture_slot;
                for (uint32_t k = 0; k < g->texture_count; ++k)
                    if (g->textures[k].stage == t->stage && g->textures[k].texture_slot == t->texture_slot &&
                        g->textures[k].sampler_slot == t->sampler_slot)
                        layout = &g->textures[k];
                int comparison=t->sampler==MAG_NATIVE_SCENE_COMPARE_LEQUAL;
                if(t->sampler&MAG_NATIVE_SCENE_SAMPLER_TABLE_BIT){
                    uint32_t index=MAG_NATIVE_SCENE_SAMPLER_TABLE_INDEX(t->sampler);
                    if(!v2||index>=v2->sampler_count)return MAG_NATIVE_INVALID;
                    const mag_native_sampler_state *state=NULL;
                    int sampler_status=v2->sampler(r->context,v2->samplers[index],&state);
                    if(sampler_status)return sampler_status;
                    if(!state)return MAG_NATIVE_INVALID;
                    comparison=state->comparison_enabled;u->samplers[index]=1;
                }else if(t->sampler>2)return MAG_NATIVE_INVALID;
                if (!layout || layout->format != s->images[image_index].format ||
                    !!layout->comparison != !!comparison)
                    return MAG_NATIVE_INVALID;
                uint32_t bit = 1u << t->sampler_slot;
                if ((samplers[t->stage] & bit) && sampler_kinds[t->stage][t->sampler_slot] != t->sampler)
                    return MAG_NATIVE_INVALID;
                samplers[t->stage] |= bit;
                sampler_kinds[t->stage][t->sampler_slot] = t->sampler;
            }
        }
        if(bounds_snapshot){
            for(uint32_t draw=0;draw<p->draw_count;++draw){
                const mag_native_raster_state *state=NULL;int status=registry4->raster(r->context,p->draws[draw].pipeline,&state);if(status)return status;
                const mag_native_scene_graphics_desc *schema=NULL;uint32_t model=0;status=r->graphics(r->context,p->draws[draw].pipeline,&schema,&model);if(status)return status;
                if(state?(state->depth_enabled&&state->depth_write):schema->depth_write)return MAG_NATIVE_UNSUPPORTED;
            }
            const mag_native_scene_image *depth=&s->images[p->depth.image];uint64_t row,rows,bytes;
            if(!mag_native_image_level_layout(depth->format,depth->width,depth->height,&row,&rows,&bytes)||add_bytes(u,bytes))return MAG_NATIVE_ALLOCATION_FAILED;
        }
        for (unsigned k = 0; k < 2; ++k)
            if (attachments[k]->image != MAG_NATIVE_SCENE_NONE)
                initialized[attachments[k]->image] = attachments[k]->store == MAG_NATIVE_SCENE_STORE && attachments[k]->load!=MAG_NATIVE_SCENE_DISCARD_LOAD;
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
        if(im->format!=MAG_NATIVE_SCENE_RGBA8&&im->format!=MAG_NATIVE_SCENE_BGRA8)return MAG_NATIVE_UNSUPPORTED;
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

int mag_native_scene_graphics_v3_validate(const mag_native_scene_graphics_v3_desc *d) {
 if(!d||d->struct_size!=sizeof(*d)||d->abi_version!=3)return MAG_NATIVE_INVALID;
 if(d->base.base.depth_compare||d->base.base.depth_write||d->base.base.blend)return MAG_NATIVE_INVALID;
 int status=mag_native_scene_graphics_v2_validate(&d->base);if(status)return status;
 if(d->raster.stencil_enabled&&!mag_native_image_has_stencil(d->base.base.depth_format))return MAG_NATIVE_UNSUPPORTED;
 return mag_native_raster_validate(&d->raster);
}
void mag_native_scene_graphics_v3_schema(const mag_native_scene_graphics_v3_desc *d,mag_native_scene_graphics_desc *out){
 *out=d->base.base;
 out->depth_compare=!d->raster.depth_enabled||!out->depth_format?0:d->raster.depth_compare==1?1:d->raster.depth_compare==3?2:3;
 out->depth_write=d->raster.depth_enabled&&out->depth_format?d->raster.depth_write:0;
 out->blend=out->color_format&&d->raster.blend_enabled?1:0;
}

int mag_native_scene_validate(const mag_native_scene *scene,const mag_native_scene_registry *registry,mag_native_scene_usage *usage){
    return scene_validate(scene,registry,usage,NULL,NULL,NULL,NULL,NULL,NULL);
}
int mag_native_scene_validate_v2(const mag_native_scene *scene,const mag_native_scene_registry_v2 *v2,mag_native_scene_usage *usage){
    if(!v2||!v2->base||v2->sampler_count>MAG_NATIVE_MAX_SAMPLERS||(v2->sampler_count&&(!v2->samplers||!v2->sampler)))return MAG_NATIVE_INVALID;
    for(uint32_t i=0;i<v2->sampler_count;++i){
        const mag_native_sampler_state *state=NULL;int status=v2->sampler(v2->base->context,v2->samplers[i],&state);
        if(status)return status;
        if(!state)return MAG_NATIVE_INVALID;
    }
    return scene_validate(scene,v2->base,usage,v2,NULL,NULL,NULL,NULL,NULL);
}

static int scene_validate_extended(const mag_native_scene_v3 *v3,const mag_native_scene_registry_v2 *v2,mag_native_scene_usage *usage,const mag_native_scene_v4 *v4,const mag_native_scene_registry_v4 *registry4,const mag_native_scene_v5 *v5,const mag_native_scene_registry_v5 *registry5){
    if(!v3||v3->struct_size!=sizeof(*v3)||v3->abi_version!=3||v3->reserved||
       v3->base.struct_size!=sizeof(v3->base)||v3->base.abi_version!=2||v3->base.reserved||!v3->base.scene||
       (v3->image_layout_count&&(v3->image_layout_count!=v3->base.scene->image_count||!v3->image_layouts))||
       v3->view_count>MAG_NATIVE_SCENE_MAX_VIEWS||(v3->view_count&&!v3->views)||
       (v3->viewport_count&&(v3->viewport_count!=v3->base.scene->pass_count||!v3->viewports))||
       !v2||!v2->base||v2->sampler_count!=v3->base.sampler_count||v2->samplers!=v3->base.samplers||
       v2->sampler_count>MAG_NATIVE_MAX_SAMPLERS||(v2->sampler_count&&(!v2->samplers||!v2->sampler)))return MAG_NATIVE_INVALID;
    for(uint32_t i=0;i<v2->sampler_count;++i){const mag_native_sampler_state *state=NULL;
        int status=v2->sampler(v2->base->context,v2->samplers[i],&state);if(status)return status;if(!state)return MAG_NATIVE_INVALID;}
    return scene_validate(v3->base.scene,v2->base,usage,v2,v3,v4,registry4,v5,registry5);
}

int mag_native_scene_validate_v3(const mag_native_scene_v3 *v3,const mag_native_scene_registry_v2 *v2,mag_native_scene_usage *usage){return scene_validate_extended(v3,v2,usage,NULL,NULL,NULL,NULL);}
int mag_native_scene_validate_v4(const mag_native_scene_v4 *v4,const mag_native_scene_registry_v4 *registry,mag_native_scene_usage *usage){
 if(!v4||v4->struct_size!=sizeof(*v4)||v4->abi_version!=4||v4->reserved||!registry||!registry->base||!registry->raster||!v4->base.base.scene||v4->stencil_count!=v4->base.base.scene->pass_count||!v4->stencils)return MAG_NATIVE_INVALID;
 return scene_validate_extended(&v4->base,registry->base,usage,v4,registry,NULL,NULL);
}

int mag_native_scene_validate_v5(const mag_native_scene_v5 *v5,const mag_native_scene_registry_v5 *registry,mag_native_scene_usage *usage){
 if(!v5||v5->struct_size!=sizeof(*v5)||v5->abi_version!=5||v5->reserved||v5->reserved1||!registry||!registry->base||!registry->image||!registry->base->base||!registry->base->raster)return MAG_NATIVE_INVALID;
 const mag_native_scene_v4 *v4=&v5->base;const mag_native_scene *s=v4->base.base.scene;
 if(!s||s->struct_size!=sizeof(*s)||s->abi_version!=1||!s->passes||!s->pass_count||s->pass_count>MAG_NATIVE_SCENE_MAX_PASSES||v4->struct_size!=sizeof(*v4)||v4->abi_version!=4||v4->reserved||!v4->stencils||v4->stencil_count!=s->pass_count||v5->import_count>s->image_count||(v5->import_count&&!v5->imports))return MAG_NATIVE_INVALID;
 uint32_t draws=0;for(uint32_t i=0;i<s->pass_count;++i){if(s->passes[i].draw_count>MAG_NATIVE_SCENE_MAX_DRAWS-draws)return MAG_NATIVE_INVALID;draws+=s->passes[i].draw_count;}
 if(v5->bounds_count&&(v5->bounds_count!=draws||!v5->bounds))return MAG_NATIVE_INVALID;
 return scene_validate_extended(&v4->base,registry->base->base,usage,v4,registry->base,v5,registry);
}
