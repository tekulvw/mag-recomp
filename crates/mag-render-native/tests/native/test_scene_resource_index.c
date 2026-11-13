/* CPU-only equivalence of the actual shared validator's linear/index routes.
 * Synthetic scene/token data only; no backend, device or GPU is created. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "mag_render_scene_validate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { RESOURCES=408, DRAWS=1700, EVENTS=20000 };
typedef struct event {uint64_t kind,generation,id,version;} event;
static event events[EVENTS],baseline[EVENTS];
static unsigned event_count,buffer_calls,fail_at,record_events=1;
static int fail_status;
static uint64_t produced_id,readback_id,pixel_id;
static mag_native_binding_layout compute_layout[2];
static mag_native_binding compute_bindings[2];
static mag_native_frame compute_frame;
static mag_native_scene_buffer_info info={64,0,0,0};
static mag_native_scene_graphics_desc graphics;
static mag_native_scene_buffer_layout layout={0};
static mag_native_batch_resource resources[MAG_NATIVE_SCENE_MAX_RESOURCES];
static mag_native_scene_draw draws[DRAWS];
static mag_native_scene_buffer_binding bindings[DRAWS];
static mag_native_scene_image image;
static mag_native_scene_pass pass;
static mag_native_scene scene;
static mag_native_scene_usage baseline_usage,usage;
static void flag(const char *value){
#ifdef _WIN32
    assert(!_putenv_s("MAG_RENDER_SCENE_RESOURCE_INDEX",value?value:""));
#else
    if(value)assert(!setenv("MAG_RENDER_SCENE_RESOURCE_INDEX",value,1));
    else assert(!unsetenv("MAG_RENDER_SCENE_RESOURCE_INDEX"));
#endif
}
static void note(event e){if(record_events){assert(event_count<EVENTS);events[event_count++]=e;}}
static int buffer_info(void *context,mag_native_buffer token,mag_native_scene_buffer_info *out){
    (void)context;note((event){1,token.device_generation,token.id,token.version});
    if(++buffer_calls==fail_at)return fail_status;
    *out=info;
    if(produced_id && token.id==produced_id)out->seed_zero=1;
    if(readback_id && token.id==readback_id){out->seed_zero=1;out->length=256;}
    if(pixel_id && token.id==pixel_id)out->length=256;
    return 0;
}
static int compute_info(void *context,mag_native_pipeline token,mag_native_scene_compute_info *out){
    (void)context;note((event){3,token.device_generation,token.id,0});
    *out=(mag_native_scene_compute_info){compute_layout,2,{1,1,1}};return 0;
}
static int graphics_info(void *context,mag_native_pipeline token,const mag_native_scene_graphics_desc **out,uint32_t *model){
    (void)context;note((event){2,token.device_generation,token.id,0});*out=&graphics;*model=0;return 0;
}
static mag_native_scene_registry registry={NULL,buffer_info,compute_info,graphics_info,4,4,UINT32_MAX,UINT32_MAX};
static void build(unsigned count,unsigned draw_count){
    assert(count && count<=MAG_NATIVE_SCENE_MAX_RESOURCES && draw_count<=DRAWS);
    info=(mag_native_scene_buffer_info){64,0,0,0};fail_at=0;fail_status=0;
    produced_id=readback_id=pixel_id=0;
    for(unsigned i=0;i<count;++i)resources[i]=(mag_native_batch_resource){{7,i+1,1},MAG_NATIVE_UPLOAD,0};
    layout=(mag_native_scene_buffer_layout){.stage=0,.binding={.slot=0,.access=MAG_NATIVE_READ,.minimum_bytes=16,.alignment_bytes=4,.runtime_size_index=MAG_NATIVE_NO_SIZE_INDEX}};
    graphics=(mag_native_scene_graphics_desc){.struct_size=sizeof(graphics),.abi_version=1,.vertex_stride=16,
        .color_format=MAG_NATIVE_SCENE_RGBA8,.buffers=&layout,.buffer_count=1};
    for(unsigned i=0;i<draw_count;++i){
        bindings[i]=(mag_native_scene_buffer_binding){.stage=0,.binding={.slot=0,.buffer=resources[(3*i+2)%count].buffer,.byte_length=16}};
        draws[i]=(mag_native_scene_draw){.pipeline={7,1},.vertices=resources[(3*i)%count].buffer,
            .indices=resources[(3*i+1)%count].buffer,.vertex_length=48,.index_length=12,.index_count=3,.buffers=&bindings[i],.buffer_count=1};
    }
    image=(mag_native_scene_image){MAG_NATIVE_SCENE_RGBA8,8,8,0,{0}};
    pass=(mag_native_scene_pass){.color={0,MAG_NATIVE_SCENE_CLEAR,MAG_NATIVE_SCENE_STORE,0,{0,0,0,1}},
        .depth={.image=MAG_NATIVE_SCENE_NONE},.draws=draws,.draw_count=draw_count};
    scene=(mag_native_scene){.struct_size=sizeof(scene),.abi_version=1,.resources=resources,.resource_count=count,
        .images=&image,.image_count=1,.passes=&pass,.pass_count=1,.output_image=0};
}
static unsigned cases;
static void equivalent(const char *name,int expected){
    flag(NULL);event_count=buffer_calls=0;memset(&baseline_usage,0xa5,sizeof(baseline_usage));
    int before=mag_native_scene_validate(&scene,&registry,&baseline_usage);
    const unsigned count=event_count;memcpy(baseline,events,count*sizeof(event));
    flag("1");event_count=buffer_calls=0;memset(&usage,0xa5,sizeof(usage));
    int after=mag_native_scene_validate(&scene,&registry,&usage);
    if(before!=expected || after!=before)fprintf(stderr,"FAIL %s: expected %d, OFF %d ON %d\n",name,expected,before,after);
    assert(before==expected && after==before);
    assert(!memcmp(&baseline_usage,&usage,sizeof(usage)));
    assert(event_count==count && !memcmp(events,baseline,count*sizeof(event)));
    ++cases;printf("PASS %s: status, complete usage/budgets and registry callback order identical\n",name);
}
int main(int argc,char **argv){
    build(RESOURCES,DRAWS);equivalent("408 resources /1700 indexed draws and bindings",MAG_NATIVE_OK);
    assert(usage.bytes==26368);for(unsigned i=0;i<RESOURCES;++i)assert(usage.read[i] && !usage.write[i]);
    draws[0].vertices.id=999999;equivalent("missing vertex token",MAG_NATIVE_INVALID);
    build(8,8);draws[0].vertices.device_generation^=1;equivalent("generation is part of identity",MAG_NATIVE_INVALID);
    build(8,8);draws[0].indices.version^=1;equivalent("version is part of identity",MAG_NATIVE_INVALID);
    build(2,1);resources[1].buffer=resources[0].buffer;resources[1].buffer.device_generation^=UINT64_C(1)<<48;
    draws[0].indices=resources[1].buffer;equivalent("high generation bits distinguish declared resources",MAG_NATIVE_OK);
    build(2,1);resources[1].buffer=resources[0].buffer;resources[1].buffer.id^=UINT64_C(1)<<48;
    draws[0].indices=resources[1].buffer;equivalent("high id bits distinguish declared resources",MAG_NATIVE_OK);
    build(2,1);resources[1].buffer=resources[0].buffer;resources[1].buffer.version^=UINT64_C(1)<<48;
    draws[0].indices=resources[1].buffer;equivalent("high version bits distinguish declared resources",MAG_NATIVE_OK);
    build(8,8);resources[7].buffer=resources[0].buffer;equivalent("late exact duplicate",MAG_NATIVE_INVALID);assert(event_count==7);
    build(8,8);resources[3].buffer=resources[0].buffer;fail_at=2;fail_status=MAG_NATIVE_STALE;
    equivalent("earlier registry error precedes later duplicate",MAG_NATIVE_STALE);assert(event_count==2);
    build(8,8);resources[1].reserved=1;resources[7].buffer=resources[0].buffer;
    equivalent("earlier malformed role precedes later duplicate",MAG_NATIVE_INVALID);assert(event_count==1);
    build(8,8);resources[7].role=MAG_NATIVE_READBACK;equivalent("readback cannot be used as vertex/input",MAG_NATIVE_INVALID);
    build(8,8);info=(mag_native_scene_buffer_info){64,0,1,0};equivalent("GPU-written upload refused",MAG_NATIVE_INVALID);
    build(8,8);info.busy=1;equivalent("immutable busy upload still admitted",MAG_NATIVE_OK);
    build(8,8);info.length=MAG_NATIVE_SCENE_MAX_BYTES;equivalent("unchanged byte budget",MAG_NATIVE_ALLOCATION_FAILED);
    build(8,8);draws[0].vertex_offset=UINT64_MAX-3;equivalent("overflowing vertex range",MAG_NATIVE_INVALID);
    build(8,8);bindings[0].binding.buffer.id=999999;equivalent("missing draw binding",MAG_NATIVE_INVALID);
    build(8,8);scene.resource_count=MAG_NATIVE_SCENE_MAX_RESOURCES+1;
    equivalent("oversize resource count rejected before usage/callback writes",MAG_NATIVE_INVALID);assert(!event_count && usage.bytes==UINT64_C(0xa5a5a5a5a5a5a5a5));
    build(1,1);resources[0].buffer=(mag_native_buffer){0};draws[0].vertices=draws[0].indices=bindings[0].binding.buffer=resources[0].buffer;
    equivalent("zero token remains registry-authoritative, not an empty index slot",MAG_NATIVE_OK);
    build(8,8);scene.resources=NULL;equivalent("missing resource array",MAG_NATIVE_INVALID);
    build(8,8);pass.draw_count=MAG_NATIVE_SCENE_MAX_DRAWS+1;equivalent("oversize draw count",MAG_NATIVE_INVALID);
    build(3,1);resources[2].role=MAG_NATIVE_PRODUCED;produced_id=3;
    compute_layout[0]=(mag_native_binding_layout){.slot=0,.access=MAG_NATIVE_READ,.minimum_bytes=16,.alignment_bytes=4,.runtime_size_index=MAG_NATIVE_NO_SIZE_INDEX};
    compute_layout[1]=compute_layout[0];compute_layout[1].slot=1;compute_layout[1].access=MAG_NATIVE_READ_WRITE;
    compute_bindings[0]=(mag_native_binding){.slot=0,.buffer=resources[0].buffer,.byte_length=16};
    compute_bindings[1]=(mag_native_binding){.slot=1,.buffer=resources[2].buffer,.byte_length=16};
    compute_frame=(mag_native_frame){.struct_size=sizeof(compute_frame),.abi_version=MAG_NATIVE_ABI_VERSION,
        .pipeline={7,2},.bindings=compute_bindings,.binding_count=2,.groups={1,1,1}};
    scene.dispatches=&compute_frame;scene.dispatch_count=1;
    equivalent("compute READ/WRITE prefix and later produced draw input",MAG_NATIVE_OK);assert(usage.write[2] && usage.read[2]);
    compute_bindings[0].buffer.id=999999;equivalent("missing compute READ resource",MAG_NATIVE_INVALID);compute_bindings[0].buffer=resources[0].buffer;
    compute_bindings[1].buffer.version^=1;equivalent("missing compute WRITE version",MAG_NATIVE_INVALID);compute_bindings[1].buffer=resources[2].buffer;
    scene.dispatch_count=0;equivalent("produced input still requires earlier writer",MAG_NATIVE_INVALID);
    scene.dispatch_count=1;info.busy=1;equivalent("busy produced buffer still refuses",MAG_NATIVE_BUSY);
    build(2,1);pixel_id=1;image.upload=resources[0].buffer;
    equivalent("image upload lookup",MAG_NATIVE_OK);assert(usage.bytes==576);
    image.upload.version^=1;equivalent("missing image upload version",MAG_NATIVE_INVALID);
    build(3,1);resources[2].role=MAG_NATIVE_READBACK;readback_id=3;
    graphics.buffers=NULL;graphics.buffer_count=0;draws[0].buffers=NULL;draws[0].buffer_count=0;
    scene.diagnostic_readback=resources[2].buffer;
    equivalent("diagnostic readback lookup and padded byte budget",MAG_NATIVE_OK);assert(usage.write[2] && usage.bytes==2688);
    scene.diagnostic_readback.id=999999;equivalent("missing diagnostic readback",MAG_NATIVE_INVALID);

    /* Identical id, distinct full-width generation/version tokens. The index
     * hash's rotated generation/version deltas cancel, deliberately forcing
     * every entry into the SAME hash bucket; exact equality must decide. */
    build(8,8);
    for(unsigned i=0;i<8;++i)resources[i].buffer=(mag_native_buffer){7u^((uint64_t)i<<1),100,1u^((uint64_t)i<<44)};
    for(unsigned i=0;i<8;++i){draws[i].vertices=resources[i].buffer;draws[i].indices=resources[(i+1)%8].buffer;bindings[i].binding.buffer=resources[(i+2)%8].buffer;}
    equivalent("collisions and same-id distinct generation/version tokens",MAG_NATIVE_OK);
    resources[7].buffer=resources[2].buffer;equivalent("duplicate at end of collision chain",MAG_NATIVE_INVALID);assert(event_count==7);
    /* All4096 admitted buffers are referenced; draws cover3 resources each
     * without exceeding either resource or draw budgets. */
    build(MAG_NATIVE_SCENE_MAX_RESOURCES,DRAWS);equivalent("full4096-resource budget",MAG_NATIVE_OK);
    assert(usage.bytes==(uint64_t)MAG_NATIVE_SCENE_MAX_RESOURCES*64+256);
    if(argc>1 && !strcmp(argv[1],"--bench")){
        build(RESOURCES,DRAWS);record_events=0;
        enum { iterations=128 };double seconds[2]={0};
        for(unsigned mode=0;mode<2;++mode){flag(mode?"1":NULL);clock_t begin=clock();
            for(unsigned i=0;i<iterations;++i){buffer_calls=0;assert(mag_native_scene_validate(&scene,&registry,&usage)==0);}
            seconds[mode]=(double)(clock()-begin)/CLOCKS_PER_SEC;
        }
        printf("CPU microbenchmark same binary: OFF %.6f ms/validation, ON %.6f ms/validation (408 resources,1700 draws,%u iterations/arm); no GPU/runtime performance claim\n",
            seconds[0]*1000/iterations,seconds[1]*1000/iterations,iterations);
    }
    flag(NULL);printf("PASS scene resource lookup equivalence: %u cases; CPU-only\n",cases);return 0;
}
