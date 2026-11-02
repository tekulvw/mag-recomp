#include "mag_render_native.h"
#include "mag_render_raster.h"
#include <math.h>
int mag_native_raster_validate(const mag_native_raster_state *s) {
    if (!s || s->reserved || s->front_stencil.reserved || s->back_stencil.reserved || s->cull_mode > 3 ||
        s->front_face > 1 || s->depth_enabled > 1 || s->depth_compare > 7 || s->depth_write > 1 ||
        s->blend_enabled > 1 || s->src_rgb > 14 || s->dst_rgb > 14 || s->src_alpha > 14 ||
        s->dst_alpha > 14 || s->rgb_equation > 4 || s->alpha_equation > 4 || s->color_write_mask > 15 ||
        s->stencil_enabled > 1 || s->depth_bounds_enabled > 1 || s->bias_enabled > 1 || s->topology > 4 ||
        s->samples == 0)
        return MAG_NATIVE_INVALID;
    for (unsigned i = 0; i < 4; ++i)
        if (!isfinite(s->blend_constant[i]))
            return MAG_NATIVE_INVALID;
    if (!isfinite(s->depth_bounds_min) || !isfinite(s->depth_bounds_max) || !isfinite(s->bias_constant) ||
        !isfinite(s->bias_slope) || !isfinite(s->bias_clamp))
        return MAG_NATIVE_INVALID;
    if (s->known_fields != MAG_NATIVE_RASTER_KNOWN_ALL || s->unresolved_obligations ||
        s->depth_bounds_enabled || s->bias_enabled || s->topology || s->samples != 1 ||
        s->cull_mode == MAG_NATIVE_CULL_BOTH)
        return MAG_NATIVE_UNSUPPORTED;
    /* D3D forbids COLOR factors for the alpha equation and source-alpha-saturate
     * as a destination factor. Require a portable, explicit representation. */
    if (s->blend_enabled &&
        (s->src_alpha == 2 || s->src_alpha == 3 || s->src_alpha == 4 || s->src_alpha == 5 ||
         s->src_alpha == 10 || s->src_alpha == 11 || s->src_alpha == 14 || s->dst_alpha == 2 ||
         s->dst_alpha == 3 || s->dst_alpha == 4 || s->dst_alpha == 5 || s->dst_alpha == 10 ||
         s->dst_alpha == 11 || s->dst_alpha == 14 || s->dst_rgb == 14))
        return MAG_NATIVE_UNSUPPORTED;
    if (s->blend_enabled) {
        int color = (s->src_rgb == 10 || s->src_rgb == 11 || s->dst_rgb == 10 || s->dst_rgb == 11);
        int alpha = (s->src_rgb == 12 || s->src_rgb == 13 || s->dst_rgb == 12 || s->dst_rgb == 13);
        if (color && alpha &&
            (s->blend_constant[0] != s->blend_constant[3] || s->blend_constant[1] != s->blend_constant[3] ||
             s->blend_constant[2] != s->blend_constant[3]))
            return MAG_NATIVE_UNSUPPORTED;
    }
    if(s->stencil_enabled){
        const mag_native_stencil_face *faces[]={&s->front_stencil,&s->back_stencil};
        for(unsigned i=0;i<2;++i){const mag_native_stencil_face *f=faces[i];
            if(f->compare>7||f->fail>7||f->depth_fail>7||f->pass>7)return MAG_NATIVE_INVALID;
        }
    }
    return MAG_NATIVE_OK;
}
