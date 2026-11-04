#include "mag_extension.h"
#include <cstddef>
#include <mutex>

namespace {
/* Lifecycle slots (prepare, start, frame_poll, stop) accept several registrants, called in
 * registration order (stop in reverse). With one registrant the slot holds its function
 * directly, exactly as before; with none it stays null. The trace and payload slots share
 * per-call scratch or return a decision, so each keeps a single owner. */
constexpr std::size_t kMaxRegistrants = 8;

template <typename Fn> struct Fanout {
    Fn fn[kMaxRegistrants]{};
    std::size_t count{};
};

MagExtensionCallbacks registry{};
Fanout<void (*)(const char*)> prepares, starts;
Fanout<void (*)(void)> polls, stops;
std::mutex registry_lock;

void fan_prepare(const char* elf) { for (std::size_t i = 0; i < prepares.count; ++i) prepares.fn[i](elf); }
void fan_start(const char* elf) { for (std::size_t i = 0; i < starts.count; ++i) starts.fn[i](elf); }
void fan_poll(void) { for (std::size_t i = 0; i < polls.count; ++i) polls.fn[i](); }
void fan_stop(void) { for (std::size_t i = stops.count; i > 0; --i) stops.fn[i - 1](); }

template <typename Fn> bool full(const Fanout<Fn>& list, Fn incoming) { return incoming && list.count == kMaxRegistrants; }
template <typename Fn> void add(Fanout<Fn>& list, Fn& slot, Fn incoming, Fn fan) {
    if (!incoming) return;
    list.fn[list.count++] = incoming;
    slot = list.count == 1 ? incoming : fan;
}
template <typename Slot> bool taken(Slot current, Slot incoming) { return current && incoming; }
template <typename Slot> void take(Slot& current, Slot incoming) { if (incoming) current = incoming; }
}

extern "C" int mag_extension_register(const MagExtensionCallbacks* c) {
    if (!c) return -1;
    std::lock_guard<std::mutex> guard(registry_lock);
    MagExtensionCallbacks& r = registry;
    if (full(prepares, c->prepare) || full(starts, c->start) || full(polls, c->frame_poll) || full(stops, c->stop) ||
        taken(r.trace_enter, c->trace_enter) || taken(r.trace_exit, c->trace_exit) ||
        taken(r.payload_select, c->payload_select) || taken(r.payload_begin, c->payload_begin))
        return -1;
    add(prepares, r.prepare, c->prepare, fan_prepare);
    add(starts, r.start, c->start, fan_start);
    add(polls, r.frame_poll, c->frame_poll, fan_poll);
    add(stops, r.stop, c->stop, fan_stop);
    take(r.trace_enter, c->trace_enter); take(r.trace_exit, c->trace_exit);
    take(r.payload_select, c->payload_select); take(r.payload_begin, c->payload_begin);
    return 0;
}

extern "C" const MagExtensionCallbacks* mag_extension(void) { return &registry; }
