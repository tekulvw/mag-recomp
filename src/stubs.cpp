#include "mag_spawn_observer.h"
/* Queue firmware diagnostics preserve the original lifted behavior. */
#ifdef MAG_QUEUE_FIRMWARE
#include "ppu_recomp.h"
#include <stdio.h>
#include <stdlib.h>
extern "C" void mag_load_queue_firmware(void);
extern "C" void ppu_register_function(uint64_t, void (*)(ppu_context*));
void mag_sre_func_210127CC(ppu_context*);
void mag_sre_func_210171B8(ppu_context*);
void mag_sre_func_210174F8(ppu_context*);
void mag_sre_func_210178D8(ppu_context*);
// Bounded, read-only observations. Queue addresses come from actual firmware calls.
static std::atomic<unsigned> trace_queues[16]{};
static std::atomic<unsigned> trace_counters[16]{}, trace_flags[16]{};
static bool detailed_queue_trace() { return getenv("MAG_TRACE_QUEUE") != nullptr; }
static void remember_queue(unsigned q, unsigned data) {
    if (!detailed_queue_trace() || !q || q>0xFFFFFF80u || !data || data>0xFFFFFFE0u) return;
    for (unsigned i=0;i<16;i++) {
        auto& slot=trace_queues[i]; unsigned value=slot.load();
        if (value==q || (!value && slot.compare_exchange_strong(value,q))) {
            trace_counters[i].store(vm_read32(data+16)&~1u);
            trace_flags[i].store(vm_read32(data+20));
            return;
        }
    }
}
extern "C" void mag_queue_snapshot(void) {
    if (!detailed_queue_trace()) return;
    static unsigned ticks=0;
    if (++ticks>120) return;
    for (unsigned index=0;index<16;index++) {
        unsigned q=trace_queues[index].load(); if (!q) continue;
        fprintf(stderr,"[mag-queue-snapshot] tick=%u q=%08X raw=",ticks,q);
        // Diagnostic samples are not a coherent multiword transaction.
        for (unsigned i=0;i<128;i+=8) fprintf(stderr," %016llX",(unsigned long long)vm_read64(q+i));
        unsigned counter=trace_counters[index].load(), flag=trace_flags[index].load();
        if (counter && counter<0xFFFF0000u) fprintf(stderr," counter=%08X:%08X",counter,vm_read32(counter));
        if (flag && flag<0xFFFF0000u) fprintf(stderr," flag=%08X:%016llX:%016llX",flag,(unsigned long long)vm_read64(flag),(unsigned long long)vm_read64(flag+8));
        fprintf(stderr,"\n");
    }
}
// Retail mage_g only: original GetJobs pool and IsJobMarkerPassed predicate.
// The launcher compilation has MAG_LAUNCHER_HOOKS; it never reads these addresses.
#ifndef MAG_LAUNCHER_HOOKS
extern "C" uint32_t ppu_vm_size;
extern "C" uint8_t* vm_base;
static bool job_marker_range(uint64_t address, uint64_t bytes) {
    const uint64_t limit=ppu_vm_size ? uint64_t(ppu_vm_size) : (uint64_t(1)<<32);
    return vm_base && address && address<=limit && bytes<=limit-address;
}
// Exact retail capture hooks are retained by tools/movie_picture_hooks.py.
extern "C" void sail_graphics_picture_capture_begin(void);
extern "C" uint64_t sail_graphics_picture_capture_end(void);
extern "C" int rsx_picture_bind_descriptor(uint32_t,uint64_t);
extern "C" int rsx_picture_capture_command(uint32_t,uint32_t,uint32_t,uint32_t);
extern "C" const uint8_t* rsx_picture_data(uint64_t,uint32_t,uint32_t,uint32_t);
extern "C" void rsx_picture_release(uint64_t);
static bool timed_movie_enabled() {
    const char* value=getenv("PS3RECOMP_SAIL_TIMED_THX");return value && !strcmp(value,"1");
}
extern "C" void mag_movie_picture_begin(void) {
    if(timed_movie_enabled())sail_graphics_picture_capture_begin();
}
extern "C" void mag_movie_picture_end(uint32_t object,int completed) {
    if(!timed_movie_enabled())return;
    const uint64_t token=sail_graphics_picture_capture_end();
    if(!completed){if(token)rsx_picture_release(token);return;}
    if(!job_marker_range(uint64_t(object)+0x18,32)){if(token)rsx_picture_release(token);abort();}
    const uint32_t descriptor=object+0x18,offset=vm_read32(descriptor);
    const uint32_t location=(vm_read32(descriptor+4)&3)==2?1:0;
    if(token && !rsx_picture_data(token,location,offset,5120*720)) {
        fprintf(stderr,"[mag-picture] selected token disagrees with original descriptor obj=%08X offset=%08X location=%u token=%llu\n",object,offset,location,(unsigned long long)token);
        rsx_picture_release(token);abort();
    }
    const int result=rsx_picture_bind_descriptor(descriptor,token);
    if(token)rsx_picture_release(token);
    if(result!=1){fprintf(stderr,"[mag-picture] descriptor capture failed\n");abort();}
    static std::atomic<unsigned> count{0};
    if(token && count.fetch_add(1)<512)fprintf(stderr,"[mag-picture] descriptor=%08X token=%llu offset=%08X location=%u\n",descriptor,(unsigned long long)token,offset,location);
}
extern "C" void mag_movie_picture_command(uint32_t data_ea,uint32_t descriptor,uint32_t offset) {
    if(!timed_movie_enabled())return;
    if(!job_marker_range(descriptor,32)||!job_marker_range(data_ea,4))abort();
    const uint32_t location=(vm_read32(descriptor+4)&3)==2?1:0;
    int result=rsx_picture_capture_command(data_ea,descriptor,location,offset);
    if(result<0){fprintf(stderr,"[mag-picture] unknown command identity descriptor=%08X word=%08X offset=%08X\n",descriptor,data_ea,offset);abort();}
    static std::atomic<unsigned> count{0};
    if(result==1 && count.fetch_add(1)<512)fprintf(stderr,"[mag-picture] FIFO captured descriptor=%08X word=%08X offset=%08X location=%u\n",descriptor,data_ea,offset,location);
}
// Original retail libgcm transfer thunk. This observer does not perform copies
// or edit any register; the original thunk retains its tail trampoline.
void func_00E4C960(ppu_context*);
static void vram_transfer_trace(ppu_context* ctx) {
    static std::atomic<unsigned> count{0};
    const unsigned n=count.fetch_add(1);
    if(n<128) {
        const uint64_t line_ea=ctx->gpr[1]+0x74;
        const uint32_t lines=job_marker_range(line_ea,4)?vm_read32(line_ea):~0u;
        fprintf(stderr,"[mag-vram-transfer] n=%u lr=%08X context=%08X src=%08X src_dma=%08X src_pitch=%08X dst=%08X dst_dma=%08X dst_pitch=%08X line_bytes=%08X lines=%08X\n",n,
            (unsigned)ctx->lr,(unsigned)ctx->gpr[3],(unsigned)ctx->gpr[4],(unsigned)ctx->gpr[5],
            (unsigned)ctx->gpr[6],(unsigned)ctx->gpr[7],(unsigned)ctx->gpr[8],
            (unsigned)ctx->gpr[9],(unsigned)ctx->gpr[10],lines);
    }
    func_00E4C960(ctx);
}
// Opt-in retail movie observations preserve the original virtual calls.
void func_003D5E5C(ppu_context*);
void func_003C65E0(ppu_context*);
static void movie_trace_record(const char* phase, uint32_t obj) {
    if (!job_marker_range(obj,0x790)) return;
    static std::atomic<unsigned> records{0};
    if (records.fetch_add(1)>=96) return;
    uint32_t vtable=vm_read32(obj), opd=0, code=0;
    if (job_marker_range(vtable,16)) {
        opd=vm_read32(vtable+12);
        if (job_marker_range(opd,8)) code=vm_read32(opd);
    }
    fprintf(stderr,"[mag-movie] %s obj=%08X state=%u request=%u init=%u player_state=%u vtable=%08X cleanup_opd=%08X cleanup=%08X resource=%08X container=%08X\n",
            phase,obj,vm_read32(obj+0x28),vm_read32(obj+0x2C),vm_read8(obj+0x30),vm_read32(obj+0x34),
            vtable,opd,code,vm_read32(obj+0x58),vm_read32(obj+0x64));
}
static void movie_update_trace(ppu_context* ctx) {
    const uint32_t obj=(uint32_t)ctx->gpr[3];
    static std::atomic<unsigned> calls{0},last_state{~0u},last_request{~0u};
    const bool valid=job_marker_range(obj,0x790);
    const unsigned state=valid?vm_read32(obj+0x28):~0u;
    const unsigned request=valid?vm_read32(obj+0x2C):~0u;
    const unsigned previous_state=last_state.exchange(state), previous_request=last_request.exchange(request);
    const bool report=calls.fetch_add(1)<4 || state!=previous_state || request!=previous_request;
    if(report) movie_trace_record("update-before",obj);
    func_003D5E5C(ctx);
    if(report) movie_trace_record("update-after",obj);
}
// Retail indirect entry observations: preserve callbacks and register state.
void func_0027268C(ppu_context*);
void func_003CE224(ppu_context*);
void func_003BFD90(ppu_context*);
// Exact retail command handlers: strings are GFxValue type4 at +8, stride16.
// All calls continue through the original handlers without guest writes.
void func_00263AD0(ppu_context*);void func_00263458(ppu_context*);
void func_002633E4(ppu_context*);void func_002637C0(ppu_context*);
void func_0026374C(ppu_context*);
static void frontend_command_trace(ppu_context* ctx,const char* label,void(*original)(ppu_context*)) {
    static std::atomic<unsigned> records{0};
    if(records.fetch_add(1)<512) {
        const uint32_t args=uint32_t(ctx->gpr[4]),count=uint32_t(ctx->gpr[5]);
        fprintf(stderr,"[mag-frontend] %s count=%u lr=%08X",label,count,unsigned(ctx->lr));
        for(unsigned i=0;i<count && i<8;i++) {
            const uint64_t a=uint64_t(args)+16*i;
            if(!job_marker_range(a,16))break;
            const unsigned type=vm_read32(a);
            fprintf(stderr," arg%u.type=%u",i,type);
            if(type==4) {
                const uint64_t data=vm_read32(a+8);char str[128]{};
                for(unsigned j=0;j+1<sizeof(str) && job_marker_range(data+j,1);j++) {
                    const unsigned ch=vm_read8(data+j);if(!ch)break;
                    str[j]=(ch>=32 && ch<127)?char(ch):'?';
                }
                fprintf(stderr," value=%s",str);
            }
        }
        fprintf(stderr,"\n");
    }
    original(ctx);
}
static void frontend_event_trace(ppu_context*c){frontend_command_trace(c,"event",func_00263AD0);}
static void frontend_load_trace(ppu_context*c){frontend_command_trace(c,"load",func_00263458);}
static void frontend_unload_trace(ppu_context*c){frontend_command_trace(c,"unload",func_002633E4);}
static void frontend_start_trace(ppu_context*c){frontend_command_trace(c,"start",func_002637C0);}
static void frontend_stop_trace(ppu_context*c){frontend_command_trace(c,"stop",func_0026374C);}

