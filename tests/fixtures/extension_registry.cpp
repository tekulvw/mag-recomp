/* Synthetic check of the extension callback registry; no title code. */
#include "mag_extension.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static int starts, polls, enters, exits, prepares;
static void on_prepare(const char* elf){assert(std::strcmp(elf,"fixture.elf")==0);++prepares;}
static uint64_t seen_scratch;
static void on_start(const char* elf) { assert(std::strcmp(elf, "fixture.elf") == 0); starts++; }
static void on_poll(void) { polls++; }
static void on_enter(void*, uint32_t function, const uint64_t args[3], uint64_t scratch[8]) {
    assert(function == 0x1234 && args[2] == 7); scratch[7] = 0xfeedu; enters++;
}
static void on_exit(void*, uint32_t function, const uint64_t[3], int completed, uint64_t scratch[8]) {
    assert(function == 0x1234 && completed == 1); seen_scratch = scratch[7]; exits++;
}

int main() {
    const MagExtensionCallbacks* none = mag_extension();
    assert(none && !none->start && !none->frame_poll && !none->trace_enter && !none->payload_select && !none->prepare);

    MagExtensionCallbacks a = {};
    a.prepare = on_prepare; a.start = on_start; a.trace_enter = on_enter; a.trace_exit = on_exit;
    assert(mag_extension_register(&a) == 0);

    MagExtensionCallbacks double_registration = {};
    double_registration.start = on_start; double_registration.frame_poll = on_poll;
    assert(mag_extension_register(&double_registration) == -1);
    assert(!mag_extension()->frame_poll);          /* a refused registration changes nothing */

    MagExtensionCallbacks b = {};
    b.frame_poll = on_poll;
    assert(mag_extension_register(&b) == 0);

    const MagExtensionCallbacks* x = mag_extension();
    x->prepare("fixture.elf");assert(prepares==1 && starts==0);
    x->start("fixture.elf"); x->frame_poll();
    uint64_t args[3] = {0, 0, 7}, scratch[8] = {};
    x->trace_enter(nullptr, 0x1234, args, scratch);
    x->trace_exit(nullptr, 0x1234, args, 1, scratch);
    assert(starts == 1 && polls == 1 && enters == 1 && exits == 1 && seen_scratch == 0xfeedu);
    std::puts("PASS extension registry");
    return 0;
}
