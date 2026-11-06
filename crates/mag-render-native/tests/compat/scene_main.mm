#import <Metal/Metal.h>
#include "mag_render_scene.h"
#include "metal_shader.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <unistd.h>
#include "mag_render_imported_image.h"
#include "mag_render_scene_packet.h"
#include <thread>
extern "C" int mag_render_test_owner_open(MagRenderHostOwner *);
extern "C" int mag_render_test_owner_gate(void);
extern "C" int mag_render_test_owner_image(MagRenderHostImage*);
extern "C" int mag_render_test_owner_image_check(void);
extern "C" const char *mag_render_test_owner_name(void);
extern "C" void mag_render_test_owner_signal(void *);
extern "C" void mag_render_test_owner_close(int);
struct SceneProbeReport {
    uint32_t abi_version, struct_size;
    int32_t status;
    uint32_t submitted, completed, pending;
    uint64_t retained_before_terminal, retained_after_terminal;
    uint32_t center_rgba, ui_rgba, shadow_rgba, outside_rgba, pass_count, draw_count;
};
extern "C" int mag_render_native_scene_probe_target(const MagRenderHostOwner *, const mag_native_pipeline_desc *,
                                             const mag_native_pipeline_desc *,
                                             const mag_native_scene_graphics_desc *, const MagRenderHostImage*,uint32_t,
                                             void (*)(void *), void *, SceneProbeReport *);
struct ReservedProbeReport {
    uint32_t abi_version, struct_size;
    int32_t status;
    uint32_t identical;
    uint64_t differing_bytes, chunked_submissions, chunk_slots, chunks_used;
};
extern "C" int mag_render_native_scene_probe_reserved(const MagRenderHostOwner *, const mag_native_pipeline_desc *,
                                                      const mag_native_pipeline_desc *,
                                                      const mag_native_scene_graphics_desc *, const MagRenderHostImage *,
                                                      ReservedProbeReport *);
struct MemoryProbeReport {
    uint32_t abi_version, struct_size;
    int32_t status;
    uint32_t frames;
    uint64_t warm[9], last[9];
};
extern "C" int mag_render_native_scene_probe_memory(const MagRenderHostOwner *, const mag_native_pipeline_desc *,
                                                    const mag_native_pipeline_desc *,
                                                    const mag_native_scene_graphics_desc *, uint32_t, uint32_t,
                                                    MemoryProbeReport *);
/* Many sequential submissions with distinct per-frame content on the real
 * queue: finished submissions must be released, native object counts must
 * stay flat and the device allocation must not grow with the frame count. */
