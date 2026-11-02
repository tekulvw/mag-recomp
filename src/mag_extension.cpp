#include "mag_extension.h"
#include <mutex>

namespace {
MagExtensionCallbacks registry{};
std::mutex registry_lock;

template <typename Slot> bool taken(Slot current, Slot incoming) { return current && incoming; }
template <typename Slot> void take(Slot& current, Slot incoming) { if (incoming) current = incoming; }
}

extern "C" int mag_extension_register(const MagExtensionCallbacks* c) {
    if (!c) return -1;
    std::lock_guard<std::mutex> guard(registry_lock);
    MagExtensionCallbacks& r = registry;
    if (taken(r.start, c->start) || taken(r.frame_poll, c->frame_poll) || taken(r.stop, c->stop) ||
        taken(r.trace_enter, c->trace_enter) || taken(r.trace_exit, c->trace_exit) ||
        taken(r.payload_select, c->payload_select) || taken(r.payload_begin, c->payload_begin) || taken(r.prepare, c->prepare))
        return -1;
    take(r.start, c->start); take(r.frame_poll, c->frame_poll); take(r.stop, c->stop);
    take(r.trace_enter, c->trace_enter); take(r.trace_exit, c->trace_exit);
    take(r.payload_select, c->payload_select); take(r.payload_begin, c->payload_begin);take(r.prepare,c->prepare);
    return 0;
}

extern "C" const MagExtensionCallbacks* mag_extension(void) { return &registry; }
