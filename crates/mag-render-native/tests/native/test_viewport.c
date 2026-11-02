#include "mag_render_viewport.h"
#include <assert.h>
int main(void){
    mag_native_viewport v={-.25f,.125f,640,480,0,1,0,0,640,480,7,0};
    assert(mag_native_viewport_valid(&v,640,480));
    v.scissor_x=1;assert(!mag_native_viewport_valid(&v,640,480));
    v.scissor_width=639;assert(mag_native_viewport_valid(&v,640,480));
    v.scissor_width=0;assert(mag_native_viewport_valid(&v,640,480));
    v.scissor_x=UINT32_MAX;assert(!mag_native_viewport_valid(&v,640,480));v.scissor_x=0;
    v.known_fields=3;assert(!mag_native_viewport_valid(&v,640,480));v.known_fields=7;
    v.x=NAN;assert(!mag_native_viewport_valid(&v,640,480));v.x=0;
    v.min_depth=.7f;v.max_depth=.3f;assert(!mag_native_viewport_valid(&v,640,480));
    return 0;
}