static int check_memory_flat(const MagRenderHostOwner *owner,const mag_native_pipeline_desc *first,
                             const mag_native_pipeline_desc *second,const mag_native_scene_graphics_desc *graphics,
                             NSMutableDictionary *result){
    MemoryProbeReport r={};r.abi_version=1;r.struct_size=sizeof(r);
    const uint32_t frames=240,warm=40;
    const int status=mag_render_native_scene_probe_memory(owner,first,second,graphics,frames,warm,&r);
    const double growth=((double)r.last[0]-(double)r.warm[0])/1048576.0;
    fprintf(stderr,"[memory flat] status=%d frames=%u allocated_mb warm=%.2f last=%.2f growth=%.2f buffers=%llu/%llu pipelines=%llu/%llu images=%llu/%llu retired_alive=%llu/%llu\n",
        status,r.frames,(double)r.warm[0]/1048576.0,(double)r.last[0]/1048576.0,growth,
        (unsigned long long)r.warm[1],(unsigned long long)r.last[1],(unsigned long long)r.warm[2],(unsigned long long)r.last[2],
        (unsigned long long)r.warm[3],(unsigned long long)r.last[3],(unsigned long long)r.warm[7],(unsigned long long)r.last[7]);
    result[@"memory"]=@{@"status":@(status),@"frames":@(r.frames),@"allocated_growth_mb":@(growth),
        @"retired_alive":@(r.last[7]),@"buffers_flat":@(r.warm[1]==r.last[1]),@"pipelines_flat":@(r.warm[2]==r.last[2]),
        @"images_flat":@(r.warm[3]==r.last[3])};
    if(status)return status;
    return r.last[7]==0&&r.warm[1]==r.last[1]&&r.warm[2]==r.last[2]&&r.warm[3]==r.last[3]&&growth<4.0?0:-6;
}
static std::vector<mag_native_scene_buffer_layout> buffers(Shader &shader, uint32_t stage) {
    std::vector<mag_native_scene_buffer_layout> out;
    for (NSDictionary *b in shader.manifest[@"buffers"]) {
        if ([b[@"group"] unsignedIntValue] != stage || ![b[@"access"] isEqual:@"read"])
            return {};
        out.push_back({stage,
                       0,
                       {[b[@"binding"] unsignedIntValue],
                        MAG_NATIVE_READ,
                        [b[@"minimum_binding_bytes"] unsignedLongLongValue],
                        0,
                        MAG_NATIVE_NO_SIZE_INDEX,
                        [b[@"layout"][@"alignment_bytes"] unsignedIntValue],
                        [b[@"address_space"] isEqual:@"uniform"] ? MAG_NATIVE_UNIFORM : MAG_NATIVE_STORAGE}});
    }
    return out;
}
static void check_rejected_scenes(const MagRenderHostOwner *owner) {
    mag_native_device *device = NULL;
    assert(mag_native_create(owner, &device) == 0);
    mag_native_scene_image image = {MAG_NATIVE_SCENE_D32, 8, 8, 0, {}};
    mag_native_scene_pass pass = {};
    pass.color.image = MAG_NATIVE_SCENE_NONE;
    pass.depth.image = 0;
    pass.depth.load = MAG_NATIVE_SCENE_LOAD;
    pass.depth.clear[0] = 1;
    mag_native_scene scene = {};
    scene.struct_size = sizeof(scene);
    scene.abi_version = 1;
    scene.images = &image;
    scene.image_count = 1;
    scene.passes = &pass;
    scene.pass_count = 1;
    scene.output_image = MAG_NATIVE_SCENE_NONE;
    auto rejected = [&] {
        mag_native_ticket ticket = {};
        assert(mag_native_submit_scene(device, &scene, &ticket) == MAG_NATIVE_INVALID);
        assert(!ticket.device_generation && !ticket.submission_id);
        mag_native_event event = {};
        uint32_t count = 0;
        assert(mag_native_poll(device, &event, 1, &count) == 0 && !count);
    };
    rejected(); // Load before any clear/store.
    pass.depth.load = MAG_NATIVE_SCENE_CLEAR;
    pass.draw_count = 1;
    rejected(); // Missing draw list.
    pass.draw_count = 0;
    image.width = UINT32_MAX;
    rejected();
    image.width = 8;
    scene.output_image = 0;
    rejected(); // Depth cannot be a color output.
    scene.output_image = MAG_NATIVE_SCENE_NONE;
    pass.depth.clear[0] = NAN;
    rejected();
    pass.depth.clear[0] = 1;
    scene.pass_count = 0;
    rejected();
    scene.pass_count = 1;
    image.format = MAG_NATIVE_SCENE_RGBA8;
    rejected(); // Explicit attachment format.
    assert(mag_native_close(device) == 0);
}
/* Reserved submission order on the real queue: reservation validates the
 * import under the owner lease, a second reservation or close is BUSY, an
 * abandoned reservation releases its position, and an imported clear-only
 * scene encodes into its reservation and completes with the reserved ticket. */
