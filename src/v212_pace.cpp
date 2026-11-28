#include "v212_pace.h"
#include "ppu_recomp.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

extern "C" {
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
void ppu_register_function(uint64_t, void (*)(ppu_context*));
void ps3_indirect_call(ppu_context*);
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));
}

// Behaviour specification: the normal 2.12 module path selects the prepared
// original image. Loading publishes that image's functions and exports through
// the runtime PRX loader. Start/stop run its original OPDs with the caller's
// argument block, and write the guest return separately from the API status.
// The resident SDK registry cannot remove exports: unloading remains an
// explicit NOT_REMOVABLE error instead of claiming to remove live bindings.
// ABI/error references: RPCS3 Modules/sys_prx_.cpp and lv2/sys_prx.h.
namespace {
constexpr uint32_t invalid=0x80010002u, unknown=0x8001112eu;
constexpr uint32_t already_started=0x80011133u, not_started=0x80011134u;
constexpr uint32_t already_stopped=0x80011135u, not_removable=0x80011138u;
constexpr uint32_t library_found=0x8001113bu, module_error=0x80011001u;
constexpr uint32_t handle=0x212u;
enum class State { absent, initialized, starting, started, stopping, stopped };
State state=State::absent;
std::recursive_mutex lock;

bool range(uint64_t ea,uint64_t size) {
    const uint64_t limit=ppu_vm_size?ppu_vm_size:UINT64_C(0x100000000);
    return (vm_base||ppu_vm_size) && ea && ea<=UINT32_MAX && ea<limit && size<=limit-ea;
}
void result(ppu_context* ctx,uint32_t value) {
    ctx->gpr[3]=static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
}
void registrar(uint32_t ea,void(*fn)(void*)) {
    ppu_register_function(ea,reinterpret_cast<void(*)(ppu_context*)>(fn));
}
bool path_matches(uint64_t ea,const char* expected) {
    if(!expected || !*expected) return false;
    auto length=std::strlen(expected);
    if(length>=512 || !range(ea,length+1)) return false;
    for(size_t i=0;i<=length;++i)
        if(vm_read8(ea+i)!=static_cast<uint8_t>(expected[i])) return false;
    return true;
}
// ADDR32 linkage records use the real guest export value. A function value
// is its OPD (including its original TOC), while a variable value is storage.
// Resolve and validate the entire batch before mutating a host image copy or
// publishing any guest image, functions or exports through prx_load_module.
bool bind_imports(const MagV212PaceModule* prepared,std::vector<uint8_t>& image) {
    const auto* m=prepared->module;
    const auto count=prepared->import_reference_count;
    if(count>4096u || (count&&!prepared->import_references))return false;
    struct Binding {uint32_t offset,value;};
    std::vector<Binding> bindings;
    bindings.reserve(count);
    for(uint32_t i=0;i<count;++i) {
        const auto& ref=prepared->import_references[i];
        if(!ref.library || !*ref.library || ref.type!=1 || ref.addend ||
           ref.kind>PRX_EXPORT_VARIABLE || ref.target%4 || ref.target<m->base ||
           uint64_t(ref.target)+4>uint64_t(m->base)+m->image_size)return false;
        const uint32_t offset=ref.target-m->base;
        const auto* source=m->image+offset;
        const uint32_t expected=uint32_t(source[0])<<24|uint32_t(source[1])<<16|
                                uint32_t(source[2])<<8|source[3];
        if(expected!=ref.expected)return false;
        for(const auto& previous:bindings)if(previous.offset==offset)return false;
        const uint32_t value=prx_resolve_library_export(ref.library,ref.nid,
                                                       static_cast<prx_export_kind>(ref.kind));
        const uint32_t width=ref.kind==PRX_EXPORT_FUNCTION?8:1;
        if(!value || !range(value,width) || (ref.kind==PRX_EXPORT_FUNCTION && value%4)) {
            std::fprintf(stderr,"[mag-v212-pace] unresolved or invalid typed import linkage\n");
            return false;
        }
        bindings.push_back({offset,value});
    }
    image.assign(m->image,m->image+m->image_size);
    for(const auto& binding:bindings)
        for(unsigned i=0;i<4;++i)image[binding.offset+i]=static_cast<uint8_t>(binding.value>>(24-i*8));
    return true;
}
void load(ppu_context* ctx) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    auto* prepared=mag_v212_pace_prepared_module();
    if(ctx->gpr[4] || ctx->gpr[5]) { result(ctx,invalid); return; }
    if(!prepared || !prepared->module || !path_matches(ctx->gpr[3],prepared->guest_path)) {
        std::fprintf(stderr,"[mag-v212-pace] unsupported module path\n");
        result(ctx,unknown); return;
    }
    if(state!=State::absent) { result(ctx,library_found); return; }
    auto* m=prepared->module;
    if(m->base!=0x21200000u || !m->image || !m->image_size || m->image_size>0x100000u ||
       !range(m->base,m->image_size)) { result(ctx,module_error); return; }
    std::vector<uint8_t> image;
    try {
        if(!bind_imports(prepared,image)) { result(ctx,module_error); return; }
    } catch(const std::bad_alloc&) { result(ctx,module_error); return; }
    prx_module bound=*m;bound.image=image.data();
    if(!prx_load_module(&bound,registrar).ok) { result(ctx,module_error); return; }
    state=State::initialized;
    std::fprintf(stderr,"[mag-v212-pace] loaded original module handle=0x%x\n",handle);
    result(ctx,handle);
}
void lifecycle(ppu_context* ctx,bool start) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if(ctx->gpr[3]!=handle || state==State::absent) { result(ctx,unknown); return; }
    const uint64_t args=ctx->gpr[4],argp=ctx->gpr[5],destination=ctx->gpr[6];
    if(args>UINT32_MAX || argp>UINT32_MAX || !range(destination,4) || destination%4 ||
       ctx->gpr[7] || ctx->gpr[8] || (args && !range(argp,args))) { result(ctx,invalid); return; }
    if(start && state==State::started) { result(ctx,already_started); return; }
    if(!start && state==State::initialized) { result(ctx,not_started); return; }
    if(!start && state==State::stopped) { result(ctx,already_stopped); return; }
    if(state==State::starting || state==State::stopping) { result(ctx,module_error); return; }
    auto* prepared=mag_v212_pace_prepared_module();
    auto opd=start?prepared->start_opd:prepared->stop_opd;
    auto* m=prepared->module;
    if(opd<m->base || uint64_t(opd)+8>uint64_t(m->base)+m->image_size) { result(ctx,module_error); return; }
    const uint32_t code=vm_read32(opd),toc=vm_read32(opd+4);
    if(code<m->base || code>=uint64_t(m->base)+m->image_size || code%4 ||
       toc<m->base || toc>=uint64_t(m->base)+0x100000u) { result(ctx,module_error); return; }
    // Like the SDK's real-PRX import bridge, reserve a lower stack slice for
    // host-to-guest reentry. The callee's TOC save must not overwrite the
    // original caller's ABI linkage area at r1+0x28.
    if(ctx->gpr[1]<0x1100u) { result(ctx,invalid); return; }
    const uint64_t entry_sp=(ctx->gpr[1]-0x1000u)&~uint64_t(15);
    if(!range(entry_sp,0x1000u)) { result(ctx,invalid); return; }
    const auto previous=state;
    state=start?State::starting:State::stopping;
    uint64_t saved[32];std::memcpy(saved,ctx->gpr,sizeof(saved));
    const uint64_t saved_lr=ctx->lr,saved_ctr=ctx->ctr;
    ctx->gpr[1]=entry_sp;ctx->gpr[3]=args;ctx->gpr[4]=argp;ctx->gpr[2]=toc;ctx->ctr=code;
    ps3_indirect_call(ctx);
    const uint32_t module_result=static_cast<uint32_t>(ctx->gpr[3]);
    std::memcpy(ctx->gpr,saved,sizeof(saved));ctx->lr=saved_lr;ctx->ctr=saved_ctr;
    vm_write32(destination,module_result);
    state=module_result==0?(start?State::started:State::stopped):previous;
    std::fprintf(stderr,"[mag-v212-pace] original module_%s result=0x%08x\n",start?"start":"stop",module_result);
    result(ctx,0); // Wrapper success is separate from the module's guest result.
}
void start(ppu_context* ctx) { lifecycle(ctx,true); }
void stop(ppu_context* ctx) { lifecycle(ctx,false); }
void unload(ppu_context* ctx) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if(ctx->gpr[3]!=handle || state==State::absent) { result(ctx,unknown); return; }
    if(ctx->gpr[4] || ctx->gpr[5]) { result(ctx,invalid); return; }
    std::fprintf(stderr,"[mag-v212-pace] unload unsupported by resident export registry\n");
    result(ctx,not_removable);
}
}
extern "C" void mag_v212_pace_register(void) {
    static std::once_flag once;
    std::call_once(once,[] {
        ps3_hle_register_ctx(0x26090058u,"sys_prx_load_module",load);
        ps3_hle_register_ctx(0x9f18429du,"sys_prx_start_module",start);
        ps3_hle_register_ctx(0x80fb0c19u,"sys_prx_stop_module",stop);
        ps3_hle_register_ctx(0xf0aece0du,"sys_prx_unload_module",unload);
    });
}
