#include "v212_launcher_hooks.h"
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
void ppu_register_function(uint64_t, void (*)(ppu_context*));
}
void func_000153D0(ppu_context*);
void func_00011088(ppu_context*);
void func_0026FE00(ppu_context*);
void func_000594C8(ppu_context*);

// Behaviour specification: in the exact 2.12 launcher, an explicitly selected
// local no-ticket profile may restore the original false setting after retail
// init overwrites it. A selected valid short account name may fill an empty,
// unlocked configuration through its original setter. Existing names, NP status
// and connection state remain owned by the guest. All mapping guards must hold.
namespace {
[[noreturn]] void reject(const char* reason) {
    std::fprintf(stderr,"[mag-v212-net] rejected: %s\n",reason);
    std::fflush(stderr);
    std::_Exit(89);
}
void range(uint64_t address,uint64_t length) {
    const uint64_t end=ppu_vm_size?ppu_vm_size:UINT64_C(0x100000000);
    if (!vm_base || !address || address>=end || length>end-address)
        reject("guest range outside known VM bounds");
}
unsigned config() {
    range(0x428fe4,4);
    const uint64_t root=vm_read32(0x428fe4);
    if (!root) reject("missing launcher configuration");
    range(root,0x28+0xde);
    return static_cast<unsigned>(root)+0x28;
}
void supply_account(ppu_context* ctx,unsigned cfg) {
    const char* selected=std::getenv("MAG_LOCAL_ACCOUNT_NAME");
    if (!selected) return;
    if (!ctx || !mag_local_account_name_valid(selected)) reject("invalid selected local account name");
    const unsigned length=static_cast<unsigned>(std::strlen(selected)),name=cfg+0x4c;
    bool matches=true;
    for (unsigned i=0;i<=length;++i) matches &= vm_read8(name+8+i)==static_cast<unsigned char>(selected[i]);
    const int action=mag_local_account_policy(true,vm_read32(0x58bb4),vm_read8(cfg+0xd5),
        vm_read8(cfg+0xdd),vm_read32(name+0x18),vm_read32(name+0x1c),length,matches);
    if (action<0) reject("local account configuration or existing identity mismatch");
    if (!action) return;
    if (ctx->gpr[1]<0x200 || ctx->gpr[1]>UINT32_MAX) reject("invalid private guest call frame");
    ppu_context call=*ctx;
    call.gpr[1]=(ctx->gpr[1]-0x200)&~uint64_t(15);
    range(call.gpr[1],0x200);
    const unsigned source=static_cast<unsigned>(call.gpr[1])+0x80;
    for (unsigned i=0;i<32;++i) vm_write8(source+i,vm_read8(name+i));
    for (unsigned i=0;i<=length;++i) vm_write8(source+8+i,static_cast<unsigned char>(selected[i]));
    vm_write32(source+0x18,length);
    call.gpr[3]=cfg;call.gpr[4]=source;
    func_000594C8(&call);
    matches=vm_read32(name+0x18)==length;
    for (unsigned i=0;i<=length;++i) matches &= vm_read8(name+8+i)==static_cast<unsigned char>(selected[i]);
    if (!matches) reject("original account setter did not retain selected identity");
    std::fprintf(stderr,"[mag-v212-net] original setter supplied selected local identity; NP remains offline\n");
}
void ticket_use(ppu_context* ctx) {
    if (!ctx) reject("missing PPU context");
    if (std::getenv("MAG_LOCAL_NO_TICKET")) {
        const unsigned cfg=config();
        range(ctx->gpr[3],0x228a);
        const unsigned object=static_cast<unsigned>(ctx->gpr[3]);
        uint8_t effective=vm_read8(object+0x2289);
        const int action=mag_v212_restore_local_ticket(true,vm_read32(0x13780),vm_read32(0x13784),
                                                       vm_read8(cfg+0xd5),effective);
        if (action<0) reject("local ticket code or configuration mismatch");
        if (action) {
            vm_write8(object+0x2289,effective);
            std::fprintf(stderr,"[mag-v212-net] restored configured UseNPTicket=0\n");
        }
        supply_account(ctx,cfg);
    }
    func_0026FE00(ctx);
}
void settings(ppu_context* ctx) {
    func_000153D0(ctx);
    const unsigned cfg=config();
    std::fprintf(stderr,"[mag-v212-net] settings ticket=%u handled=%u\n",vm_read8(cfg+0xd5),static_cast<unsigned>(ctx->gpr[3]));
}
void event(ppu_context* ctx) {
    if (!ctx) reject("missing PPU context");
    range(ctx->gpr[3],0x21c);
    const unsigned object=static_cast<unsigned>(ctx->gpr[3]);
    std::fprintf(stderr,"[mag-v212-net] launcher event=%u error=%08X state=%u\n",static_cast<unsigned>(ctx->gpr[4]),static_cast<unsigned>(ctx->gpr[5]),vm_read32(object+0x218));
    func_00011088(ctx);
    std::fprintf(stderr,"[mag-v212-net] launcher after event state=%u\n",vm_read32(object+0x218));
}
}

extern "C" void mag_v212_launcher_register() {
    if (std::getenv("MAG_LOCAL_NO_TICKET") && std::getenv("PS3RECOMP_NP_TICKET_FILE"))
        reject("local no-ticket and NP ticket file select different profiles");
    if (!std::getenv("MAG_LOCAL_NO_TICKET") && !std::getenv("MAG_TRACE_NETWORK")) return;
    // These are instruction identities, not substitutes for the supervisor's
    // full exact-ELF SHA256 and semantic span verification.
    constexpr unsigned guards[]={0x13780u,0x13784u,0x58bb4u,0x26fe00u,0x594c8u};
    for (unsigned address:guards) range(address,4);
    if (vm_read32(0x13780)!=0x38000001 || vm_read32(0x13784)!=0x98092289 ||
        vm_read32(0x58bb4)!=0x3863004c || vm_read32(0x26fe00)!=0x88632289 ||
        vm_read32(0x594c8)!=0xf821ff51) reject("2.12 launcher instruction identity mismatch");
    ppu_register_function(0x153d0,settings);
    ppu_register_function(0x26fe00,ticket_use);
    ppu_register_function(0x11088,event);
}
