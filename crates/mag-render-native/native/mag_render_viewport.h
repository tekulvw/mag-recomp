#ifndef MAG_RENDER_VIEWPORT_H
#define MAG_RENDER_VIEWPORT_H
#include <stdint.h>
#include <math.h>
/* Canonical D3D/Metal top-left viewport. Vulkan applies its one existing Y
 * conversion when encoding. Source jitter is retained as fractional x/y; this
 * does not move geometry in the shader or alter homogeneous clipping. */
typedef struct mag_native_viewport {
    float x,y,width,height,min_depth,max_depth;
    uint32_t scissor_x,scissor_y,scissor_width,scissor_height;
    uint32_t known_fields,reserved;
} mag_native_viewport;
#define MAG_NATIVE_VIEWPORT_KNOWN_RECT 1u
#define MAG_NATIVE_VIEWPORT_KNOWN_DEPTH 2u
#define MAG_NATIVE_VIEWPORT_KNOWN_SCISSOR 4u
#define MAG_NATIVE_VIEWPORT_KNOWN_ALL 7u
static inline int mag_native_viewport_valid(const mag_native_viewport *v,uint32_t width,uint32_t height){
    if(!v||v->reserved||v->known_fields!=MAG_NATIVE_VIEWPORT_KNOWN_ALL||!width||!height||
       !isfinite(v->x)||!isfinite(v->y)||!isfinite(v->width)||!isfinite(v->height)||
       !isfinite(v->min_depth)||!isfinite(v->max_depth)||v->width<=0||v->height<=0||
       v->width>16384||v->height>16384||v->x < -32768||v->y < -32768||
       v->x+v->width>32767||v->y+v->height>32767||
       v->min_depth<0||v->max_depth>1||v->min_depth>v->max_depth||
       v->scissor_x>width||v->scissor_y>height||v->scissor_width>width-v->scissor_x||
       v->scissor_height>height-v->scissor_y)return 0;
    return 1;
}
#endif
