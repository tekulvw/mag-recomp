#include "mag_render_image_validate.h"
static int has_upload(mag_native_buffer b){return b.device_generation||b.id||b.version;}
int mag_native_image_layout_validate(const mag_native_scene_image *im,const mag_native_scene_image_layout *l,
                                     uint64_t *allocation_bytes,uint64_t *upload_extent){
    mag_native_image_format f;
    if(!im||!l||!allocation_bytes||!upload_extent||im->reserved||l->reserved||l->reserved1||
       !im->width||!im->height||im->width>8192||im->height>8192||!mag_native_image_format_info(im->format,&f))return MAG_NATIVE_INVALID;
    if((l->dimension!=2&&l->dimension!=4)||l->depth!=1||l->samples!=1)return MAG_NATIVE_UNSUPPORTED;
    if(l->layers!=(l->dimension==4?6u:1u)||(l->dimension==4&&im->width!=im->height)||!l->mip_count)return MAG_NATIVE_INVALID;
    uint32_t extent=im->width>im->height?im->width:im->height,max_mips=0;
    for(;extent;extent>>=1)++max_mips;
    if(l->mip_count>max_mips)return MAG_NATIVE_INVALID;
    int uploaded=has_upload(im->upload);
    if(uploaded&&(!im->upload.device_generation||!im->upload.id||!im->upload.version))return MAG_NATIVE_INVALID;
    uint32_t count=l->layers*l->mip_count;
    if(uploaded?(l->subresource_count!=count||!l->subresources):l->subresource_count!=0)return MAG_NATIVE_INVALID;
    if(!uploaded&&(l->dimension!=2||l->mip_count!=1||(!f.color_attachment&&!f.depth_attachment)))return MAG_NATIVE_UNSUPPORTED;
    uint64_t total=0,end=0;
    for(uint32_t layer=0;layer<l->layers;++layer)for(uint32_t mip=0;mip<l->mip_count;++mip){
        uint32_t w=im->width>>mip,h=im->height>>mip;if(!w)w=1;if(!h)h=1;
        uint64_t row,rows,bytes;
        if(!mag_native_image_level_layout(im->format,w,h,&row,&rows,&bytes)||bytes>MAG_NATIVE_SCENE_MAX_BYTES-total)return MAG_NATIVE_ALLOCATION_FAILED;
        total+=bytes;
        if(uploaded){
            const mag_native_image_subresource *s=&l->subresources[layer*l->mip_count+mip];
            if(s->mip!=mip||s->layer!=layer||s->width!=w||s->height!=h||s->depth!=1||
               s->row_bytes!=row||s->length!=bytes||s->offset%(f.block_bytes>4?f.block_bytes:4)||s->offset<end||s->length>UINT64_MAX-s->offset)return MAG_NATIVE_INVALID;
            end=s->offset+s->length;
        }
    }
    *allocation_bytes=total;*upload_extent=end;return MAG_NATIVE_OK;
}
int mag_native_image_view_validate(const mag_native_scene_image *im,const mag_native_scene_image_layout *l,
                                   const mag_native_image_view *v){
    if(!im||!l||!v||v->reserved||!v->mip_count||!v->layer_count||v->base_mip>=l->mip_count||
       v->mip_count>l->mip_count-v->base_mip||v->base_layer>=l->layers||v->layer_count>l->layers-v->base_layer)return MAG_NATIVE_INVALID;
    if(v->dimension==2){if(v->layer_count!=1)return MAG_NATIVE_UNSUPPORTED;}
    else if(v->dimension==4){if(l->dimension!=4||v->base_layer||v->layer_count!=6)return MAG_NATIVE_UNSUPPORTED;}
    else return MAG_NATIVE_UNSUPPORTED;
    for(unsigned i=0;i<4;++i)if(v->remap[i]>5)return MAG_NATIVE_INVALID;
    return MAG_NATIVE_OK;
}
