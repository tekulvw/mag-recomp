#include "mag_render_image_format.h"
#include <assert.h>
int main(void){
    mag_native_image_format f;
    assert(mag_native_image_format_info(MAG_NATIVE_SCENE_BC1,&f)&&f.block_bytes==8&&!f.color_attachment);
    assert(mag_native_image_format_info(MAG_NATIVE_SCENE_RGBA16F,&f)&&f.block_bytes==8&&f.color_attachment);
    assert(mag_native_image_format_info(2,&f)&&f.sample_kind==3&&f.depth_attachment&&!f.color_attachment);
    uint64_t row=99,rows=99,bytes=99;
    assert(mag_native_image_level_layout(MAG_NATIVE_SCENE_BC3,5,7,&row,&rows,&bytes)&&row==32&&rows==2&&bytes==64);
    assert(mag_native_image_level_layout(MAG_NATIVE_SCENE_BC1,1,1,&row,&rows,&bytes)&&row==8&&rows==1&&bytes==8);
    assert(mag_native_image_level_layout(MAG_NATIVE_SCENE_RGBA16F,2,3,&row,&rows,&bytes)&&row==16&&rows==3&&bytes==48);
    row=rows=bytes=99;
    assert(!mag_native_image_level_layout(MAG_NATIVE_SCENE_RGBA32F,UINT32_MAX,UINT32_MAX,&row,&rows,&bytes));
    assert(row==99&&rows==99&&bytes==99);
    assert(!mag_native_image_level_layout(99,1,1,&row,&rows,&bytes));
    assert(!mag_native_image_level_layout(1,0,1,&row,&rows,&bytes));
    return 0;
}
