/* Synthetic check of the extension callback registry; no title code. */
#include "mag_extension.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

static int polls, enters, exits, prepares;
static std::string order;
static uint64_t seen_scratch;
static void on_prepare(const char* elf) { assert(std::strcmp(elf, "fixture.elf") == 0); ++prepares; }
static void on_start(const char* elf) { assert(std::strcmp(elf, "fixture.elf") == 0); order += "a"; }
static void on_start_b(const char* elf) { assert(std::strcmp(elf, "fixture.elf") == 0); order += "b"; }
static void on_stop(void) { order += "A"; }
static void on_stop_b(void) { order += "B"; }
static void on_poll(void) { polls++; }
static void on_poll_extra(void) {}
static void on_enter(void*, uint32_t function, const uint64_t args[3], uint64_t scratch[8]) {
    assert(function == 0x1234 && args[2] == 7); scratch[7] = 0xfeedu; enters++;
}
static void on_exit(void*, uint32_t function, const uint64_t[3], int completed, uint64_t scratch[8]) {
    assert(function == 0x1234 && completed == 1); seen_scratch = scratch[7]; exits++;
}

static int override_calls;
static std::string override_order;
static bool override_none(void*, uint32_t) { override_order += "n"; ++override_calls; return false; }
static bool override_first(void* ctx, uint32_t target) {
    override_order += "f"; ++override_calls; assert(ctx == &override_calls); return target == 0x15898u;
}

int main() {
    /* With nothing registered the seam reports "not handled" and calls nothing. */
    assert(mag_payload_override(&override_calls, 0x15898u) == 0 && override_calls == 0);

    const MagExtensionCallbacks* none = mag_extension();
    assert(none && !none->start && !none->frame_poll && !none->stop && !none->trace_enter && !none->payload_select &&
           !none->prepare && !none->payload_override);

    MagExtensionCallbacks a = {};
    a.prepare = on_prepare; a.start = on_start; a.stop = on_stop; a.trace_enter = on_enter; a.trace_exit = on_exit;
    assert(mag_extension_register(&a) == 0);
    /* One registrant: its functions sit in the slots unchanged. */
    assert(mag_extension()->start == on_start && mag_extension()->stop == on_stop && mag_extension()->prepare == on_prepare);

    /* A single-owner slot already taken refuses the whole registration, lifecycle slots included. */
    MagExtensionCallbacks clash = {};
    clash.start = on_start_b; clash.frame_poll = on_poll; clash.trace_enter = on_enter;
    assert(mag_extension_register(&clash) == -1);
    assert(!mag_extension()->frame_poll && mag_extension()->start == on_start);

    MagExtensionCallbacks b = {};
    b.start = on_start_b; b.stop = on_stop_b; b.frame_poll = on_poll;
    assert(mag_extension_register(&b) == 0);

    const MagExtensionCallbacks* x = mag_extension();
    assert(x->frame_poll == on_poll && x->start != on_start);
    x->prepare("fixture.elf"); assert(prepares == 1 && order.empty());
    x->start("fixture.elf"); x->frame_poll(); x->stop();
    assert(order == "abBA" && polls == 1);
    uint64_t args[3] = {0, 0, 7}, scratch[8] = {};
    x->trace_enter(nullptr, 0x1234, args, scratch);
    x->trace_exit(nullptr, 0x1234, args, 1, scratch);
    assert(enters == 1 && exits == 1 && seen_scratch == 0xfeedu);

    /* Payload overrides are asked in registration order until one handles the payload. */
    MagExtensionCallbacks o1 = {}, o2 = {};
    o1.payload_override = override_none; o2.payload_override = override_first;
    assert(mag_extension_register(&o1) == 0);
    assert(mag_extension()->payload_override == override_none);
    assert(mag_payload_override(&override_calls, 0x15898u) == 0 && override_order == "n");
    assert(mag_extension_register(&o2) == 0);
    override_order.clear();
    assert(mag_payload_override(&override_calls, 0x15898u) == 1 && override_order == "nf");
    override_order.clear();
    assert(mag_payload_override(&override_calls, 0x12D28u) == 0 && override_order == "nf");
    for (int i = 0; i < 6; ++i) assert(mag_extension_register(&o1) == 0);
    assert(mag_extension_register(&o1) == -1);

    /* Lifecycle slots hold eight registrants; a ninth is refused and changes nothing. */
    MagExtensionCallbacks extra = {};
    extra.frame_poll = on_poll_extra;
    for (int i = 0; i < 7; ++i) assert(mag_extension_register(&extra) == 0);
    MagExtensionCallbacks ninth = {};
    ninth.frame_poll = on_poll_extra; ninth.payload_select = nullptr; ninth.start = on_start;
    const auto before = mag_extension()->start;
    assert(mag_extension_register(&ninth) == -1 && mag_extension()->start == before);
    polls = 0; mag_extension()->frame_poll(); assert(polls == 1);
    std::puts("PASS extension registry");
    return 0;
}