static void startup_ui_trace(ppu_context* ctx) {
    static std::atomic<unsigned> calls{0};
    const unsigned call=calls.fetch_add(1);
    const uint64_t sf=uint64_t(uint32_t(ctx->gpr[3]))+0x70;
    if ((call<12 || call%600==0) && call<240000 && job_marker_range(sf,0x30)) {
        const uint32_t view=vm_read32(sf+0x28),active=vm_read32(sf+0x2C);
        uint32_t vt=0,opd=0,code=0;
        if(job_marker_range(view,4)) vt=vm_read32(view);
        if(job_marker_range(vt,0x80)) opd=vm_read32(vt+0x7C);
        if(job_marker_range(opd,8)) code=vm_read32(opd);
        fprintf(stderr,"[mag-startup-ui] call=%u sf=%08X view=%08X active=%u dt=%.9g advance_opd=%08X advance_code=%08X lr=%08X\n",
            call,unsigned(sf),view,active,ctx->fpr[1],opd,code,unsigned(ctx->lr));
        if(job_marker_range(0x01FB9760,0x798)) fprintf(stderr,"[mag-frontend-clock] call=%u movie_state=%u duration=%llu timecode=%llu\n",
            call,vm_read32(0x01FB9760+0x28),(unsigned long long)vm_read64(0x01FB9760+0x690),(unsigned long long)vm_read64(0x01FB9760+0x790));
        if (job_marker_range(view,0xB30)) {
            const auto field=[](uint32_t base,uint32_t offset)->uint32_t { const uint64_t a=uint64_t(base)+offset; return base && job_marker_range(a,4)?vm_read32(a):0; };
            const uint32_t def=field(view,0x6C),a=field(def,0x1C),b=field(a,0x14),c=field(b,0x20);
            const uint64_t data=field(c,0x2C)&~3u,url=(data+8)&~uint64_t(3);
            char name[160]{};
            if (data && job_marker_range(url,sizeof(name))) for(unsigned i=0;i+1<sizeof(name);i++) {
                const unsigned ch=vm_read8(url+i);if(!ch)break;name[i]=(ch>=32 && ch<127)?char(ch):'?';
            }
            fprintf(stderr,"[mag-startup-ui] definition=%08X url=%s levels=%u flags=%08X\n",
                def,name,vm_read32(view+0x60),vm_read32(view+0xB2C));
            const uint32_t timers=vm_read32(view+0xAE0),count=vm_read32(view+0xAE4);
            fprintf(stderr,"[mag-startup-ui] clock_ms=%llu frame_time_bits=%08X frame_duration_bits=%08X timers=%08X count=%u\n",
                (unsigned long long)vm_read64(view+0x130),vm_read32(view+0x138),vm_read32(view+0x13C),timers,count);
            if (count<=16 && job_marker_range(timers,uint64_t(count)*4)) for (unsigned i=0;i<count;i++) {
                const uint32_t timer=vm_read32(timers+4*i);
                if(job_marker_range(timer,0x44)) fprintf(stderr,"[mag-startup-ui] timer=%08X duration_ms=%u deadline_ms=%llu active=%u\n",
                    timer,vm_read32(timer+0x28),(unsigned long long)vm_read64(timer+0x30),vm_read8(timer+0x40));
            }
        }
    }
    func_0027268C(ctx);
}
static void movie_alloc_trace(ppu_context* ctx) {
    static std::atomic<unsigned> calls{0};
    const bool report=calls.fetch_add(1)<64;
    if(report) fprintf(stderr,"[mag-movie-allocator] alloc pArg=%08X boundary=%llu size=%llu lr=%08X\n",
        unsigned(ctx->gpr[3]),(unsigned long long)ctx->gpr[4],(unsigned long long)ctx->gpr[5],unsigned(ctx->lr));
    func_003CE224(ctx);
    if(report) fprintf(stderr,"[mag-movie-allocator] allocated=%08X\n",unsigned(ctx->gpr[3]));
}
static void movie_free_trace(ppu_context* ctx) {
    static std::atomic<unsigned> calls{0};
    if(calls.fetch_add(1)<64) fprintf(stderr,"[mag-movie-allocator] free pArg=%08X boundary=%llu memory=%08X lr=%08X\n",
        unsigned(ctx->gpr[3]),(unsigned long long)ctx->gpr[4],unsigned(ctx->gpr[5]),unsigned(ctx->lr));
    func_003BFD90(ctx);
}
static void movie_cleanup_trace(ppu_context* ctx) {
    const uint32_t obj=(uint32_t)ctx->gpr[3];
    movie_trace_record("cleanup-before",obj);
    func_003C65E0(ctx);
    movie_trace_record("cleanup-after",obj);
}
extern "C" void mag_job_marker_snapshot(void) {
    mag_spawn_observer::poll(); // Concurrent host cadence; never a map-load barrier.
    if (!getenv("MAG_TRACE_JOB_MARKERS")) return;
    static unsigned ticks=0;
    if (ticks>=480) return;
    ++ticks;
    constexpr uint64_t pool=0x020CDB70, module_a=0x01F6C740, module_b=0x01F6C560;
    // Pins are derived from the audited retail ELF, not prototype addresses.
    // Bounds use 64-bit sums before any guest access, including nested pointers.
    const struct { uint64_t address; uint32_t word; } pins[]={
        {0x0023D7D0,0xF821FE51}, {0x0023D830,0x3BFFFFFF},
        {0x00A047D8,0x80030000}, {0x00DFD018,0xF821FF31},
        {0x00DFD074,0x57601838}, {0x01B961E8,uint32_t(pool)},
        {0x01B92798,uint32_t(module_a)}, {0x01B926EC,uint32_t(module_b)}
    };
    for (const auto& pin:pins) {
        if (!job_marker_range(pin.address,4) || vm_read32(pin.address)!=pin.word) {
            if (ticks==1) fprintf(stderr,"[mag-job-markers] retail mage_g pins mismatch; snapshot disabled\n");
            ticks=480;
            return;
        }
    }
    if (!job_marker_range(pool,0x80+uint64_t(1024)*0x2B0)) return;
    unsigned allocated=0, matches=0;
    // Samples are deliberately read-only and are not a coherent transaction.
    for (unsigned group=0;group<16;group++) {
        const uint64_t bitmap=vm_read64(pool+uint64_t(group)*8);
        for (unsigned bit=0;bit<64;bit++) {
            if (!(bitmap&(uint64_t(1)<<bit))) continue;
            ++allocated;
            const uint64_t job=pool+0x80+uint64_t(group*64+bit)*0x2B0;
            const uint32_t module=vm_read32(job+0x288);
            // Observe all allocated retail jobs; module names can change after intro.
            if (++matches>32) continue;
            const unsigned marker=vm_read16(job+0x2A0);
            const uint64_t container=vm_read32(job+0x2A4);
            fprintf(stderr,"[mag-job-markers] tick=%u slot=%u job=%08X module=%08X marker=%04X container=%08X",
                    ticks,group*64+bit,unsigned(job),module,marker,unsigned(container));
            if ((container&3) || !job_marker_range(container,16)) {
                fprintf(stderr," invalid-container\n"); continue;
            }
            const uint64_t base=vm_read32(container);
            fprintf(stderr," fields=%08X:%08X:%08X base=%08X",
                    vm_read32(container+4),vm_read32(container+8),vm_read32(container+12),unsigned(base));
            if ((base&1) || !job_marker_range(base,0x80)) {
                fprintf(stderr," invalid-marker-base\n"); continue;
            }
            fprintf(stderr," array3_capacity_minus1=%08X array3_headers=%08X marker_block=",
                    vm_read32(base+0x2C),vm_read32(base+0x3C));
            unsigned minimum=0xFFFF;
            for (unsigned offset=0;offset<16;offset+=2) {
                const unsigned value=vm_read16(base+0x70+offset);
                fprintf(stderr,"%s%04X",offset?":":"",value);
                if (offset && value<minimum) minimum=value;
            }
            fprintf(stderr," sampled_min=%04X sampled_pass=%u\n",minimum,unsigned(minimum>=marker));
        }
    }
    fprintf(stderr,"[mag-job-markers] tick=%u allocated=%u matching=%u shown=%u noncoherent=1\n",
            ticks,allocated,matches,matches<32?matches:32);
}
#else
extern "C" void mag_job_marker_snapshot(void) {}
#endif