static void check_reserved_submission(const MagRenderHostOwner *owner,const MagRenderHostImage *target){
    mag_native_device *device=NULL;assert(mag_native_create(owner,&device)==0);
    mag_native_image_certificate certificate={*target,NULL};mag_native_image ref={};
    assert(mag_native_image_import_certified(device,&certificate,&ref)==0);
    mag_native_scene_import import={0,0,ref};
    mag_native_ticket first={},busy={},second={};
    assert(mag_native_reserve_scene_v5(device,&import,1,&first)==0&&first.device_generation&&first.submission_id);
    assert(mag_native_reserve_scene_v5(device,&import,1,&busy)==MAG_NATIVE_BUSY&&!busy.submission_id);
    assert(mag_native_close(device)==MAG_NATIVE_BUSY);
    assert(mag_native_abandon_scene_reservation(device)==0);
    assert(mag_native_abandon_scene_reservation(device)==MAG_NATIVE_STALE);
    assert(mag_native_reserve_scene_v5(device,&import,1,&second)==0&&second.submission_id>first.submission_id);
    mag_native_scene_image image={MAG_NATIVE_SCENE_RGBA8,target->width,target->height,0,{}};
    mag_native_scene_pass pass={};
    pass.color.image=0;pass.color.load=MAG_NATIVE_SCENE_CLEAR;pass.color.store=1;
    pass.color.clear[0]=0.25f;pass.color.clear[3]=1;pass.depth.image=MAG_NATIVE_SCENE_NONE;
    mag_native_scene scene={};scene.struct_size=sizeof(scene);scene.abi_version=1;
    scene.images=&image;scene.image_count=1;scene.passes=&pass;scene.pass_count=1;scene.output_image=MAG_NATIVE_SCENE_NONE;
    mag_native_stencil_attachment stencil={MAG_NATIVE_SCENE_NONE,0,0,0,0,0};
    mag_native_scene_v5 v5={};v5.struct_size=sizeof(v5);v5.abi_version=5;
    v5.base.struct_size=sizeof(v5.base);v5.base.abi_version=4;v5.base.stencils=&stencil;v5.base.stencil_count=1;
    v5.base.base.struct_size=sizeof(v5.base.base);v5.base.base.abi_version=3;
    v5.base.base.base.struct_size=sizeof(v5.base.base.base);v5.base.base.base.abi_version=2;v5.base.base.base.scene=&scene;
    v5.imports=&import;v5.import_count=1;
    mag_native_ticket encoded={};
    const int status=mag_native_submit_scene_v5(device,&v5,NULL,NULL,&encoded);
    if(status)fprintf(stderr,"[reserved submission] encode status=%d\n",status);
    assert(status==0);
    assert(encoded.device_generation==second.device_generation&&encoded.submission_id==second.submission_id);
    assert(mag_native_abandon_scene_reservation(device)==MAG_NATIVE_STALE);
    int completed=0;
    for(int i=0;i<2000&&!completed;++i){mag_native_event event={};uint32_t count=0;assert(mag_native_poll(device,&event,1,&count)==0);
        if(count){assert(event.ticket.submission_id==second.submission_id&&event.succeeded);completed=1;}
        else usleep(1000);}
    assert(completed);
    assert(mag_native_image_release(device,ref)==0);
    const int closed=mag_native_close(device);
    if(closed)fprintf(stderr,"[reserved submission] close status=%d\n",closed);
    assert(closed==0);
    fprintf(stderr,"[reserved submission] reserve=%llu abandoned busy/close then encoded=%llu completed\n",
        (unsigned long long)first.submission_id,(unsigned long long)encoded.submission_id);
}
/* Queue order around a held reservation, on the real queue: original work
 * committed after a reservation waits for it; abandoning the reservation
 * commits it empty and releases that work; so does encoding into it. */
static int wait_completed(id<MTLCommandBuffer> b,int ms){
    for(int i=0;i<ms;++i){if(b.status>=MTLCommandBufferStatusCompleted)return 1;usleep(1000);}
    return b.status>=MTLCommandBufferStatusCompleted;
}
static void check_reservation_queue_order(const MagRenderHostOwner *owner,const MagRenderHostImage *target){
    id<MTLCommandQueue> queue=(__bridge id<MTLCommandQueue>)owner->queue;
    for(int abandon=1;abandon>=0;--abandon){
        mag_native_device *device=NULL;assert(mag_native_create(owner,&device)==0);
        mag_native_image_certificate certificate={*target,NULL};mag_native_image ref={};
        assert(mag_native_image_import_certified(device,&certificate,&ref)==0);
        mag_native_scene_import import={0,0,ref};
        mag_native_ticket reserved={};
        assert(mag_native_reserve_scene_v5(device,&import,1,&reserved)==0);
        id<MTLCommandBuffer> later=[queue commandBuffer];
        [later commit];
        assert(!wait_completed(later,50)); // held behind the reservation
        if(abandon){
            assert(mag_native_abandon_scene_reservation(device)==0);
        }else{
            mag_native_scene_image image={MAG_NATIVE_SCENE_RGBA8,target->width,target->height,0,{}};
            mag_native_scene_pass pass={};
            pass.color.image=0;pass.color.load=MAG_NATIVE_SCENE_CLEAR;pass.color.store=1;
            pass.color.clear[2]=0.5f;pass.color.clear[3]=1;pass.depth.image=MAG_NATIVE_SCENE_NONE;
            mag_native_scene scene={};scene.struct_size=sizeof(scene);scene.abi_version=1;
            scene.images=&image;scene.image_count=1;scene.passes=&pass;scene.pass_count=1;scene.output_image=MAG_NATIVE_SCENE_NONE;
            mag_native_stencil_attachment stencil={MAG_NATIVE_SCENE_NONE,0,0,0,0,0};
            mag_native_scene_v5 v5={};v5.struct_size=sizeof(v5);v5.abi_version=5;
            v5.base.struct_size=sizeof(v5.base);v5.base.abi_version=4;v5.base.stencils=&stencil;v5.base.stencil_count=1;
            v5.base.base.struct_size=sizeof(v5.base.base);v5.base.base.abi_version=3;
            v5.base.base.base.struct_size=sizeof(v5.base.base.base);v5.base.base.base.abi_version=2;v5.base.base.base.scene=&scene;
            v5.imports=&import;v5.import_count=1;
            mag_native_ticket encoded={};
            assert(mag_native_submit_scene_v5(device,&v5,NULL,NULL,&encoded)==0);
        }
        assert(wait_completed(later,2000)); // released in queue order
        for(int i=0;i<2000;++i){mag_native_event event={};uint32_t count=0;assert(mag_native_poll(device,&event,1,&count)==0);if(!count&&abandon)break;if(count)break;usleep(1000);}
        assert(mag_native_image_release(device,ref)==0);
        assert(mag_native_close(device)==0);
    }
    fprintf(stderr,"[reservation queue order] later work held, then released by abandon and by encoding\n");
}
/* The state-first sequence through the real registered worker and its C
 * entry points, across two threads, on the real queue:
 * - the boundary thread commits ordinary Mains (their terminals left unread,
 *   as a host waiting on later queue work leaves them), then reserves a
 *   position with its imports, and queues later original work;
 * - a late thread commits a packet into the position (or declines it);
 * - the later work completes, and the position's ticket reaches a terminal.
 * A clear-only packet on the imported target stands in for Main. */
