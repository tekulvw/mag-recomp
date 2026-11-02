/* External, source-bound SDK compatibility. Generic native code includes none
 * of these SDK internals. Only owned synthetic state/device/queue is created. */
#include "rsx_metal_backend.m"
#include "mag_render_native_api.h"
#include <assert.h>
static id<MTLSharedEvent> native_gate;
static id<MTLCommandBuffer> native_prior;
static rsx_native_owner borrowed;
/* Deliberate test-only legacy primitive trap: native mode must not call it. */
rsx_frontend_device *rsx_frontend_metal_create(id<MTLDevice> d,id<MTLCommandQueue> q,int(*r)(void)) {(void)d;(void)q;(void)r;abort();}
void rsx_frontend_close(rsx_frontend_device *device) {assert(!device);}
static void translate(MagRenderHostOwner *out) {
#define COPY(field) out->field=borrowed.field
    COPY(abi_version);COPY(struct_size);COPY(backend);COPY(reserved);COPY(lifetime);
    COPY(device);COPY(queue);COPY(userdata);COPY(queue_family);COPY(reserved1);
    COPY(instance);COPY(physical_device);COPY(get_instance_proc);COPY(get_device_proc);
    COPY(retain);COPY(release);COPY(try_begin);COPY(end);
#undef COPY
}
int mag_render_test_owner_open(MagRenderHostOwner *out) {
    if(s_dev || s_queue || s_ready)return -1;
    setenv("PS3RECOMP_RSX_NATIVE_OWNER","1",1);
    setenv("PS3RECOMP_METAL_HEADLESS","1",1);
    setenv("PS3RECOMP_RSX_ENGINE","dispatch",1);
    if(rsx_metal_backend_init(8,8,"Owned native regression")!=0)return -1;
    borrowed=(rsx_native_owner){.abi_version=1,.struct_size=sizeof(borrowed)};
    if(rsx_metal_backend_borrow_owner(&borrowed))return -1;
    assert(!rsx_metal_backend_frontend());
    translate(out);
    mag_native_device *probe=NULL;assert(mag_native_create(out,&probe)==0);
    assert(out->try_begin(out->userdata,out->lifetime)==-4);
    assert(s_engine_backend.submit_and_wait_checked(NULL,0));
    assert(out->try_begin(out->userdata,out->lifetime)==0);out->end(out->userdata,out->lifetime);
    rsx_state state={0};s_backend_vtable.set_blend(NULL,&state);
    assert(out->try_begin(out->userdata,out->lifetime)==-4);
    assert(s_engine_backend.submit_and_wait_checked(NULL,0));
    assert(out->try_begin(out->userdata,out->lifetime)==0);out->end(out->userdata,out->lifetime);
    rsx_draw_engine_method_at(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_TRIANGLES,0);
    assert(out->try_begin(out->userdata,out->lifetime)==-4);
    /* A direct backend boundary cannot close a common partial primitive. */
    assert(s_engine_backend.submit_and_wait_checked(NULL,0));
    assert(out->try_begin(out->userdata,out->lifetime)==-4);
    rsx_draw_engine_method_at(NV4097_SET_BEGIN_END,0,0);
    assert(rsx_draw_engine_flush_checked());
    assert(out->try_begin(out->userdata,out->lifetime)==0);out->end(out->userdata,out->lifetime);
    assert(mag_native_close(probe)==0);return 0;
}
int mag_render_test_owner_contract(void) {
    MagRenderHostOwner owner;if(mag_render_test_owner_open(&owner))return -1;
    mag_native_device *context=NULL;assert(mag_native_create(&owner,&context)==0);
    rsx_metal_backend_shutdown();assert(s_dev && s_queue && s_ready);
    assert(rsx_metal_backend_owner_shutdown_requested(owner.lifetime)==1);
    assert(rsx_metal_backend_owner_retry_shutdown(owner.lifetime)==-4);
    assert(mag_native_close(context)==0);
    assert(s_dev && "neutral release must not call original shutdown or wait");
    assert(rsx_metal_backend_owner_retry_shutdown(owner.lifetime)==1);
    assert(!s_dev && !s_queue && !s_ready);
    assert(owner.retain(owner.userdata,owner.lifetime)==-3);
    assert(rsx_metal_backend_init(8,8,"Owned native reinit")==0);
    rsx_native_owner replacement={.abi_version=1,.struct_size=sizeof(replacement)};
    assert(rsx_metal_backend_borrow_owner(&replacement)==0);
    assert(replacement.lifetime!=owner.lifetime);
    assert(owner.retain(owner.userdata,owner.lifetime)==-3);
    assert(rsx_metal_backend_owner_retry_shutdown(owner.lifetime)==-3);
    rsx_metal_backend_shutdown();
    puts("PASS actual SDK owner gate, dirty commit boundary, retained shutdown and explicit retry");return 0;
}
const char *mag_render_test_owner_name(void) {return s_dev.name.UTF8String;}
int mag_render_test_owner_gate(void) {
    native_gate=[s_dev newSharedEvent];native_prior=[s_queue commandBuffer];if(!native_gate || !native_prior)return -1;
    [native_prior encodeWaitForEvent:native_gate value:1];[native_prior commit];return 0;
}
void mag_render_test_owner_signal(void *unused) {(void)unused;if(native_gate)native_gate.signaledValue=1;}
void mag_render_test_owner_close(int drained) {
    mag_render_test_owner_signal(NULL);if(native_prior)[native_prior waitUntilCompleted];
    native_prior=nil;native_gate=nil;if(!drained)return;
    rsx_metal_backend_shutdown();
}

