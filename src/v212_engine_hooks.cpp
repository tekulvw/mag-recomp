#include "v212_engine_hooks.h"
#include "retail_network_policy.h"
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
void ppu_register_function(uint64_t, void (*)(ppu_context*));
}
void func_00449F88(ppu_context*);
void func_00109FA8(ppu_context*);
void func_00156CB4(ppu_context*);

namespace {
[[noreturn]] void reject(const char* reason) {
    std::fprintf(stderr, "[mag-v212-engine-net] rejected: %s\n", reason);
    std::fflush(stderr);
    std::_Exit(89);
}
void range(uint64_t address, uint64_t length) {
    const uint64_t end = ppu_vm_size ? ppu_vm_size : UINT64_C(0x100000000);
    if (!vm_base || !address || address >= end || length > end-address)
        reject("guest range outside known VM bounds");
}
unsigned config() {
    range(0x163c178, 4);
    const uint64_t root = vm_read32(0x163c178);
    if (!root) reject("missing engine configuration");
    range(root, 0x28+0xde);
    return static_cast<unsigned>(root)+0x28;
}
void supply_account(ppu_context* ctx, unsigned cfg, uint64_t caller_lr) {
    const char* selected = std::getenv("MAG_LOCAL_ACCOUNT_NAME");
    if (!selected) return;
    if (!ctx || !mag_local_account_name_valid(selected))
        reject("invalid selected local account name");
    const unsigned length = static_cast<unsigned>(std::strlen(selected));
    const unsigned name = cfg+0x4c;
    bool matches = true;
    for (unsigned i=0; i<=length; ++i)
        matches &= vm_read8(name+8+i) == static_cast<unsigned char>(selected[i]);
    const unsigned getter = vm_read32(0x1093bc);
    const unsigned tickets = vm_read8(cfg+0xd5), locked = vm_read8(cfg+0xdd);
    const unsigned guest_length = vm_read32(name+0x18), capacity = vm_read32(name+0x1c);
    const int action = mag_local_account_policy(true, getter, tickets, locked,
        guest_length, capacity, length, matches);
    if (action < 0) {
        // Diagnostic metadata only: retain the exact rejecting inputs without
        // printing either identity or changing the conservative account policy.
        std::fprintf(stderr, "[mag-v212-engine-net] account policy rejected cfg=0x%08X "
            "length=%u capacity=%u selected_length=%u locked=%u ticket=%u matches=%u "
            "getter=0x%08X caller_lr=0x%08llX\n", cfg, guest_length, capacity, length,
            locked, tickets, matches ? 1u : 0u, getter,
            static_cast<unsigned long long>(caller_lr));
        reject("local account conflicts with guest configuration");
    }
    if (!action) return;
    if (ctx->gpr[1] < 0x2b0) reject("invalid private guest call frame");
    ppu_context call = *ctx;
    call.gpr[1] = (ctx->gpr[1]-0x200)&~uint64_t(15);
    // The original setter itself opens a further 0xb0-byte frame.
    range(call.gpr[1]-0xb0, 0x2b0);
    const unsigned source = static_cast<unsigned>(call.gpr[1])+0x80;
    for (unsigned i=0; i<32; ++i) vm_write8(source+i, vm_read8(name+i));
    for (unsigned i=0; i<=length; ++i)
        vm_write8(source+8+i, static_cast<unsigned char>(selected[i]));
    vm_write32(source+0x18, length);
    call.gpr[3] = cfg;
    call.gpr[4] = source;
    // Reader TOC is 0x15fc994. The setter's original cross-TOC thunk
    // 0xb83074 selects 0x15eca10 before branching to 0x109fa8.
    call.gpr[2] = 0x15eca10;
    func_00109FA8(&call);
    matches = vm_read32(name+0x18) == length;
    for (unsigned i=0; i<=length; ++i)
        matches &= vm_read8(name+8+i) == static_cast<unsigned char>(selected[i]);
    if (!matches) reject("original account setter did not retain selected identity");
    std::fprintf(stderr, "[mag-v212-engine-net] original setter supplied selected local identity\n");
}
void settings(ppu_context* ctx) {
    if (!ctx) reject("missing PPU context");
    // Settings dispatch can run repeatedly while the original parser is still
    // applying other tables, including MAGCharacterData.PlayerName. Observe
    // this reader; identity policy belongs at actual connection consumption.
    func_00449F88(ctx);
    if (ctx->gpr[3]) {
        const unsigned cfg = config();
        std::fprintf(stderr, "[mag-v212-engine-net] settings ticket=%u handled=%u\n",
            vm_read8(cfg+0xd5), static_cast<unsigned>(ctx->gpr[3]));
    }
}
void connect(ppu_context* ctx) {
    if (!ctx) reject("missing PPU context");
    if (std::getenv("MAG_LOCAL_NO_TICKET")) {
        const unsigned cfg = config();
        if (vm_read8(cfg+0xd5)) reject("configured UseNPTicket is not false");
        supply_account(ctx, cfg, ctx->lr);
    }
    // r3=original client, r4=host, r5=port. The original routine owns transport,
    // authentication and return value; the private setter uses a copied context.
    func_00156CB4(ctx);
}
}

extern "C" void mag_v212_engine_register() {
    const char* local = std::getenv("MAG_LOCAL_NO_TICKET");
    if (local && std::strcmp(local, "1")) reject("local no-ticket selection must be explicit");
    if (local && std::getenv("PS3RECOMP_NP_TICKET_FILE"))
        reject("local no-ticket and NP ticket file select different profiles");
    if (std::getenv("MAG_LOCAL_ACCOUNT_NAME") && !local)
        reject("local account requires explicit no-ticket profile");
    if (!local && !std::getenv("MAG_TRACE_NETWORK")) return;
    constexpr unsigned guards[] = {0x449f88u, 0x1093bcu, 0x109fa8u,
        0x109fb8u, 0x109410u, 0x152660u, 0xc26ab0u, 0x15b9fa8u, 0x15b9facu,
        0x156cb4u, 0x15a1e68u, 0x15a1e6cu, 0x153ea48u};
    for (unsigned address : guards) range(address, 4);
    if (vm_read32(0x449f88) != 0x81221780 ||
        vm_read32(0x1093bc) != 0x3863004c ||
        vm_read32(0x109fa8) != 0xf821ff51 ||
        vm_read32(0x109fb8) != 0x880300dd ||
        vm_read32(0x109410) != 0x886300d5 ||
        vm_read32(0x152660) != 0x987f2289 ||
        vm_read32(0xc26ab0) != 0x88632289 ||
        vm_read32(0x15b9fa8) != 0x449f88 ||
        vm_read32(0x15b9fac) != 0x15fc994 ||
        vm_read32(0x156cb4) != 0xf821fe11 ||
        vm_read32(0x15a1e68) != 0x156cb4 ||
        vm_read32(0x15a1e6c) != 0x15eca10 ||
        vm_read32(0x153ea48) != 0x15a1e68)
        reject("2.12 engine instruction or callback identity mismatch");
    ppu_register_function(0x449f88, settings);
    ppu_register_function(0x156cb4, connect);
}
