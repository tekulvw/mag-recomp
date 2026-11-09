/* Synthetic policy seam: an unregistered override leaves the lifted call in place; a handling
 * override replaces it. No title code. */
#include "mag_extension.h"
#include <cassert>
#include <cstdio>

extern "C" {
typedef struct spu_context { unsigned pc; int host_depth; } spu_context;
int lifted_runs;
void mag_wws_policy_spu_func_00003190(spu_context* ctx);
}

static int handled;
static bool take_one(void* opaque, uint32_t target) {
    auto* ctx = static_cast<spu_context*>(opaque);
    if (target != 0x15898u) return false;
    ++handled; ctx->pc = 0x3260u;
    return true;
}

int main() {
    spu_context ctx{0x15898u, 0};
    mag_wws_policy_spu_func_00003190(&ctx);
    assert(lifted_runs == 1 && ctx.pc == 0x3260u && ctx.host_depth == 0);
    MagExtensionCallbacks callbacks = {};
    callbacks.payload_override = take_one;
    assert(mag_extension_register(&callbacks) == 0);
    ctx.pc = 0x15898u;
    mag_wws_policy_spu_func_00003190(&ctx);
    assert(lifted_runs == 1 && handled == 1 && ctx.pc == 0x3260u && ctx.host_depth == 0);
    ctx.pc = 0x12D28u;
    mag_wws_policy_spu_func_00003190(&ctx);
    assert(lifted_runs == 2 && handled == 1);
    std::puts("PASS payload override seam");
    return 0;
}
