#ifndef MAG_RENDER_IMAGE_FORMAT_H
#define MAG_RENDER_IMAGE_FORMAT_H
#include <stdint.h>
/* Additive IDs preserve scene ABI's0none/1RGBA8/2D32/3BGRA8. */
enum mag_native_extended_scene_format {
    MAG_NATIVE_SCENE_R8=4, MAG_NATIVE_SCENE_RG8=5,
    MAG_NATIVE_SCENE_BC1=6, MAG_NATIVE_SCENE_BC2=7, MAG_NATIVE_SCENE_BC3=8,
    MAG_NATIVE_SCENE_R16=9, MAG_NATIVE_SCENE_RG16=10,
    MAG_NATIVE_SCENE_RGBA16F=11, MAG_NATIVE_SCENE_RGBA32F=12,
    MAG_NATIVE_SCENE_R32F=13, MAG_NATIVE_SCENE_RG16F=14,
    MAG_NATIVE_SCENE_D24S8=15,MAG_NATIVE_SCENE_D32S8=16
};
typedef struct mag_native_image_format {
    uint32_t block_width, block_height, block_bytes;
    uint32_t sample_kind, color_attachment, depth_attachment;
} mag_native_image_format;
static inline int mag_native_image_format_info(uint32_t id, mag_native_image_format *out) {
    mag_native_image_format f={1,1,0,0,0,0};
    switch(id){
    case 1:case 3:f.block_bytes=4;f.color_attachment=1;break;
    case 2:case MAG_NATIVE_SCENE_D24S8:f.block_bytes=4;f.sample_kind=3;f.depth_attachment=1;break;
    case MAG_NATIVE_SCENE_D32S8:f.block_bytes=8;f.sample_kind=3;f.depth_attachment=1;break;
    case MAG_NATIVE_SCENE_R8:f.block_bytes=1;f.color_attachment=1;break;
    case MAG_NATIVE_SCENE_RG8:case MAG_NATIVE_SCENE_R16:f.block_bytes=2;f.color_attachment=1;break;
    case MAG_NATIVE_SCENE_RG16:case MAG_NATIVE_SCENE_R32F:case MAG_NATIVE_SCENE_RG16F:f.block_bytes=4;f.color_attachment=1;break;
    case MAG_NATIVE_SCENE_RGBA16F:f.block_bytes=8;f.color_attachment=1;break;
    case MAG_NATIVE_SCENE_RGBA32F:f.block_bytes=16;f.color_attachment=1;break;
    case MAG_NATIVE_SCENE_BC1:f.block_width=f.block_height=4;f.block_bytes=8;break;
    case MAG_NATIVE_SCENE_BC2:case MAG_NATIVE_SCENE_BC3:f.block_width=f.block_height=4;f.block_bytes=16;break;
    default:return 0;
    }
    if(!out)return 0;
    *out=f;return 1;
}
static inline int mag_native_image_has_stencil(uint32_t format){return format==MAG_NATIVE_SCENE_D24S8||format==MAG_NATIVE_SCENE_D32S8;}
/* Tight canonical payload math, including one full block for a sub4 mip.
 * No multiplication, ceil or caller-owned output wraps on malformed extents. */
static inline int mag_native_image_level_layout(uint32_t format,uint32_t width,uint32_t height,
                                                uint64_t *row_bytes,uint64_t *rows,uint64_t *bytes){
    mag_native_image_format f;
    if(!width||!height||!row_bytes||!rows||!bytes||!mag_native_image_format_info(format,&f))return 0;
    uint64_t columns=((uint64_t)width+f.block_width-1)/f.block_width;
    uint64_t r=((uint64_t)height+f.block_height-1)/f.block_height;
    uint64_t pitch=columns*f.block_bytes;
    if(r>UINT64_MAX/pitch)return 0;
    *row_bytes=pitch;*rows=r;*bytes=pitch*r;return 1;
}
#endif
