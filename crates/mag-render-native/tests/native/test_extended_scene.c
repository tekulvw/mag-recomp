#ifdef NDEBUG
#undef NDEBUG
#endif
#define main bgra_fixture_main
#include "test_bgra_scene.c"
#undef main
static int extended_buffer(void *c,mag_native_buffer b,mag_native_scene_buffer_info *out){
    if(b.device_generation==7&&b.id==3&&b.version==1){*out=(mag_native_scene_buffer_info){288,0,0,0};return 0;}
    return buffer_info(c,b,out);
}
int main(void){
    const mag_native_buffer vertices={7,1,1},indices={7,2,1},pixels={7,3,1};
    mag_native_batch_resource resources[]={{vertices,MAG_NATIVE_UPLOAD,0},{indices,MAG_NATIVE_UPLOAD,0},{pixels,MAG_NATIVE_UPLOAD,0}};
    mag_native_scene_image images[]={{MAG_NATIVE_SCENE_RGBA16F,8,8,0,{0}},{MAG_NATIVE_SCENE_BC1,4,4,0,pixels}};
    mag_native_image_subresource levels[18];
    for(unsigned layer=0;layer<6;++layer)for(unsigned mip=0;mip<3;++mip){
        unsigned n=layer*3+mip,w=4u>>mip;
        levels[n]=(mag_native_image_subresource){mip,layer,w,w,1,8,n*16,8};
    }
    mag_native_scene_image_layout layouts[]={{2,1,1,1,1,0,NULL,0,0},{4,1,6,3,1,0,levels,18,0}};
    mag_native_image_view view={1,4,1,2,0,6,{2,1,0,5},0};
    mag_native_scene_texture_layout texture={1,0,0,MAG_NATIVE_SCENE_BC1,0,0};
    graphics=(mag_native_scene_graphics_desc){.struct_size=sizeof(graphics),.abi_version=1,.vertex_stride=16,
        .color_format=MAG_NATIVE_SCENE_RGBA16F,.fragment={.source_length=1},.textures=&texture,.texture_count=1};
    graphics_model=1;
    mag_native_scene_texture_binding binding={1,0,0,MAG_NATIVE_SCENE_VIEW_TABLE_BIT,0,0};
    mag_native_scene_draw draw={.pipeline={7,1},.vertices=vertices,.indices=indices,.vertex_length=48,.index_length=12,.index_count=3,.textures=&binding,.texture_count=1};
    mag_native_scene_pass pass={.color={0,MAG_NATIVE_SCENE_CLEAR,MAG_NATIVE_SCENE_STORE,0,{4,-2,0,1}},.depth={.image=MAG_NATIVE_SCENE_NONE},.draws=&draw,.draw_count=1};
    mag_native_scene scene={.struct_size=sizeof(scene),.abi_version=1,.resources=resources,.resource_count=3,.images=images,.image_count=2,.passes=&pass,.pass_count=1,.output_image=0};
    mag_native_viewport viewport={.x=0.25f,.y=-0.5f,.width=8,.height=8,.min_depth=0,.max_depth=1,.scissor_width=8,.scissor_height=8,.known_fields=MAG_NATIVE_VIEWPORT_KNOWN_ALL};
    mag_native_scene_v3 v3={.struct_size=sizeof(v3),.abi_version=3,.base={.struct_size=sizeof(mag_native_scene_v2),.abi_version=2,.scene=&scene},.image_layouts=layouts,.image_layout_count=2,.view_count=1,.views=&view,.viewports=&viewport,.viewport_count=1};
    mag_native_scene_registry registry={NULL,extended_buffer,compute_info,graphics_info,4,4,UINT32_MAX,UINT32_MAX};
    mag_native_scene_registry_v2 r2={&registry,NULL,0,NULL};mag_native_scene_usage usage;
    assert(mag_native_scene_validate_v3(&v3,&r2,&usage)==0);
    levels[17].offset=288; /* Complete resource must fit actual upload generation. */
    assert(mag_native_scene_validate_v3(&v3,&r2,&usage)==MAG_NATIVE_INVALID);levels[17].offset=272;
    binding.image=MAG_NATIVE_SCENE_VIEW_TABLE_BIT|1;
    assert(mag_native_scene_validate_v3(&v3,&r2,&usage)==MAG_NATIVE_INVALID);binding.image=MAG_NATIVE_SCENE_VIEW_TABLE_BIT;
    viewport.scissor_x=1;
    assert(mag_native_scene_validate_v3(&v3,&r2,&usage)==MAG_NATIVE_INVALID);viewport.scissor_x=0;
    viewport.scissor_width=0;
    assert(mag_native_scene_validate_v3(&v3,&r2,&usage)==0);viewport.scissor_width=8;
    view=(mag_native_image_view){0,2,0,1,0,1,{0,1,2,3},0};texture.format=MAG_NATIVE_SCENE_RGBA16F;
    assert(mag_native_scene_validate_v3(&v3,&r2,&usage)==MAG_NATIVE_INVALID); /* View alias cannot sample its own attachment. */
    puts("PASS: real V3 admission preserves full cube/mips, remap, HDR clear and viewport; rejects overflow, missing views and attachment aliases");
    return 0;
}
