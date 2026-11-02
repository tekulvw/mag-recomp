#include "mag_render_sampler.h"
#include <math.h>
int mag_native_sampler_state_validate(const mag_native_sampler_state *s) {
    if (!s || s->min_filter > 1 || s->mag_filter > 1 || s->mip_filter > 2 || s->wrap_s > 3 || s->wrap_t > 3 ||
        s->wrap_r > 3 || s->comparison_enabled > 1 || s->comparison > 7 || !isfinite(s->lod_min) ||
        !isfinite(s->lod_max) || !isfinite(s->lod_bias) || !isfinite(s->max_anisotropy) || s->lod_min < 0 ||
        s->lod_max < s->lod_min || s->max_anisotropy < 1 || s->max_anisotropy > 16 ||
        floorf(s->max_anisotropy) != s->max_anisotropy)
        return MAG_NATIVE_INVALID;
    for (unsigned i = 0; i < 4; ++i)
        if (!isfinite(s->border[i]))
            return MAG_NATIVE_INVALID;
    if (s->known_fields != MAG_NATIVE_SAMPLER_KNOWN_ALL)
        return MAG_NATIVE_UNSUPPORTED;
    if (s->wrap_s == 3 || s->wrap_t == 3 || s->wrap_r == 3) {
        int black = s->border[0] == 0 && s->border[1] == 0 && s->border[2] == 0 &&
                    (s->border[3] == 0 || s->border[3] == 1);
        int white = s->border[0] == 1 && s->border[1] == 1 && s->border[2] == 1 && s->border[3] == 1;
        if (!black && !white)
            return MAG_NATIVE_UNSUPPORTED;
    }
    return MAG_NATIVE_OK;
}
