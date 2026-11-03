#include "mag_render_image_validate.h"
#include <assert.h>
int main(void){
    mag_native_scene_image image={MAG_NATIVE_SCENE_BC1,4,4,0,{1,2,3}};
    mag_native_image_subresource subs[18];
    for(unsigned face=0;face<6;++face)for(unsigned mip=0;mip<3;++mip){unsigned n=face*3+mip;uint32_t w=4u>>mip;
        subs[n]=(mag_native_image_subresource){mip,face,w,w,1,8,n*8u,8};}
    mag_native_scene_image_layout shape={4,1,6,3,1,0,subs,18,0};uint64_t allocation=0,upload=0;
    assert(mag_native_image_layout_validate(&image,&shape,&allocation,&upload)==MAG_NATIVE_OK&&allocation==144&&upload==144);
    subs[17].offset=0;assert(mag_native_image_layout_validate(&image,&shape,&allocation,&upload)==MAG_NATIVE_INVALID);subs[17].offset=136;
    subs[17].row_bytes=4;assert(mag_native_image_layout_validate(&image,&shape,&allocation,&upload)==MAG_NATIVE_INVALID);subs[17].row_bytes=8;
    mag_native_image_view view={0,4,1,2,0,6,{0,1,2,3},0};
    assert(mag_native_image_view_validate(&image,&shape,&view)==MAG_NATIVE_OK);
    view.base_mip=2;assert(mag_native_image_view_validate(&image,&shape,&view)==MAG_NATIVE_INVALID);view.base_mip=1;
    view.dimension=2;view.base_layer=5;view.layer_count=1;assert(mag_native_image_view_validate(&image,&shape,&view)==MAG_NATIVE_OK);
    view.remap[0]=6;assert(mag_native_image_view_validate(&image,&shape,&view)==MAG_NATIVE_INVALID);
    image.upload=(mag_native_buffer){0,0,0};shape.subresources=0;shape.subresource_count=0;
    assert(mag_native_image_layout_validate(&image,&shape,&allocation,&upload)==MAG_NATIVE_UNSUPPORTED);
    return 0;
}