static void queue_attach_trace(ppu_context* ctx) {
    unsigned q=(unsigned)ctx->gpr[3];
    fprintf(stderr,"[mag-queue] attach q=%08X owner=%llX event=%llX\n",q,(unsigned long long)vm_read64(q+0x70),(unsigned long long)vm_read64(q+0x78));
    mag_sre_func_210174F8(ctx);
    fprintf(stderr,"[mag-queue] attach rc=%08X event=%llX\n",(unsigned)ctx->gpr[3],(unsigned long long)vm_read64(q+0x78));
}
static void queue_detach_trace(ppu_context* ctx) {
    unsigned q=(unsigned)ctx->gpr[3];
    fprintf(stderr,"[mag-queue] detach q=%08X owner=%llX event=%llX\n",q,(unsigned long long)vm_read64(q+0x70),(unsigned long long)vm_read64(q+0x78));
    mag_sre_func_210178D8(ctx);
    fprintf(stderr,"[mag-queue] detach rc=%08X event=%llX\n",(unsigned)ctx->gpr[3],(unsigned long long)vm_read64(q+0x78));
}
static void queue_signal_trace(ppu_context* ctx) {
    unsigned owner=(unsigned)ctx->gpr[3], waiter=(unsigned)ctx->gpr[4];
    static std::atomic<unsigned> calls{0};
    bool trace = calls.fetch_add(1) < (detailed_queue_trace()?256u:8u);
    if (trace) fprintf(stderr,"[mag-queue] notify owner=%08X waiter=%04X enabled=%08X data=%llX\n",owner,waiter,vm_read32((owner&~15u)+0xB0),(unsigned long long)vm_read64((owner&~15u)+0xB08+32*(waiter>>8)));
    unsigned taskset=0;
    if (trace && detailed_queue_trace() && (waiter>>8)<16) {
        taskset=(unsigned)vm_read64((owner&~15u)+0xB08+32*(waiter>>8));
        if (taskset && taskset<0xFFFF0000u) fprintf(stderr,"[mag-queue-signal] before taskset=%08X signal=%016llX:%016llX waiting=%016llX:%016llX\n",taskset,(unsigned long long)vm_read64(taskset+0x40),(unsigned long long)vm_read64(taskset+0x48),(unsigned long long)vm_read64(taskset+0x50),(unsigned long long)vm_read64(taskset+0x58));
    }
    mag_sre_func_210127CC(ctx);
    if (taskset && taskset<0xFFFF0000u) fprintf(stderr,"[mag-queue-signal] after taskset=%08X signal=%016llX:%016llX\n",taskset,(unsigned long long)vm_read64(taskset+0x40),(unsigned long long)vm_read64(taskset+0x48));
    if (trace) fprintf(stderr,"[mag-queue] notify rc=%08X\n",(unsigned)ctx->gpr[3]);
}
static void queue_push_trace(ppu_context* ctx) {
    unsigned q=(unsigned)ctx->gpr[3], data=(unsigned)ctx->gpr[4];
    remember_queue(q,data);
    static std::atomic<unsigned> entries{0};
    unsigned entry=entries.fetch_add(1);
    if (detailed_queue_trace() && entry<256 && data && data<=0xFFFFFFE0u) {
        fprintf(stderr,"[mag-queue-input] n=%u q=%08X data=%08X words=",entry,q,data);
        for (unsigned i=0;i<32;i+=4) fprintf(stderr," %08X",vm_read32(data+i));
        unsigned counter=vm_read32(data+16)&~1u;
        if (counter && counter<0xFFFF0000u) fprintf(stderr," counter=%08X:%08X",counter,vm_read32(counter));
        fprintf(stderr,"\n");
    }
    mag_sre_func_210171B8(ctx);
    static std::atomic<unsigned> calls{0};
    if (calls.fetch_add(1) < (detailed_queue_trace()?256u:8u)) fprintf(stderr,"[mag-queue] push q=%08X rc=%08X head=%llX tail=%llX init=%u\n",q,(unsigned)ctx->gpr[3],(unsigned long long)vm_read64(q),(unsigned long long)vm_read64(q+8),vm_read32(q+0x2C));
}

