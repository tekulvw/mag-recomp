/* Whole-scene capacity is independent from the retained diagnostic batch ABI. */
#include "mag_render_scene_validate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static int buffer_info(void *c, mag_native_buffer b, mag_native_scene_buffer_info *out) {
    (void)c;
    if(b.device_generation!=7 || !b.id || b.version!=1) return MAG_NATIVE_STALE;
    *out=(mag_native_scene_buffer_info){4,0,0,0};return 0;
}
static int compute_info(void *c,mag_native_pipeline p,mag_native_scene_compute_info *out) {
    (void)c;(void)p;(void)out;return MAG_NATIVE_STALE;
}
static int graphics_info(void *c,mag_native_pipeline p,const mag_native_scene_graphics_desc **out,uint32_t *model) {
    (void)c;(void)p;(void)out;(void)model;return MAG_NATIVE_STALE;
}
int main(void) {
    enum { resource_count=65,image_count=66 };
    mag_native_batch_resource *resources=calloc(resource_count,sizeof(*resources));
    mag_native_scene_image *images=calloc(image_count,sizeof(*images));
    assert(resources&&images);
    for(unsigned i=0;i<resource_count;++i)resources[i]=(mag_native_batch_resource){{7,i+1,1},MAG_NATIVE_UPLOAD,0};
    for(unsigned i=0;i<image_count;++i)images[i]=(mag_native_scene_image){MAG_NATIVE_SCENE_RGBA8,1,1,0,{0}};
    for(unsigned i=1;i<image_count;++i)images[i].upload=resources[i-1].buffer;
    mag_native_scene_pass pass={.color={0,MAG_NATIVE_SCENE_CLEAR,MAG_NATIVE_SCENE_STORE,0,{0,0,0,1}},.depth={.image=MAG_NATIVE_SCENE_NONE}};
    mag_native_scene s={.struct_size=sizeof(s),.abi_version=MAG_NATIVE_SCENE_ABI,.resources=resources,.resource_count=resource_count,.images=images,.image_count=image_count,.passes=&pass,.pass_count=1,.output_image=0};
    mag_native_scene_registry r={NULL,buffer_info,compute_info,graphics_info,4,4,UINT32_MAX,UINT32_MAX};
    mag_native_scene_usage u;
    assert(mag_native_scene_validate(&s,&r,&u)==MAG_NATIVE_OK);
    assert(u.bytes==4*(resource_count+image_count));
    resources[64].buffer=resources[0].buffer;
    assert(mag_native_scene_validate(&s,&r,&u)==MAG_NATIVE_INVALID);
    resources[64].buffer.id=65;
    images[16].width=8193;
    assert(mag_native_scene_validate(&s,&r,&u)==MAG_NATIVE_INVALID);
    images[16].width=1;
    s.resource_count=MAG_NATIVE_SCENE_MAX_RESOURCES+1;
    assert(mag_native_scene_validate(&s,&r,&u)==MAG_NATIVE_INVALID);
    s.resource_count=resource_count;s.image_count=MAG_NATIVE_SCENE_MAX_IMAGES+1;
    assert(mag_native_scene_validate(&s,&r,&u)==MAG_NATIVE_INVALID);
    s.image_count=image_count;s.resource_count=0;s.resources=NULL;
    for(unsigned i=0;i<image_count;++i)images[i].upload=(mag_native_buffer){0};
    for(unsigned i=0;i<5;++i)images[i].width=images[i].height=8192;
    assert(mag_native_scene_validate(&s,&r,&u)==MAG_NATIVE_ALLOCATION_FAILED);
    assert(MAG_NATIVE_BATCH_MAX_RESOURCES==64); /* retained ABI budget unchanged */
    assert(MAG_NATIVE_BATCH_MAX_BYTES==64u*1024u*1024u);
    free(images);free(resources);
    puts("PASS: scene beyond legacy image/resource caps, full admission and retained batch budget");
    return 0;
}
