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

int main() {
    const MagExtensionCallbacks* none = mag_extension();
    assert(none && !none->start && !none->frame_poll && !none->stop && !none->trace_enter && !none->payload_select && !none->prepare);

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