struct HarnessPacket {
    MagRenderPacketImage image{};
    MagRenderPacketPass pass{};
    mag_native_viewport viewport{};
    MagRenderPacketStencilAttachment stencil{MAG_NATIVE_SCENE_NONE,0,0,0,0,0};
    MagRenderPacketProgram programs[2]{};
    uint32_t location0=0;
    mag_native_vertex_attribute attribute{0,4,0,0};
    MagRenderPacketPipeline pipeline{};
    float vertices[12]={-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
    uint32_t indices[3]={0,1,2};
    MagRenderPacketBuffer buffers[2]{};
    MagRenderPacketDraw draw{};
    MagRenderPacketItem item{};
    MagRenderScenePacketV3Desc desc{};
    MagRenderPacketImageRequirement requirement{};
    MagRenderPacketImageBinding binding{};
    // One full-target triangle with the fixture's scene vertex/fragment
    // programs, written into the imported target.
    HarnessPacket(const MagRenderHostImage *target,uint64_t publication,NSData *vs,NSData *fs){
        const MagRenderClientSource source={1,2,publication,4};
        image={MAG_NATIVE_SCENE_RGBA8,2,target->width,target->height,1,1,1,1,NULL,0,0};
        programs[0].stage=0;
        programs[0].shader={vs.bytes,(uint64_t)vs.length,"scene_vertex",12,1};
        programs[0].vertex_location_count=1;programs[0].vertex_locations=&location0;
        programs[1].stage=1;
        programs[1].shader={fs.bytes,(uint64_t)fs.length,"scene_fragment",14,1};
        pipeline.vertex_program=0;pipeline.fragment_program=1;pipeline.vertex_stride=16;
        pipeline.attribute_count=1;pipeline.attributes=&attribute;
        pipeline.color_format=MAG_NATIVE_SCENE_RGBA8;pipeline.depth_format=0;pipeline.binding_model=1;
        pipeline.state.known_fields=511;pipeline.state.color_write_mask=15;pipeline.state.samples=1;
        buffers[0]={0,0,vertices,sizeof(vertices)};
        buffers[1]={0,0,indices,sizeof(indices)};
        draw.source_item=0;draw.source_partition=0;draw.pipeline=0;draw.index_count=3;
        draw.vertices={0,0,0,sizeof(vertices)};draw.indices={1,0,0,sizeof(indices)};
        item={0,0,0,0};
        pass.source=source;
        pass.color={0,MAG_NATIVE_SCENE_CLEAR,0,0,{0.1f,0.2f,0.3f,1}}; // packet store 0 = store
        pass.depth={MAG_NATIVE_SCENE_NONE,0,0,0,{0,0,0,0}};
        pass.draws=&draw;pass.draw_count=1;
        viewport={0,0,float(target->width),float(target->height),0,1,0,0,target->width,target->height,7,0};
        auto &b=desc.base.base;
        b.abi_version=1;b.struct_size=sizeof(b);b.source=source;b.intended_items=1;
        b.programs=programs;b.program_count=2;b.pipelines=&pipeline;b.pipeline_count=1;
        b.buffers=buffers;b.buffer_count=2;
        b.image_count=1;b.images=&image;b.pass_count=1;b.passes=&pass;b.output_image=0;
        b.items=&item;b.item_count=1;
        desc.base.abi_version=2;desc.base.struct_size=sizeof(desc.base);
        desc.base.viewports=&viewport;desc.base.viewport_count=1;
        desc.abi_version=3;desc.struct_size=sizeof(desc);
        desc.stencils=&stencil;desc.stencil_count=1;
        requirement={0,0,1};
        binding.image=0;binding.source_key=1;binding.host=*target;binding.validate_planes=NULL;
    }
};
static int poll_ticket(uint64_t worker,uint64_t ticket,int ms){
    for(int i=0;i<ms;++i){
        MagRenderClientReceipt r{};r.abi_version=1;r.struct_size=sizeof(r);
        const int s=mag_render_client_worker_poll(worker,&r);
        if(s==0&&r.ticket==ticket&&(r.flags&MAG_RENDER_CLIENT_TERMINAL))return 1;
        usleep(1000);
    }
    return 0;
}
static void check_worker_position_sequence(const MagRenderHostOwner *owner,const MagRenderHostImage *target,const char *directory){
    NSString *root=[NSString stringWithUTF8String:directory];
    NSData *vs=[NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"scene_vertex/shader.metal"]];
    NSData *fs=[NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"scene_fragment/shader.metal"]];
    assert(vs&&fs);
    id<MTLCommandQueue> queue=(__bridge id<MTLCommandQueue>)owner->queue;
    MagRenderHostOwnerV2 v2{};v2.abi_version=2;v2.struct_size=sizeof(v2);v2.base=*owner;
    // Variants: commit or decline the position, each after one ordinary
    // commit and after two still in flight (the reservation then waits,
    // bounded, for capacity as an ordinary commit's admission does).
    for(int variant=0;variant<4;++variant){
        const int decline=variant&1;
        const uint64_t ordinary=1+(variant>>1);
        uint64_t worker=0;
        int status=mag_render_client_worker_create_scene_v2(&v2,64,64u<<20,&worker);
        if(status)fprintf(stderr,"[worker position harness] create status=%d\n",status);
        assert(status==0&&worker);
        // An ordinary commit whose terminal is left unread (a reservation
        // needs fewer than two submissions in flight).
        for(uint64_t p=1;p<=ordinary;++p){
            HarnessPacket h(target,p,vs,fs);
            MagRenderClientReceipt r{};r.abi_version=1;r.struct_size=sizeof(r);
            status=mag_render_client_worker_submit_scene_subgraph_imported_v3(worker,&h.desc,&h.requirement,1,&h.binding,1,&r);
            if(status)fprintf(stderr,"[worker position harness] ordinary commit %llu status=%d\n",(unsigned long long)p,status);
            assert(status==0);
        }
        HarnessPacket main(target,3,vs,fs);
        MagRenderClientReceipt reserved{};reserved.abi_version=1;reserved.struct_size=sizeof(reserved);
        status=mag_render_client_worker_reserve_position_imported(worker,&main.requirement,1,&main.binding,1,&reserved);
        if(status)fprintf(stderr,"[worker position harness] reserve status=%d\n",status);
        assert(status==0&&reserved.ticket&&reserved.native_ticket);
        id<MTLCommandBuffer> later=[queue commandBuffer];[later commit];
        // The boundary thread holds the host's owner lease while the late half
        // starts (as the FIFO does during its own native work), making no
        // worker call meanwhile, then releases it.
        assert(owner->try_begin(owner->userdata,owner->lifetime)==0);
        int late_status=1;
        std::thread late([&]{
            if(decline){late_status=mag_render_client_worker_decline_position(worker,reserved.ticket);return;}
            MagRenderClientReceipt r{};r.abi_version=1;r.struct_size=sizeof(r);
            late_status=mag_render_client_worker_submit_scene_subgraph_imported_v3_at(worker,&main.desc,&main.requirement,1,&main.binding,1,reserved.ticket,&r);
        });
        usleep(20000);
        owner->end(owner->userdata,owner->lifetime);
        // Then it tries the next frame's ordinary commit (refused while the
        // position is held, its preparation cancelled).
        {
            HarnessPacket next(target,5,vs,fs);
            MagRenderClientReceipt r{};r.abi_version=1;r.struct_size=sizeof(r);
            const int refused=mag_render_client_worker_submit_scene_subgraph_imported_v3(worker,&next.desc,&next.requirement,1,&next.binding,1,&r);
            fprintf(stderr,"[worker position harness] ordinary commit while held status=%d\n",refused);
        }
        late.join();
        if(late_status)fprintf(stderr,"[worker position harness] late %s status=%d\n",decline?"decline":"commit",late_status);
        assert(late_status==0);
        assert(wait_completed(later,2000));
        assert(poll_ticket(worker,reserved.ticket,2000));
        for(int i=0;i<200;++i){MagRenderClientReceipt r{};r.abi_version=1;r.struct_size=sizeof(r);(void)mag_render_client_worker_poll(worker,&r);usleep(1000);}
        status=mag_render_client_worker_close(worker);
        if(status)fprintf(stderr,"[worker position harness] close status=%d\n",status);
        assert(status==0);
    }
    fprintf(stderr,"[worker position harness] reserve_imported -> late commit and -> late decline both completed across threads, after one and two ordinary commits\n");
}
/* A position-only reservation (no imports yet) holds its queue position and
 * ticket; the encoding validates the scene's own import under a short lease
 * and completes with that ticket. An import that is no longer valid at
 * encoding releases the reservation and fails the encoding. */