#ifdef MAG_LAUNCHER_HOOKS
#include "retail_handoff.h"
#include <stdlib.h>
#include <cstring>
#include "retail_network_policy.h"
static unsigned ticket_config() {
    unsigned root=vm_read32(0x37D7E8);
    return root ? vm_read32(root)+0x28 : 0;
}
static void ticket_settings_trace(ppu_context* ctx) {
    unsigned cfg=ticket_config();
    fprintf(stderr,"[mag-net] settings before cfg=%08X ticket=%u\n",cfg,cfg?vm_read8(cfg+0xD5):255);
    func_00014AD0(ctx);
    cfg=ticket_config();
    fprintf(stderr,"[mag-net] settings after cfg=%08X ticket=%u handled=%u\n",cfg,cfg?vm_read8(cfg+0xD5):255,(unsigned)ctx->gpr[3]);
}
static void supply_local_account(ppu_context* ctx, unsigned cfg) {
    const char* selected=getenv("MAG_LOCAL_ACCOUNT_NAME");
    if (!selected) return;
    if (!cfg || !mag_local_account_name_valid(selected)) {
        fprintf(stderr,"[mag-net] unsupported local account profile\n"); exit(89);
    }
    const unsigned selected_length=(unsigned)std::strlen(selected);
    unsigned name=cfg+0x4C;
    bool matches=true;
    for(unsigned i=0;i<=selected_length;i++) matches &= vm_read8(name+8+i)==(unsigned char)selected[i];
    int change=mag_local_account_policy(true,vm_read32(0x867F4),vm_read8(cfg+0xD5),
                                       vm_read8(cfg+0xDD),vm_read32(name+0x18),
                                       vm_read32(name+0x1C),matches);
    if(change<0) { fprintf(stderr,"[mag-net] local account profile rejected: code/config/name mismatch\n"); exit(89); }
    if(!change) return;
    // A private guest call frame carries a short string with the same allocator
    // prefix as the empty destination. The real retail setter owns the update.
    ppu_context call=*ctx;
    call.gpr[1]=(ctx->gpr[1]-0x200)&~uint64_t(15);
    unsigned source=(unsigned)call.gpr[1]+0x80;
    for(unsigned i=0;i<32;i++) vm_write8(source+i,vm_read8(name+i));
    for(unsigned i=0;i<=selected_length;i++) vm_write8(source+8+i,(unsigned char)selected[i]);
    vm_write32(source+0x18,selected_length);
    call.gpr[3]=cfg; call.gpr[4]=source;
    func_00087108(&call);
    matches=vm_read32(name+0x18)==selected_length;
    for(unsigned i=0;i<=selected_length;i++) matches &= vm_read8(name+8+i)==(unsigned char)selected[i];
    if(!matches) { fprintf(stderr,"[mag-net] original local account setter did not retain selected identity\n"); exit(89); }
    fprintf(stderr,"[mag-net] original account setter supplied local %s; NP remains offline\n",selected);
}
static void ticket_use_trace(ppu_context* ctx) {
    unsigned obj=(unsigned)ctx->gpr[3];
    if (getenv("MAG_LOCAL_NO_TICKET")) {
        unsigned cfg=ticket_config();
        uint8_t effective=vm_read8(obj+0x2280);
        int change=mag_restore_local_ticket(true,vm_read32(0x15D0C),vm_read32(0x15D10),
                                            cfg?vm_read8(cfg+0xD5):255,effective);
        if(change<0) { fprintf(stderr,"[mag-net] local ticket profile rejected: code/config mismatch\n"); exit(89); }
        if(change>0) {
            vm_write8(obj+0x2280,effective);
            fprintf(stderr,"[mag-net] local profile restored configured UseNPTicket=0 after retail override\n");
        }
    }
    supply_local_account(ctx,ticket_config());
    func_002D6530(ctx);
    static unsigned n=0;
    if(n++<12)fprintf(stderr,"[mag-net] ticket check client=%08X enabled=%u\n",obj,(unsigned)ctx->gpr[3]);
}
static void launcher_event_trace(ppu_context* ctx) {
    unsigned obj=(unsigned)ctx->gpr[3];
    fprintf(stderr,"[mag-net] launcher event=%u error=%08X state=%u\n",(unsigned)ctx->gpr[4],(unsigned)ctx->gpr[5],vm_read32(obj+0x218));
    func_00010EC8(ctx);
    fprintf(stderr,"[mag-net] launcher after event state=%u\n",vm_read32(obj+0x218));
}
static void ticket_stage_trace(ppu_context* ctx) {
    unsigned obj=(unsigned)ctx->gpr[3];
    func_000D1E48(ctx);
    static unsigned n=0;
    if(n++<12)fprintf(stderr,"[mag-net] NP entry obj=%08X stage=%u result=%u\n",obj,vm_read32(obj+0x18),(unsigned)ctx->gpr[3]);
}
#endif // MAG_LAUNCHER_HOOKS
#endif // MAG_QUEUE_FIRMWARE
extern "C" void ps3_load_prx_modules(void) {
#ifdef MAG_QUEUE_FIRMWARE
    mag_load_queue_firmware();
#ifdef MAG_LAUNCHER_HOOKS
    mag_register_handoff();
    if (getenv("MAG_TRACE_NETWORK") || getenv("MAG_LOCAL_NO_TICKET")) {
        ppu_register_function(0x14AD0,ticket_settings_trace);
        ppu_register_function(0x2D6530,ticket_use_trace);
        ppu_register_function(0x10EC8,launcher_event_trace);
        ppu_register_function(0xD1E48,ticket_stage_trace);
    }
#endif
#ifndef MAG_LAUNCHER_HOOKS
    if (getenv("MAG_TRACE_MOVIE") && job_marker_range(0x003D5E5C,4) &&
        vm_read32(0x003D5E5C)==0xF821FEF1 && job_marker_range(0x003C65E0,4) &&
        vm_read32(0x003C65E0)==0xF821FDE1) {
        ppu_register_function(0x003D5E5C,movie_update_trace);
        ppu_register_function(0x003C65E0,movie_cleanup_trace);
    }
#endif
#ifndef MAG_LAUNCHER_HOOKS
    if(getenv("MAG_TRACE_VRAM_TRANSFER") && job_marker_range(0x00E4C960,16) &&
        vm_read32(0x00E4C960)==0xF8410028 && vm_read32(0x00E4C964)==0x3C420003 &&
        vm_read32(0x00E4C968)==0x3842FD40 && vm_read32(0x00E4C96C)==0x4BB35DF8)
        ppu_register_function(0x00E4C960,vram_transfer_trace);
    if(getenv("MAG_TRACE_FRONTEND")) {
        const struct { uint32_t ea,word;void(*fn)(ppu_context*); } hooks[]={
            {0x00263AD0,0x2F850000,frontend_event_trace},{0x00263458,0x7C832378,frontend_load_trace},
            {0x002633E4,0x7C832378,frontend_unload_trace},{0x002637C0,0x7C832378,frontend_start_trace},
            {0x0026374C,0x7C832378,frontend_stop_trace}};
        for(const auto& h:hooks) if(job_marker_range(h.ea,4)&&vm_read32(h.ea)==h.word) ppu_register_function(h.ea,h.fn);
    }
    if (getenv("MAG_TRACE_STARTUP_UI") && job_marker_range(0x0027268C,4) && vm_read32(0x0027268C)==0x38630070)
        ppu_register_function(0x0027268C,startup_ui_trace);
    if (getenv("MAG_TRACE_MOVIE") && job_marker_range(0x003CE224,4) && vm_read32(0x003CE224)==0x38800000 &&
        job_marker_range(0x003BFD90,4) && vm_read32(0x003BFD90)==0x2F850000) {
        ppu_register_function(0x003CE224,movie_alloc_trace);
        ppu_register_function(0x003BFD90,movie_free_trace);
    }
#endif
    ppu_register_function(0x210174F8,queue_attach_trace);
    ppu_register_function(0x210178D8,queue_detach_trace);
    ppu_register_function(0x210127CC,queue_signal_trace);
    ppu_register_function(0x210171B8,queue_push_trace);
#endif
}

#ifndef MAG_QUEUE_FIRMWARE
extern "C" void mag_queue_snapshot(void) {}
extern "C" void mag_job_marker_snapshot(void) {}
#endif
