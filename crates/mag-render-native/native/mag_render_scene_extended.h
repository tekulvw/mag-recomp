#ifndef MAG_RENDER_SCENE_EXTENDED_H
#define MAG_RENDER_SCENE_EXTENDED_H
#include "mag_render_scene.h"
#include "mag_render_image_format.h"
#include "mag_render_viewport.h"
#ifdef __cplusplus
extern "C" {
#endif
/* V3 keeps all existing resource generations and sampler-table qualification.
 * Every image-layout record corresponds to the same index in base.scene.images.
 * Zero layout_count preserves exact V1/V2 images; nonzero must equal image_count.
 * Each image uses its existing single immutable upload buffer; subresource
 * offsets refer to that exact generation-qualified buffer. An uploaded image
 * supplies every declared mip/layer, without repeated/overlapping byte ranges.
 * No descriptor or upload pointer survives the synchronous admission call. */
typedef struct mag_native_image_subresource {
    uint32_t mip,layer,width,height,depth,row_bytes;
    uint64_t offset,length;
} mag_native_image_subresource;
typedef struct mag_native_scene_image_layout {
    uint32_t dimension,depth,layers,mip_count,samples,reserved;
    const mag_native_image_subresource *subresources;
    uint32_t subresource_count,reserved1;
} mag_native_scene_image_layout;
typedef struct mag_native_image_view {
    uint32_t image,dimension,base_mip,mip_count,base_layer,layer_count;
    uint32_t remap[4]; /* R,G,B,A,ZERO,ONE =0..5 */
    uint32_t reserved;
} mag_native_image_view;
/* High-bit texture.image codes select a checked view in this table. Ordinary
 * image indices still mean the complete image with identity channel remap. */
#define MAG_NATIVE_SCENE_VIEW_TABLE_BIT 0x80000000u
#define MAG_NATIVE_SCENE_VIEW_TABLE_INDEX(code) ((code)&0x7fffffffu)
#define MAG_NATIVE_SCENE_MAX_VIEWS 4096u
typedef struct mag_native_scene_v3 {
    uint32_t struct_size,abi_version;
    mag_native_scene_v2 base;
    const mag_native_scene_image_layout *image_layouts;
    uint32_t image_layout_count,view_count;
    const mag_native_image_view *views;
    const mag_native_viewport *viewports;
    uint32_t viewport_count,reserved;
} mag_native_scene_v3;
static inline uint32_t mag_native_scene_image_index(const mag_native_scene_v3 *v3,uint32_t code){
    if(!(code&MAG_NATIVE_SCENE_VIEW_TABLE_BIT))return code;
    uint32_t index=MAG_NATIVE_SCENE_VIEW_TABLE_INDEX(code);
    return v3&&v3->views&&index<v3->view_count?v3->views[index].image:UINT32_MAX;
}
/* A nonempty viewport table has one actual viewport/scissor per pass. Empty
 * preserves existing full-image diagnostic behavior. No native default proves
 * source viewport ownership. All table entries validate before queue effects. */
int mag_native_submit_scene_v3(mag_native_device *,const mag_native_scene_v3 *,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *);
int mag_native_scene_v3_layout(uint32_t *,uint32_t);
/* six words: block_width,height,bytes,sample_kind,color_attachment,depth_attachment */
int mag_native_image_format_layout(uint32_t format,uint32_t *out,uint32_t words);
#ifdef __cplusplus
}
#endif
#endif