#include "mag_render_scene.h"
#include "mag_render_host_bridge.h"
static u32 native_scene_target;
int mag_render_test_owner_image(MagRenderHostImage *out){
 u32 old=s_engine_backend.color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,64,64,NULL,0);if(!old)return -1;
 MagRenderHostImage stale={.abi_version=1,.struct_size=sizeof(stale)};
 assert(mag_render_frontend_borrow_image(borrowed.lifetime,old,&stale)==0);
 s_engine_backend.color_target_release(NULL,old);
 MagRenderHostOwner owner;translate(&owner);mag_native_device *probe=NULL;assert(mag_native_create(&owner,&probe)==0);
 stale.image=(void*)1;mag_native_image rejected={0};assert(mag_native_image_import(probe,&stale,&rejected)==MAG_NATIVE_STALE);assert(!rejected.id&&!rejected.device_generation);assert(mag_native_close(probe)==0);
 u32 reuse=s_engine_backend.color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,64,64,NULL,0);assert(reuse==old);
 MagRenderHostImage pinned={.abi_version=1,.struct_size=sizeof(pinned)};assert(mag_render_frontend_borrow_image(borrowed.lifetime,reuse,&pinned)==0);assert(pinned.resource_generation!=stale.resource_generation);
 assert(pinned.retain(pinned.userdata,pinned.owner_lifetime,pinned.resource_id,pinned.resource_generation)==0);
 s_engine_backend.color_target_release(NULL,reuse);assert(s_eng_obj[reuse-1]&&s_eng_obj_native_pins[reuse-1]==1);
 assert(s_engine_backend.submit_and_wait_checked(NULL,0));assert(owner.try_begin(owner.userdata,owner.lifetime)==0);
 assert(pinned.validate(pinned.userdata,pinned.owner_lifetime,pinned.resource_id,pinned.resource_generation)==MAG_NATIVE_STALE);owner.end(owner.userdata,owner.lifetime);
 pinned.release(pinned.userdata,pinned.owner_lifetime,pinned.resource_id,pinned.resource_generation);assert(!s_eng_obj[reuse-1]);
 native_scene_target=s_engine_backend.color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,64,64,NULL,0);if(!native_scene_target)return -1;
 *out=(MagRenderHostImage){.abi_version=1,.struct_size=sizeof(*out)};int status=mag_render_frontend_borrow_image(borrowed.lifetime,native_scene_target,out);
 assert(s_engine_backend.submit_and_wait_checked(NULL,0));return status;
}
int mag_render_test_owner_image_check(void){
 uint32_t pixel=0;s_engine_backend.readback(NULL,native_scene_target,32,32,1,1,&pixel,4);return pixel==0xff404000u?0:-1;
}