static void check_position_reservation(const MagRenderHostOwner *owner,const MagRenderHostImage *target){
    mag_native_device *device=NULL;assert(mag_native_create(owner,&device)==0);
    mag_native_image_certificate certificate={*target,NULL};mag_native_image ref={};
    assert(mag_native_image_import_certified(device,&certificate,&ref)==0);
    mag_native_scene_import import={0,0,ref};
    mag_native_ticket reserved={},busy={};
    assert(mag_native_reserve_scene_v5(device,&import,0,&reserved)==MAG_NATIVE_INVALID);
    assert(mag_native_reserve_scene_v5(device,NULL,0,&reserved)==0&&reserved.submission_id);
    assert(mag_native_reserve_scene_v5(device,NULL,0,&busy)==MAG_NATIVE_BUSY);
    mag_native_scene_image image={MAG_NATIVE_SCENE_RGBA8,target->width,target->height,0,{}};
    mag_native_scene_pass pass={};
    pass.color.image=0;pass.color.load=MAG_NATIVE_SCENE_CLEAR;pass.color.store=1;
    pass.color.clear[1]=0.5f;pass.color.clear[3]=1;pass.depth.image=MAG_NATIVE_SCENE_NONE;
    mag_native_scene scene={};scene.struct_size=sizeof(scene);scene.abi_version=1;
    scene.images=&image;scene.image_count=1;scene.passes=&pass;scene.pass_count=1;scene.output_image=MAG_NATIVE_SCENE_NONE;
    mag_native_stencil_attachment stencil={MAG_NATIVE_SCENE_NONE,0,0,0,0,0};
    mag_native_scene_v5 v5={};v5.struct_size=sizeof(v5);v5.abi_version=5;
    v5.base.struct_size=sizeof(v5.base);v5.base.abi_version=4;v5.base.stencils=&stencil;v5.base.stencil_count=1;
    v5.base.base.struct_size=sizeof(v5.base.base);v5.base.base.abi_version=3;
    v5.base.base.base.struct_size=sizeof(v5.base.base.base);v5.base.base.base.abi_version=2;v5.base.base.base.scene=&scene;
    v5.imports=&import;v5.import_count=1;
    mag_native_ticket encoded={};
    assert(mag_native_submit_scene_v5(device,&v5,NULL,NULL,&encoded)==0);
    assert(encoded.submission_id==reserved.submission_id);
    int completed=0;
    for(int i=0;i<2000&&!completed;++i){mag_native_event event={};uint32_t count=0;assert(mag_native_poll(device,&event,1,&count)==0);
        if(count){assert(event.ticket.submission_id==reserved.submission_id&&event.succeeded);completed=1;}
        else usleep(1000);}
    assert(completed);
    /* A stale import at encoding: the reservation is released, not encoded. */
    mag_native_ticket second={};
    assert(mag_native_reserve_scene_v5(device,NULL,0,&second)==0&&second.submission_id>reserved.submission_id);
    mag_native_scene_import stale=import;stale.ref.id+=1000;v5.imports=&stale;
    assert(mag_native_submit_scene_v5(device,&v5,NULL,NULL,&encoded)!=0);
    assert(mag_native_abandon_scene_reservation(device)==MAG_NATIVE_STALE);
    assert(mag_native_image_release(device,ref)==0);
    assert(mag_native_close(device)==0);
    fprintf(stderr,"[position reservation] reserved=%llu encoded, stale import released\n",(unsigned long long)reserved.submission_id);
}
/* Reserved encoding of the six-pass probe scene must equal its direct
 * encoding byte for byte: once unchunked, then with the opt-in chunked commit
 * (three slots, so shadow maps written in the first chunk are sampled in
 * later ones and color/depth stored in one chunk are loaded in the next). */
