/* Actual common admission for source-bound existing-image requirements. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "mag_render_scene_validate.h"
#include <assert.h>
#include <stdio.h>
static uint32_t available=7,depth_width=4,depth_height=4;static int wrong_extent,alias;static unsigned queries;
static mag_native_raster_state raster;
static mag_native_scene_texture_layout texture={1,0,1,MAG_NATIVE_SCENE_RGBA8,0,0};
static mag_native_scene_graphics_desc graphics;
static int buffer(void *u,mag_native_buffer b,mag_native_scene_buffer_info *out){(void)u;if(b.device_generation!=7)return MAG_NATIVE_STALE;*out=(mag_native_scene_buffer_info){b.id==1?48:12,0,0,0};return 0;}
static int compute(void *u,mag_native_pipeline p,mag_native_scene_compute_info *out){(void)u;(void)p;(void)out;return MAG_NATIVE_STALE;}
static int graphic(void *u,mag_native_pipeline p,const mag_native_scene_graphics_desc **out,uint32_t *model){(void)u;if(p.device_generation!=7)return MAG_NATIVE_STALE;*out=&graphics;*model=1;return 0;}
static int state(void *u,mag_native_pipeline p,const mag_native_raster_state **out){(void)u;(void)p;*out=&raster;return 0;}
static int image(void *u,mag_native_image ref,uint32_t required,mag_native_import_info *out){(void)u;++queries;if(ref.device_generation!=7||!ref.id)return MAG_NATIVE_STALE;if((required&available)!=required)return MAG_NATIVE_STALE;*out=(mag_native_import_info){ref.id==11?MAG_NATIVE_SCENE_D32S8:MAG_NATIVE_SCENE_RGBA8,wrong_extent?8:(ref.id==11?depth_width:4),ref.id==11?depth_height:4,0,17,alias?99:ref.id,1};return 0;}
int main(void){
 graphics=(mag_native_scene_graphics_desc){.struct_size=sizeof(graphics),.abi_version=1,.vertex_stride=16,.color_format=MAG_NATIVE_SCENE_RGBA8,.fragment={.source_length=1},.textures=&texture,.texture_count=1};raster.known_fields=MAG_NATIVE_RASTER_KNOWN_ALL;raster.samples=1;
 mag_native_batch_resource resources[]={{{7,1,1},MAG_NATIVE_UPLOAD,0},{{7,2,1},MAG_NATIVE_UPLOAD,0}};
 mag_native_scene_image images[]={{MAG_NATIVE_SCENE_RGBA8,4,4,0,{0}},{MAG_NATIVE_SCENE_RGBA8,4,4,0,{0}},{MAG_NATIVE_SCENE_D32S8,4,4,0,{0}}};
 mag_native_scene_image_layout shapes[3];for(unsigned i=0;i<3;++i)shapes[i]=(mag_native_scene_image_layout){2,1,1,1,1,0,NULL,0,0};
 mag_native_scene_texture_binding tb={1,0,1,1,MAG_NATIVE_SCENE_NEAREST,0};
 mag_native_scene_draw draw={.pipeline={7,1},.vertices={7,1,1},.indices={7,2,1},.vertex_length=48,.index_length=12,.index_count=3,.textures=&tb,.texture_count=1};
 mag_native_scene_pass pass={.color={0,0,0,0,{0}},.depth={.image=MAG_NATIVE_SCENE_NONE},.draws=&draw,.draw_count=1};
 mag_native_stencil_attachment stencil={.image=MAG_NATIVE_SCENE_NONE};
 mag_native_scene s={.struct_size=sizeof(s),.abi_version=1,.resources=resources,.resource_count=2,.images=images,.image_count=2,.passes=&pass,.pass_count=1,.output_image=0};
 mag_native_scene_import imports[]={{1,1,{7,9}},{0,0,{7,10}},{2,6,{7,11}}};
 mag_native_scene_v5 v={.struct_size=sizeof(v),.abi_version=5,.base={.struct_size=sizeof(v.base),.abi_version=4,.base={.struct_size=sizeof(v.base.base),.abi_version=3,.base={sizeof(v.base.base.base),2,&s,NULL,0,0},.image_layouts=shapes,.image_layout_count=2},.stencils=&stencil,.stencil_count=1},.imports=imports,.import_count=1};
 mag_native_scene_registry r={NULL,buffer,compute,graphic,4,4,UINT32_MAX,UINT32_MAX};mag_native_scene_registry_v2 r2={.base=&r};mag_native_scene_registry_v4 r4={&r2,state};mag_native_scene_registry_v5 r5={&r4,image};mag_native_scene_usage usage;
 assert(mag_native_scene_validate_v5(&v,&r5,&usage)==0&&queries==1);
 imports[0].required_initialized_planes=0;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_INVALID);imports[0].required_initialized_planes=1;
 available=0;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_STALE);available=7;
 wrong_extent=1;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_STALE);wrong_extent=0;
 imports[0].ref.device_generation=8;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_STALE);imports[0].ref.device_generation=7;
 v.import_count=2;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==0);alias=1;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_INVALID);alias=0;
 s.image_count=3;v.base.base.image_layout_count=3;v.import_count=3;graphics.depth_format=MAG_NATIVE_SCENE_D32S8;
 pass.depth=(mag_native_scene_attachment){2,MAG_NATIVE_SCENE_LOAD,MAG_NATIVE_SCENE_STORE,0,{0}};stencil=(mag_native_stencil_attachment){2,MAG_NATIVE_STENCIL_LOAD,MAG_NATIVE_SCENE_STORE,0,0,0};
 assert(mag_native_scene_validate_v5(&v,&r5,&usage)==0);
 imports[2].required_initialized_planes=2;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_INVALID);imports[2].required_initialized_planes=6;
 imports[2].required_initialized_planes=1;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_INVALID);
 imports[2].required_initialized_planes=6;
 mag_native_depth_bounds bounds={1,0,0.2f,0.8f};v.bounds=&bounds;v.bounds_count=1;raster.depth_bounds_enabled=1;
 assert(mag_native_scene_validate_v5(&v,&r5,&usage)==0);
 assert(usage.bytes>=4u*4u*8u+16u); /* real snapshot plus range admission */
 v.bounds_count=0;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_INVALID);v.bounds_count=1;
 bounds.minimum=0.9f;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_INVALID);bounds.minimum=0.2f;
 pass.depth.load=MAG_NATIVE_SCENE_CLEAR;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_UNSUPPORTED);pass.depth.load=MAG_NATIVE_SCENE_LOAD;
 raster.depth_enabled=1;raster.depth_write=1;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_UNSUPPORTED);raster.depth_write=0;
 raster.stencil_enabled=1;raster.front_stencil.compare=7;raster.back_stencil.compare=7;raster.front_stencil.write_mask=255;
 assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_UNSUPPORTED);raster.front_stencil.write_mask=0;
 assert(mag_native_scene_validate_v5(&v,&r5,&usage)==0);
 /* Color4x4 render area on a distinct initialized8x8 physical depth. */
 depth_width=depth_height=8;images[2].width=images[2].height=8;
 assert(mag_native_scene_validate_v5(&v,&r5,&usage)==0);
 pass.depth.load=MAG_NATIVE_SCENE_CLEAR;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_UNSUPPORTED);pass.depth.load=MAG_NATIVE_SCENE_LOAD;
 stencil.load=MAG_NATIVE_STENCIL_CLEAR;assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_UNSUPPORTED);stencil.load=MAG_NATIVE_STENCIL_LOAD;
 depth_width=depth_height=2;images[2].width=images[2].height=2;
 assert(mag_native_scene_validate_v5(&v,&r5,&usage)==MAG_NATIVE_INVALID);
 puts("PASS existing-image initialization certificates, exact extents/generation, alias and separate planes");
}