static int check_reserved_identity(const MagRenderHostOwner *owner,const mag_native_pipeline_desc *first,
                                   const mag_native_pipeline_desc *second,const mag_native_scene_graphics_desc *graphics,
                                   const MagRenderHostImage *target,NSMutableDictionary *result){
    const char *names[2]={"MAG_RENDER_MAIN_CHUNKED_COMMIT","MAG_RENDER_MAIN_CHUNKS"};
    std::string saved[2];bool had[2];
    for(int i=0;i<2;++i){const char *v=getenv(names[i]);had[i]=v!=NULL;if(v)saved[i]=v;}
    int status=0;
    for(int chunked=0;chunked<2&&!status;++chunked){
        if(chunked){setenv(names[0],"1",1);setenv(names[1],"3",1);check_reserved_submission(owner,target);}
        else{unsetenv(names[0]);unsetenv(names[1]);}
        ReservedProbeReport r={};r.abi_version=1;r.struct_size=sizeof(r);
        status=mag_render_native_scene_probe_reserved(owner,first,second,graphics,target,&r);
        fprintf(stderr,"[reserved identity] chunked=%d status=%d identical=%u differing=%llu chunked_submissions=%llu slots=%llu used=%llu\n",
            chunked,status,r.identical,(unsigned long long)r.differing_bytes,(unsigned long long)r.chunked_submissions,
            (unsigned long long)r.chunk_slots,(unsigned long long)r.chunks_used);
        const bool shape=chunked?r.chunked_submissions==1&&r.chunk_slots==3&&r.chunks_used>=2:r.chunked_submissions==0;
        if(!status&&(!r.identical||!shape))status=-6;
        result[chunked?@"reserved_chunked":@"reserved_unchunked"]=@{@"status":@(status),@"identical":@(r.identical!=0),
            @"chunk_slots":@(r.chunk_slots),@"chunks_used":@(r.chunks_used)};
    }
    for(int i=0;i<2;++i){if(had[i])setenv(names[i],saved[i].c_str(),1);else unsetenv(names[i]);}
    return status;
}
int mag_render_scene_target_main(const char *directory, bool diagnostic,bool imported) {
    @autoreleasepool {
        NSString *root = [NSString stringWithUTF8String:directory];
        Shader first = {}, second = {}, vertex = {}, fragment = {}, cutout = {};
        if (!first.load(root, @"scene_transform", true) || !second.load(root, @"scene_prepare", true) ||
            !vertex.load(root, @"scene_lit_vertex", false) ||
            !fragment.load(root, @"scene_lit_fragment", false) ||
            !cutout.load(root, @"scene_cutout_fragment", false))
            return 2;
        auto vertex_buffers = buffers(vertex, 0);
        auto fragment_buffers = buffers(fragment, 1);
        if (vertex_buffers.size() != 1 || fragment_buffers.size() != 1)
            return 2;
        if ([vertex.manifest[@"varyings"][0][@"format"] isEqual:@"float32x2"] != YES ||
            [fragment.manifest[@"images"] count] != 3 || [fragment.manifest[@"samplers"] count] != 2)
            return 2;
        std::vector<mag_native_scene_buffer_layout> both = vertex_buffers;
        both.insert(both.end(), fragment_buffers.begin(), fragment_buffers.end());
        std::vector<mag_native_scene_texture_layout> textures;
        for (NSDictionary *t in fragment.manifest[@"images"]) {
            uint32_t slot = [t[@"binding"] unsignedIntValue];
            bool depth = [t[@"sample_kind"] isEqual:@"depth"];
            if ([t[@"group"] unsignedIntValue] != 1 || (!depth && slot != 1) ||
                (depth && slot != 3 && slot != 4))
                return 2;
            textures.push_back({1, slot, depth ? 5u : 2u,
                                depth ? MAG_NATIVE_SCENE_D32 : MAG_NATIVE_SCENE_RGBA8, depth ? 1u : 0u, 0});
        }
        for (NSDictionary *s in fragment.manifest[@"samplers"]) {
            uint32_t slot = [s[@"binding"] unsignedIntValue];
            if ([s[@"group"] unsignedIntValue] != 1 || (slot != 2 && slot != 5) ||
                [s[@"comparison"] boolValue] != (slot == 5))
                return 2;
        }
        mag_native_scene_graphics_desc graphics[5] = {};
        for (auto &g : graphics) {
            g.struct_size = sizeof(g);
            g.abi_version = 1;
            g.vertex = vertex.stage();
            g.fragment = fragment.stage();
            g.vertex_stride = 16;
            g.color_format = MAG_NATIVE_SCENE_RGBA8;
            g.depth_format = MAG_NATIVE_SCENE_D32;
            g.depth_compare = MAG_NATIVE_SCENE_DEPTH_LESS;
            g.depth_write = 1;
            g.buffers = both.data();
            g.buffer_count = uint32_t(both.size());
            g.textures = textures.data();
            g.texture_count = uint32_t(textures.size());
        }
        graphics[0].color_format = 0;
        graphics[0].fragment = {};
        graphics[0].buffers = vertex_buffers.data();
        graphics[0].buffer_count = uint32_t(vertex_buffers.size());
        graphics[0].textures = NULL;
        graphics[0].texture_count = 0;
        graphics[2].depth_write = 0;
        graphics[2].blend = MAG_NATIVE_SCENE_ALPHA;
        graphics[3].depth_compare = MAG_NATIVE_SCENE_DEPTH_DISABLED;
        graphics[3].depth_write = 0;
        graphics[3].blend = MAG_NATIVE_SCENE_ALPHA;
        mag_native_scene_texture_layout cutout_texture = {1, 1, 2, MAG_NATIVE_SCENE_RGBA8, 0, 0};
        graphics[4] = graphics[0];
        graphics[4].fragment = cutout.stage();
        graphics[4].textures = &cutout_texture;
        graphics[4].texture_count = 1;
        MagRenderHostOwner owner = {};
        MagRenderHostImage target={};
        if (mag_render_test_owner_open(&owner) || (imported&&mag_render_test_owner_image(&target))) {
            mag_render_test_owner_close(1);
            return 3;
        }
        // Before the probe's deliberate queue gate: this check needs completion.
        if(imported)check_worker_position_sequence(&owner,&target,directory);
        if(imported)check_reserved_submission(&owner,&target);
        if(imported)check_position_reservation(&owner,&target);
        if(imported)check_reservation_queue_order(&owner,&target);
        if (mag_render_test_owner_gate()) {
            mag_render_test_owner_close(1);
            return 3;
        }
        check_rejected_scenes(&owner);
        NSString *name = [NSString stringWithUTF8String:mag_render_test_owner_name()];
        SceneProbeReport report = {};
        report.abi_version = 1;
        report.struct_size = sizeof(report);
        int status =
            mag_render_native_scene_probe_target(&owner, &first.compute, &second.compute, graphics,imported?&target:NULL,
                                          diagnostic ? 1 : 0, mag_render_test_owner_signal, NULL, &report);
        mag_render_test_owner_signal(NULL);
        // Before the target readback, which leaves the owner at a dirty boundary;
        // the reserved runs render the same pixels into the same target.
        NSMutableDictionary *reserved=[NSMutableDictionary new];
        if(status==0&&!imported&&diagnostic)
            status=check_memory_flat(&owner,&first.compute,&second.compute,graphics,reserved);
        if(status==0&&imported&&diagnostic)
            status=check_reserved_identity(&owner,&first.compute,&second.compute,graphics,&target,reserved);
        if(status==0&&imported&&diagnostic)status=mag_render_test_owner_image_check();
        mag_render_test_owner_close(report.pending == 0);
        NSMutableDictionary *result = [@{
            @"device" : name,
            @"status" : @(status),
            @"submitted" : @(report.submitted),
            @"completed" : @(report.completed),
            @"pending" : @(report.pending),
            @"retained_before_terminal" : @(report.retained_before_terminal),
            @"retained_after_terminal" : @(report.retained_after_terminal),
            @"center_rgba" : @(report.center_rgba),
            @"ui_rgba" : @(report.ui_rgba),
            @"shadow_rgba" : @(report.shadow_rgba),
            @"outside_rgba" : @(report.outside_rgba),
            @"passes" : @(report.pass_count),
            @"draws" : @(report.draw_count),
            @"diagnostic_readback" : @(diagnostic),
            @"physical_presentation" : @NO,
            @"imported_original_target":@(imported),
            @"native_rejection_cases" : @7
        } mutableCopy];
        [result addEntriesFromDictionary:reserved];
        NSData *encoded = [NSJSONSerialization dataWithJSONObject:result
                                                          options:NSJSONWritingSortedKeys
                                                            error:NULL];
        printf("%s\n", [[[NSString alloc] initWithData:encoded encoding:NSUTF8StringEncoding] UTF8String]);
        return status == 0 && report.submitted == 2 && report.completed == 2 && !report.pending &&
                       report.retained_before_terminal == 608 && !report.retained_after_terminal
                   ? 0
                   : 1;
    }
}

int mag_render_scene_main(const char *directory,bool diagnostic){return mag_render_scene_target_main(directory,diagnostic,false);}
