/* The game controls adapter (docs/native-controls.md, sections 4, 6, 7 and 8). Seam events arrive
 * on the game's update thread; the adapter decides from guest state (mag_game_controls_guest.h)
 * whether a control is taken over, and the portable input engine decides what it reads. */
#include "mag_game_controls_adapter.h"
#include "mag_game_controls_catalog.h"
#include "mag_game_controls_guest.h"
#include "mag_game_controls_mouse_aim.h"
#include "player_controls_rust.h"
#include "player_input_capture.h"
#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using namespace mag;
using namespace mag_guest;
using Hook = MagControlsHook;

namespace {
// LAB trace (never commit): wall-clock ms.
long long lab_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
// Poll numbers are unique in the process so that a new session never reuses an engine sample key.
std::atomic<uint64_t> poll_counter{};

constexpr const char* InvalidState = "rejected unavailable or invalid state";
constexpr const char* MapChanged = "native control map changed; waiting for fresh input";

// Mouse as the right stick (7.5): a mouse turning this fast pushes the stick fully, and the poll
// interval the conversion assumes is kept within these bounds (seconds).
constexpr double FullStickRate = 3.0, MinStickInterval = 1.0 / 240, MaxStickInterval = 1.0 / 10, FirstStickInterval = 1.0 / 30;

// Cycle weapons: the action on R2, a turret's secondary weapon (2.3).
constexpr ActionId SeatSecondary = 0x2C4380C4;
// The actions whose buttons a vehicle driver's handler watches (2.3): both triggers (Cycle weapons and
// Cycle equipment) and the left stick's horizontal (Strafe left and right).
constexpr ActionId DriveActions[] = {0x2C4380C4, 0xCB535315, 0x44533986, 0xF3468EB5};

// Caller return addresses of the two angle-rate helpers (section 3).
constexpr uint64_t BasicYawCaller = 0x0061E7B4, SprintYawCaller = 0x00675CF0, SprintPitchCaller = 0x00676D78;

// The engine's portable event flags and the game's published event bits (2.2).
constexpr std::pair<uint32_t, uint32_t> EventBits[] = {
    {ControlChange, 0x0001}, {ControlDown, 0x0002}, {ControlUp, 0x0004}, {ControlRepeat, 0x0008},
    {ControlTap, 0x0010}, {ControlDoubleTap, 0x0020}, {ControlSingleTap, 0x0040}, {ControlPress, 0x0080},
    {ControlRelease, 0x0100}, {ControlTapDown, 0x0200}, {ControlTapPress, 0x0400}, {ControlTapRelease, 0x0800},
    {ControlWhileDown, 0x1000}, {ControlWhilePressed, 0x2000}};

uint32_t game_event_bits(uint32_t events) {
    uint32_t bits = 0;
    for (auto [engine, game] : EventBits)
        if (events & engine) {
            bits |= game;
            events &= ~engine;
        }
    if (events) throw std::runtime_error("engine reported an event the game cannot represent");
    return bits;
}

// A register holding a 32-bit guest address; anything wider is no address (0).
uint32_t address(uint64_t value) { return value <= 0xFFFFFFFFull ? uint32_t(value) : 0; }

uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A sum that must stay a finite single-precision angle; the result is rounded to single precision.
double single_sum(double base, double delta) {
    const double sum = base + delta;
    if (!std::isfinite(sum) || std::abs(sum) > std::numeric_limits<float>::max())
        throw std::runtime_error("mouse look left the single-precision range");
    return double(float(sum));
}

// Seconds from a tuning record as whole nanoseconds.
uint64_t duration_ns(float seconds) {
    const double ns = std::nearbyint(double(seconds) * 1e9);
    if (!std::isfinite(ns) || ns < 0 || ns >= 18446744073709551616.0) throw std::runtime_error("invalid tuning duration");
    return uint64_t(ns);
}

// The game's value curves (2.2), in single precision; v is already in [0, 1].
float value_curve(uint32_t kind, float v) {
    switch (kind) {
    case 0: return v;
    case 1: return v * v;
    case 2: return v >= 1.0f ? 1.0f : 0.0f;
    case 3: return v >= 0.5f ? 1.0f : 0.0f;
    case 4: return v != 0.0f ? 1.0f : 0.0f;
    default: throw std::runtime_error("unsupported native value curve");
    }
}

bool system_action(ActionId action) {
    const auto* entry = mag_game_controls_action(action);
    return entry && entry->system_channel >= 0;
}
// Situational actions are recognized by their own token only (7.3) and never required (7.4).
bool optional_action(ActionId action) {
    const auto* entry = mag_game_controls_action(action);
    return entry && entry->optional;
}

bool contains(const std::vector<uint32_t>& values, uint32_t value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Handler lifecycle bookkeeping (6.6). Only data; the adapter applies its consequences.
struct Lifecycle {
    static constexpr size_t MaxHandlers = 4096, MaxScopes = 16;
    struct Pending {
        uint64_t lifetime{};
        bool ready{};
    };
    struct Construction {
        uint32_t handler, priority, slot;
        uint64_t lifetime;
        bool tracked;
    };
    struct Addition {
        uint32_t handler, manager;
        uint64_t lifetime;
    };
    struct Refresh {
        uint32_t handler;
        std::optional<Pending> pending;
    };
    std::set<uint32_t> tombstoned, needs_init;
    std::map<uint32_t, uint64_t> lifetimes;
    std::map<uint32_t, Pending> additions;
    std::vector<Construction> constructions;
    std::vector<Addition> adds;
    std::vector<Refresh> refreshes;
    bool exhausted{};

    uint64_t lifetime(uint32_t handler) const {
        const auto it = lifetimes.find(handler);
        return it == lifetimes.end() ? 0 : it->second;
    }
    bool is_tombstoned(uint32_t handler) const { return tombstoned.count(handler) != 0; }
    // False when a new handler would exceed the counter capacity.
    bool advance(uint32_t handler) {
        if (!lifetimes.count(handler) && lifetimes.size() >= MaxHandlers) return false;
        ++lifetimes[handler];
        return true;
    }
    template <class T>
    static void push(std::vector<T>& stack, T value) {
        if (stack.size() >= MaxScopes) throw std::runtime_error("input lifecycle scope too deep");
        stack.push_back(std::move(value));
    }
    template <class T>
    static std::optional<T> pop(std::vector<T>& stack) {
        if (stack.empty()) return {};
        T value = std::move(stack.back());
        stack.pop_back();
        return value;
    }
};

// Controller buttons fed from the capture layer (8.3): codes 0..31 are buttons, 32 and 33 the
// left and right triggers as digital buttons.
struct ControllerFeed {
    static constexpr unsigned LeftTrigger = 32, RightTrigger = 33;
    using Codes = std::bitset<64>;
    bool seen{};
    uint64_t generation{};
    Codes held, blocked;

    Codes current(const MagInputControllerSample& sample) const {
        Codes codes(sample.buttons);
        const auto trigger = [&](unsigned code, double value) {
            codes[code] = value >= 0.55 || (held[code] && value > 0.45);
        };
        trigger(LeftTrigger, sample.left_trigger);
        trigger(RightTrigger, sample.right_trigger);
        return codes;
    }
};

void push_controller(unsigned code, bool down, uint64_t time) {
    mag_controls_push_event({{InputSource::Controller, code}, down, time, 1});
}

// The owner's watched controls that the profile needs (7.4).
struct Contract {
    uint32_t handler{};
    std::vector<MagGuestControl> controls;
    uint32_t mask{}, look_mask{};
};

// What the adapter last published to one control record.
struct HostCache {
    MagGuestOwner owner;
    uint32_t handler{}, token{}, binding{}, tuning{};
    MagGuestTuple tuple{};
};
struct SystemCache {
    uint32_t handler{}, token{}, binding{}, tuning{};
    MagGuestTuple tuple{};
};
struct RecognitionEntry {
    uint32_t handler{}, token{}, binding{}, tuning{};
    MagGuestTuple tuple{};
    uint64_t id{};
    bool start_blocked{};
    ActionId action{};  // The action the engine recognizes for this record (7.3 step 4).
};

// Fields of a tuning record (2.2) the recognizer uses.
struct Tuning {
    uint32_t curve{};
    float low{}, high{}, epsilon{}, threshold{};
    ControlRecognitionConfig config;
};

struct AssistEntry {
    uint32_t record{}, actor_handle{};
    bool dirty{};
};
struct AssistScope {
    uint32_t camera{}, record{};
};
constexpr size_t MaxAssistEntries = 32, MaxAssistScopes = 16, MaxRecognitionEntries = 1024;
}  // namespace

struct MagGameControlsAdapter::State {
    MagGameControlsGuest guest;
    uint64_t session;
    void (*capture)(bool);
    void (*mouse_capture)(bool);
    bool look_held_by_menu = false, look_held_seen = false;  // applied state; last observed state
    unsigned look_held_streak = 0;
    // Handler updates (about one per frame) a menu's hold on look must last before the lock follows.
    static constexpr unsigned MenuLookStableUpdates = 3;
    ControlsProfile profile;
    std::mutex pending_mutex;
    std::optional<ControlsProfile> pending;
    const char* status = "waiting for input poll";
    std::string error_status;  // status text of the last error (InvalidState and its reason)
    void (*census_sink)(const char*) = nullptr;
    std::set<std::string> census_seen;

    // Scopes on the owning thread.
    std::optional<uint64_t> thread;
    unsigned outer{}, poll{}, handler_depth{}, actor_depth{}, yaw_rate{}, sprint_pitch_rate{};
    // 7.4: whether gameplay routes were available at the start of the current handler update.
    std::optional<bool> update_gameplay_available;
    bool yaw_rate_observed{}, sprint_pitch_observed{};

    // Poll, sample and frame.
    uint64_t poll_number{}, tick{}, attached_tick{}, seen_generation{};
    bool capture_poll_open{}, poll_sealed{}, sealed{};
    std::optional<InputFrameKey> sample_key;
    std::optional<HostInputFrame> frame;

    // Owners, routes and the channel contract.
    std::optional<MagGuestOwner> owner, handler_owner, actor_owner;
    std::optional<Contract> contract;
    bool gameplay_eligible{}, mouse_eligible{};
    // 7.5: the mouse drives the right stick this poll, and the deflection it gives (x right, y up).
    bool stick_look{};
    double stick_x{}, stick_y{};
    uint64_t seal_ns{}, previous_seal_ns{};
    std::optional<InputContext> last_context;
    std::vector<uint32_t> system_routes;
    // 7.2: handlers registered with the input manager other than the player's, and of those the seat
    // handlers of this poll (watching every look channel: a mounted weapon's, a vehicle seat's), the
    // channels seat handlers consumed in this poll, and the open seat handler update.
    std::vector<uint32_t> registered_routes;
    std::set<uint32_t> seat_handlers, drive_handlers;
    uint64_t seat_poll{};
    uint32_t seat_consumed{};
    struct SeatScope {
        uint32_t pad{}, before{};
    };
    std::optional<SeatScope> seat_scope;
    std::optional<std::vector<uint32_t>> gameplay_routes;
    std::map<uint32_t, ActionId> channel_actions;

    // Recognition.
    uint32_t recognition_handler{}, current_control{};
    uint64_t next_control_id{};
    std::map<uint32_t, RecognitionEntry> entries;
    std::map<uint32_t, HostCache> host_caches;
    std::map<uint32_t, SystemCache> system_caches;
    std::map<ActionId, ControlOffer> gameplay_receipts, system_receipts;
    // 7.3: presses made on a menu screen that no handler has read yet, by action: the press
    // (its pulse) and when it was first left unread.
    struct MenuUnread { uint64_t pulse_id, since_ns; };
    std::map<ActionId, MenuUnread> menu_unread;

    MagGameControlsMouseAim latch;
    std::optional<MagMouseAimIdentity> latch_identity;
    bool wake_positive = false;  // sign of the last wake input (7.5); the first one is positive

    // Opt-in diagnostics (MAG_GAME_CONTROLS_TRACE=1): about once a second, how far mouse look got
    // through this adapter, so a camera that does not turn can be traced to the step that stopped it.
    struct LookTrace {
        bool on = [] { const char* v = std::getenv("MAG_GAME_CONTROLS_TRACE"); return v && *v && std::strcmp(v, "0"); }();
        std::chrono::steady_clock::time_point next{};
        unsigned routes{}, eligible{}, look_held{}, held_by_menu{}, mouse_eligible{}, motion_frames{}, prepares{},
            prepare_motion{}, latched{}, consumer_seams{}, consumed{}, capture_on{}, capture_off{}, stick{}, seated{};
        double dx{}, dy{};
    } look_trace;
    void trace_look() {
        auto& t = look_trace;
        if (!t.on) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < t.next) return;
        if (t.routes)
            std::fprintf(stderr,
                         "[game-controls] trace look: routes=%u gameplay=%u look_held=%u held_by_menu=%u mouse_eligible=%u "
                         "motion_frames=%u (dx %.0f dy %.0f) prepared=%u with_motion=%u latched=%u consumer_seams=%u consumed=%u "
                         "mouse lock requests on/off=%u/%u stick_look=%u seated=%u\n",
                         t.routes, t.eligible, t.look_held, t.held_by_menu, t.mouse_eligible, t.motion_frames, t.dx, t.dy,
                         t.prepares, t.prepare_motion, t.latched, t.consumer_seams, t.consumed, t.capture_on, t.capture_off, t.stick,
                         t.seated);
        const bool on = t.on;
        t = LookTrace{};
        t.on = on;
        t.next = now + std::chrono::seconds(1);
    }
    std::map<uint32_t, AssistEntry> assist;
    std::vector<std::optional<AssistScope>> cleanup_scopes;
    Lifecycle lifecycle;
    ControllerFeed controller;

    State(MagGameControlsMemory memory, uint64_t session, const ControlsProfile& profile, void (*capture)(bool),
          void (*mouse_capture)(bool))
        : guest(memory), session(session), capture(capture), mouse_capture(mouse_capture), profile(profile), latch(session) {}

    // ---- Shared helpers ----------------------------------------------------------------------

    bool frame_valid() const {
        return frame && sample_key && mag_controls_frame_valid(*sample_key, frame->capture_generation);
    }
    bool mouse_motion() const { return frame && (frame->mouse_dx != 0 || frame->mouse_dy != 0); }
    bool owner_handler(uint32_t handler) const { return owner && owner->handler == handler; }
    bool removal_known_clear(uint32_t handler) const { return guest.removal_pending(handler) == std::optional<bool>(false); }
    bool registered_once(uint32_t handler) const { return guest.registration_count(handler) == std::optional<unsigned>(1); }
    void clear_receipts() {
        gameplay_receipts.clear();
        system_receipts.clear();
    }
    void latch_cancel() {
        latch.cancel();
        latch_identity.reset();
    }
    void register_shared_native_actions() {
        std::vector<ActionId> shared;
        for (const auto& action : mag_game_controls_actions())
            if (action.system_channel >= 0) shared.push_back(action.id);
        mag_controls_set_shared_native_actions(shared);
    }

    // The 16 bytes at a control record still hold what the adapter wrote, under the same token,
    // binding and tuning.
    bool control_unchanged(uint32_t record, uint32_t token, uint32_t binding, uint32_t tuning, const MagGuestTuple& tuple) const {
        auto r = guest.reads();
        MagGuestTuple current;
        return r.u32(record) == std::optional<uint32_t>(token) && r.u32(uint64_t(record) + RecordBinding) == std::optional<uint32_t>(binding) &&
               r.u32(uint64_t(record) + RecordTuning) == std::optional<uint32_t>(tuning) &&
               r.bytes(uint64_t(record) + RecordTuple, current.data(), current.size()) && current == tuple;
    }
    bool write_neutral(uint32_t record) const {
        const auto neutral = mag_guest_neutral_tuple();
        return guest.reads().write_bytes(uint64_t(record) + RecordTuple, neutral.data(), neutral.size());
    }

    // 6.4: give back every gameplay control the adapter still holds, then forget them all.
    void reset_host_caches() {
        const auto caches = std::exchange(host_caches, {});
        for (const auto& [record, cache] : caches)
            if (guest.owner_matches(cache.owner) && control_unchanged(record, cache.token, cache.binding, cache.tuning, cache.tuple))
                write_neutral(record);
    }
    // 6.5: the same for system buttons, optionally for one handler or without writing.
    void reset_system_caches(std::optional<uint32_t> only = {}, bool discard = false) {
        for (auto it = system_caches.begin(); it != system_caches.end();) {
            const auto& [record, cache] = *it;
            if (only && cache.handler != *only) {
                ++it;
                continue;
            }
            if (!discard && system_control_unchanged(record, cache)) write_neutral(record);
            it = system_caches.erase(it);
        }
    }
    bool system_control_unchanged(uint32_t record, const SystemCache& cache) const {
        if (!guest.base_update(cache.handler)) return false;
        const auto controls = guest.watched_controls(cache.handler);
        if (!controls) return false;
        const bool listed = std::any_of(controls->begin(), controls->end(), [&](const MagGuestControl& c) {
            return c.record == record && c.token == cache.token && c.binding == cache.binding;
        });
        return listed && control_unchanged(record, cache.token, cache.binding, cache.tuning, cache.tuple);
    }

    // 6.2.
    void cancel(const char* reason) {
        status = reason;
        reset_system_caches();
        reset_host_caches();
        last_context.reset();
        latch_cancel();
        actor_owner.reset();
        handler_owner.reset();
        frame.reset();
        contract.reset();
        gameplay_eligible = mouse_eligible = stick_look = false;
        clear_receipts();
        // Releasing engine input discards the sealed sample; it cannot be attached in this poll.
        sealed = false;
        mag_controls_set_supported_context(false);
        mag_controls_release_input(InputResetReason::Context);
        if (mouse_capture) mouse_capture(false);
        if (capture) capture(false);
    }
    // Cancel from an error path; never throws, and always records the reason.
    void cancel_quietly(const char* reason) noexcept {
        try {
            cancel(reason);
        } catch (...) {
            status = reason;
        }
    }

    // 6.3.
    void retire() {
        if (sample_key && !frame_valid()) {
            reset_host_caches();
            clear_receipts();
        }
        latch_cancel();
        stick_look = false;
        frame.reset();
        contract.reset();
        if (sample_key) mag_controls_retire_frame(*sample_key);
        sample_key.reset();
        sealed = false;
    }

    // Section 7: a changed capture generation means the engine let go of held input.
    void check_capture_generation() {
        const uint64_t generation = mag_controls_current_generation();
        if (generation == seen_generation) return;
        seen_generation = generation;
        reset_system_caches();
        reset_host_caches();
        clear_receipts();
        latch_cancel();
    }

    // ---- Handler lifecycle (6.6) -------------------------------------------------------------

    bool tracked(uint32_t handler) const { return owner_handler(handler) || lifecycle.is_tombstoned(handler); }

    void revoke(uint32_t handler, bool reconstruction) {
        reset_system_caches(handler, reconstruction);
        mag_controls_forget_consumer(handler);
        std::erase_if(entries, [&](const auto& entry) { return entry.second.handler == handler; });
        if (recognition_handler == handler) recognition_handler = 0;
        if (!tracked(handler) || lifecycle.exhausted) return;
        if (owner_handler(handler)) {
            // A reconstructed handler's old control records may already be gone: never write them.
            if (reconstruction) host_caches.clear();
            cancel(reconstruction ? "owned input handler reconstructed" : "owned input handler removed");
        } else {
            std::erase_if(host_caches, [&](const auto& cache) { return cache.second.handler == handler; });
        }
        lifecycle.tombstoned.insert(handler);
        lifecycle.additions.erase(handler);
        if (!lifecycle.advance(handler)) {
            lifecycle.exhausted = true;
            cancel("input lifecycle capacity exhausted");
            return;
        }
        if (reconstruction) lifecycle.needs_init.insert(handler);
    }

    void lifecycle_event(Hook event, const MagGameControlsCpu& cpu) {
        const auto r3 = address(cpu.gpr[3]), r4 = address(cpu.gpr[4]), r5 = address(cpu.gpr[5]);
        switch (event) {
        case Hook::HandlerRemove: if (mag_guest_pointer_like(r4)) revoke(r4, false); break;
        case Hook::HandlerDestroy: if (mag_guest_pointer_like(r3)) revoke(r3, false); break;
        case Hook::HandlerConstruct: construct_enter(r3, r4, r5); break;
        case Hook::HandlerConstructed: construct_exit(); break;
        case Hook::HandlerAdd: add_enter(r3, r4); break;
        case Hook::HandlerAdded: add_exit(r3); break;
        case Hook::HandlerRefresh: refresh_enter(r3); break;
        case Hook::HandlerRefreshed: refresh_exit(); break;
        default: break;
        }
    }
    void construct_enter(uint32_t handler, uint32_t priority, uint32_t slot) {
        if (mag_guest_pointer_like(handler) && tracked(handler)) revoke(handler, true);
        Lifecycle::push(lifecycle.constructions,
                        {handler, priority, slot, lifecycle.lifetime(handler), tracked(handler)});
    }
    void construct_exit() {
        const auto scope = Lifecycle::pop(lifecycle.constructions);
        if (scope && scope->tracked && lifecycle.lifetime(scope->handler) == scope->lifetime &&
            guest.freshly_constructed(scope->handler, scope->priority, scope->slot))
            lifecycle.needs_init.erase(scope->handler);
    }
    void add_enter(uint32_t manager, uint32_t handler) {
        const uint64_t lifetime = lifecycle.lifetime(handler);
        Lifecycle::push(lifecycle.adds, {handler, manager, lifetime});
        if (!lifecycle.exhausted && lifecycle.is_tombstoned(handler) && !lifecycle.needs_init.count(handler) &&
            manager == InputManager && guest.registration_count(handler) == std::optional<unsigned>(0) &&
            removal_known_clear(handler))
            lifecycle.additions[handler] = {lifetime, false};
    }
    void add_exit(uint32_t added) {
        const auto scope = Lifecycle::pop(lifecycle.adds);
        if (!scope) return;
        const auto it = lifecycle.additions.find(scope->handler);
        if (it == lifecycle.additions.end() || it->second.lifetime != scope->lifetime) return;
        if (added == scope->handler && registered_once(scope->handler) && removal_known_clear(scope->handler))
            it->second.ready = true;
        else
            lifecycle.additions.erase(it);
    }
    void refresh_enter(uint32_t handler) {
        const auto it = lifecycle.additions.find(handler);
        const bool found = it != lifecycle.additions.end();
        if (owner_handler(handler) && !lifecycle.is_tombstoned(handler) && !(found && it->second.ready)) cancel(MapChanged);
        Lifecycle::push(lifecycle.refreshes,
                        {handler, found ? std::optional<Lifecycle::Pending>(it->second) : std::nullopt});
    }
    void refresh_exit() {
        const auto scope = Lifecycle::pop(lifecycle.refreshes);
        if (!scope) return;
        const uint32_t handler = scope->handler;
        const auto it = lifecycle.additions.find(handler);
        if (scope->pending && scope->pending->ready && it != lifecycle.additions.end() && it->second.ready &&
            it->second.lifetime == scope->pending->lifetime && registered_once(handler) && removal_known_clear(handler))
            lifecycle.tombstoned.erase(handler);
        lifecycle.additions.erase(handler);
    }

    // ---- Update tick and poll (7.1, 8.2, 8.3) ------------------------------------------------

    void install_pending_profile() {
        std::optional<ControlsProfile> next;
        {
            std::lock_guard lock(pending_mutex);
            next.swap(pending);
        }
        if (!next) return;
        retire();
        cancel("controls profile changed; release held controls");
        forget_recognition_consumers();
        entries.clear();
        profile = std::move(*next);
        mag_controls_configure(profile);
        register_shared_native_actions();
        controller = {};
    }
    void forget_recognition_consumers() {
        std::set<uint32_t> consumers(system_routes.begin(), system_routes.end());
        for (const auto& [record, entry] : entries) consumers.insert(entry.handler);
        for (const auto& [record, cache] : system_caches) consumers.insert(cache.handler);
        if (gameplay_routes) consumers.insert(gameplay_routes->begin(), gameplay_routes->end());
        if (owner) consumers.insert(owner->handler);
        for (uint32_t consumer : consumers) mag_controls_forget_consumer(consumer);
    }

    void outer_enter() {
        if (outer++) return;
        install_pending_profile();
        check_capture_generation();
        ++tick;
        retire();
        status = "waiting for input poll";
    }
    void outer_exit() {
        if (!outer || --outer) return;
        abort_capture_poll();
        retire();
        handler_owner.reset();
        actor_owner.reset();
    }
    void abort_capture_poll() {
        if (!capture_poll_open) return;
        capture_poll_open = false;
        mag_input_capture_poll_abort(poll_number);
    }

    void poll_enter() {
        if (++poll != 1) return;
        update_keyboard_ownership();
        retire();
        poll_number = ++poll_counter;
        poll_sealed = false;
        seat_consumed = 0;
        if (!mag_input_capture_poll_begin(poll_number)) throw std::runtime_error("capture layer refused the input poll");
        capture_poll_open = true;
    }
    // 8.2: the replacement owns the keyboard only while the local handler is validly registered,
    // and mutes the legacy keyboard pad only while this poll would attach on foot: in an in-match
    // menu it routes the system buttons alone, so the pad keeps the menu's other buttons.
    void update_keyboard_ownership() {
        bool owned = false, on_foot = false;
        try {
            if (const auto local = guest.local_owner()) {
                owned = !lifecycle.exhausted && !lifecycle.is_tombstoned(local->handler) &&
                        removal_known_clear(local->handler) && guest.system_routes().has_value();
                on_foot = owned && local->enabled && !local->pre_spawn && guest.gameplay_routes().has_value() &&
                          build_contract(*local);
            }
        } catch (...) {
            mag_input_capture_set_keyboard_pad_live(0);
            mag_input_capture_set_keyboard_owner(0);
            throw;
        }
        // A menu that holds look on foot (the in-match pause menu, the loadout screen) is served
        // by the pad as well.
        {
            static int last = -1;
            const int live = owned && (!on_foot || look_held_by_menu);
            if (live != last) { last = live; std::fprintf(stderr, "[lab-rx] t=%lld pad_live=%d owned=%d on_foot=%d look_held_by_menu=%d\n", lab_ms(), live, int(owned), int(on_foot), int(look_held_by_menu)); }
        }
        mag_input_capture_set_keyboard_pad_live(owned && (!on_foot || look_held_by_menu));
        mag_input_capture_set_keyboard_owner(owned);
    }
    void sample_seal() {
        if (!poll || poll_sealed) return;
        poll_sealed = true;
        mag_controls_clear_controller_sample();
        MagInputControllerSample sample{};
        const bool complete = capture_poll_open && mag_input_capture_poll_complete(poll_number, &sample);
        capture_poll_open = false;
        if (complete) feed_controller(sample);
        previous_seal_ns = seal_ns;
        seal_ns = now_ns();
        const InputFrameKey key{session, poll_number, 0};
        mag_controls_seal_sample(key);
        sample_key = key;
        sealed = true;
    }
    // 8.3. An incomplete sample (not called) neither releases nor connects.
    void feed_controller(const MagInputControllerSample& sample) {
        mag_controls_set_controller_connection(sample.connected != 0);
        if (!sample.connected) {
            if (controller.seen) mag_controls_release_source(InputSource::Controller);
            controller = {};
            return;
        }
        mag_controls_capture_controller_axes({sample.move_x, sample.move_y, sample.look_x, sample.look_y,
                                              sample.left_trigger, sample.right_trigger});
        const auto held = controller.current(sample);
        const uint64_t time = now_ns();
        if (!controller.seen || mag_controls_current_generation() != controller.generation) {
            // A new device lifetime: release what was tracked and require every held code to be
            // released before it counts.
            for (unsigned code = 0; code < held.size(); ++code)
                if (controller.held[code]) push_controller(code, false, time);
            controller.blocked = held;
        } else {
            const auto changed = held ^ controller.held;
            for (unsigned code = 0; code < held.size(); ++code) {
                if (!changed[code]) continue;
                if (held[code] && controller.blocked[code]) continue;
                if (!held[code]) controller.blocked[code] = false;
                push_controller(code, held[code], time);
            }
        }
        controller.held = held;
        controller.seen = true;
        controller.generation = mag_controls_current_generation();
    }
    void poll_exit() {
        if (!poll) return;
        if (poll == 1) {
            observe(system_receipts);
            spend_unread_presses();
        }
        if (--poll) return;
        abort_capture_poll();
        if (sealed && !frame) cancel("no supported local gameplay handler");
    }
    // 7.3, end of the poll: the engine offers a press until it is observed, so a press that no
    // handler read in its poll would wait and act much later, wherever an action's control is next
    // read (D-pad Left on foot, which nothing watches there, replayed on the respawn bar; X on foot,
    // bleeding out when the player goes down). On foot every press no recognition kept as a receipt
    // is spent here; a press still held stays held. Gameplay receipts are completed at the end of the
    // actor update (7.5). A menu screen starts reading some buttons only after it appears (the respawn
    // bar reads the D-pad about 1.5 s after the awaiting-spawn state begins), so a press made on a menu
    // screen waits for its reader, for at most MenuPressWaitNs.
    static constexpr uint64_t MenuPressWaitNs = 3'000'000'000;
    void spend_unread_presses() {
        if (!frame_valid()) return;
        const bool menu = !gameplay_eligible;
        const uint64_t now = now_ns();
        std::map<ActionId, MenuUnread> waiting;
        for (const auto& action : mag_game_controls_actions()) {
            if (gameplay_receipts.count(action.id)) continue;
            const auto offer = mag_controls_offer(*sample_key, action.id);
            if (!offer) continue;
            if (menu) {
                const auto it = menu_unread.find(action.id);
                const uint64_t since = it != menu_unread.end() && it->second.pulse_id == offer->pulse_id ? it->second.since_ns : now;
                if (now - since < MenuPressWaitNs) {
                    waiting.emplace(action.id, MenuUnread{offer->pulse_id, since});
                    continue;
                }
            }
            mag_controls_observe(*sample_key, action.id, *offer);
        }
        menu_unread = std::move(waiting);
    }
    // 7.3, leaving a menu screen for gameplay: a press made there that no menu handler read is
    // spent before the first on-foot handler update, so it never acts on foot. Only those presses:
    // one made after the last menu poll is a press on foot.
    void spend_menu_presses() {
        if (!frame_valid()) return;
        for (const auto& [action, unread] : menu_unread)
            if (const auto offer = mag_controls_offer(*sample_key, action); offer && offer->pulse_id == unread.pulse_id)
                mag_controls_observe(*sample_key, action, *offer);
        menu_unread.clear();
    }
    void observe(std::map<ActionId, ControlOffer>& receipts) {
        if (frame_valid())
            for (const auto& [action, offer] : receipts) mag_controls_observe(*sample_key, action, offer);
        receipts.clear();
    }

    // ---- Handler update and attachment (7.2, 7.4) --------------------------------------------

    // Diagnostics only: describe the handler being updated, once per distinct description.
    void census(uint32_t handler) noexcept {
        if (!census_sink || census_seen.size() >= 2048) return;
        try {
            auto r = guest.reads();
            const char* role = owner_handler(handler) ? "owner"
                : gameplay_routes && contains(*gameplay_routes, handler) ? "gameplay-route"
                : seat_handlers.count(handler) ? "seat"
                : drive_handlers.count(handler) ? "drive"
                : contains(registered_routes, handler) ? "registered"
                : contains(system_routes, handler) ? "system-route" : "other";
            char head[160];
            std::snprintf(head, sizeof head, "census role=%s context=%s enabled=%d state=%u/%08X controls=", role,
                          !frame ? "none" : gameplay_eligible ? "OnFoot" : "Menu",
                          int(r.u8(uint64_t(handler) + 0x40).value_or(0xFF)), owner ? unsigned(owner->state_id) : 0u,
                          owner ? r.u32(owner->state).value_or(0) : 0u);
            std::string line = head;
            if (const auto controls = guest.watched_controls(handler)) {
                for (const auto& control : *controls) {
                    char item[24];
                    std::snprintf(item, sizeof item, "%08X:%u,", control.token, control.channel);
                    line += item;
                }
            } else {
                line += "unreadable";
            }
            if (census_seen.insert(line).second) census_sink(line.c_str());
        } catch (...) {
        }
    }

    void handler_enter(MagGameControlsCpu& cpu) {
        if (++handler_depth != 1) return;
        update_gameplay_available.reset();
        update_handler(cpu);
        census(address(cpu.gpr[3]));  // as the update leaves it
    }
    void update_handler(MagGameControlsCpu& cpu) {
        recognition_handler = 0;
        seat_scope.reset();
        if (!sample_key || !sealed) return;
        std::vector<uint32_t> registered;
        auto routes = guest.system_routes(&registered);
        if (!routes) {
            cancel("native system input routing contract unavailable");
            return;
        }
        const auto gameplay = guest.gameplay_routes();
        update_gameplay_available = gameplay.has_value();
        if (!frame && !attach(gameplay)) return;
        system_routes = std::move(*routes);
        std::erase_if(registered, [&](uint32_t handler) { return owner_handler(handler); });
        registered_routes = std::move(registered);
        if (!gameplay) revoke_gameplay();
        find_seat_handlers();
        const uint32_t handler = address(cpu.gpr[3]), pad = address(cpu.gpr[4]);
        // 7.2 step 5: remember what a seat handler's own update consumes.
        if (seat_handlers.count(handler))
            if (const auto before = guest.reads().u32(uint64_t(pad) + PadConsumed)) seat_scope = SeatScope{pad, *before};
        route(handler, pad);
    }
    // Once per poll: the registered handlers that watch every look channel of the player's contract.
    void find_seat_handlers() {
        if (seat_poll == poll_number) return;
        seat_poll = poll_number;
        seat_handlers.clear();
        drive_handlers.clear();
        // Seats are looked for only outside the soldier's aim states (2.2): on foot the soldier aims.
        if (!contract || !contract->look_mask || !owner || owner->mouse_supported) return;
        // The channels of the driving buttons, as the player's handler takes them (7.4).
        uint32_t drive_mask = 0;
        unsigned drive_found = 0;
        for (const auto& [channel, action] : channel_actions)
            if (std::find(std::begin(DriveActions), std::end(DriveActions), action) != std::end(DriveActions)) {
                drive_mask |= 1u << channel;
                ++drive_found;
            }
        for (uint32_t handler : registered_routes) {
            const auto controls = guest.watched_controls(handler);
            if (!controls) continue;
            uint32_t channels = 0;
            for (const auto& control : *controls) channels |= 1u << control.channel;
            if ((channels & contract->look_mask) == contract->look_mask) seat_handlers.insert(handler);
            if (drive_found == std::size(DriveActions) && (channels & drive_mask) == drive_mask)
                drive_handlers.insert(handler);
        }
    }
    bool seated() const { return !seat_handlers.empty(); }
    void handler_exit() {
        if (!handler_depth || --handler_depth) return;
        if (seat_scope) {
            if (const auto after = guest.reads().u32(uint64_t(seat_scope->pad) + PadConsumed))
                seat_consumed |= *after & ~seat_scope->before;
            seat_scope.reset();
        }
        update_gameplay_available.reset();
        handler_owner.reset();
        recognition_handler = 0;
    }
    void revoke_gameplay() {
        gameplay_eligible = mouse_eligible = stick_look = false;
        gameplay_routes.reset();
        reset_host_caches();
        gameplay_receipts.clear();
        latch_cancel();
    }

    std::optional<Contract> build_contract(const MagGuestOwner& local) const {
        const auto controls = guest.watched_controls(local.handler);
        if (!controls) return {};
        std::set<ActionId> needed;
        for (const auto& binding : profile.bindings)
            if ((binding.contexts & uint32_t(InputContext::OnFoot)) && !system_action(binding.action) &&
                !optional_action(binding.action) &&
                (binding.control.source != InputSource::Controller || profile.controller_remap))
                needed.insert(binding.action);
        std::set<uint32_t> tokens;
        std::set<ActionId> bound;
        for (const auto& binding : profile.bindings) bound.insert(binding.action);
        Contract result{local.handler, *controls, 0, 0};
        unsigned looks = 0;
        for (const auto& control : *controls) {
            if (!tokens.insert(control.token).second) return {};
            if (const unsigned bit = mag_game_controls_look_bit(control.token)) {
                looks |= bit;
                result.look_mask |= 1u << control.channel;
                result.mask |= 1u << control.channel;
                continue;
            }
            const ActionId action = mag_game_controls_token_action(control.token);
            if (mag_game_controls_action(action) || bound.count(action)) {
                needed.erase(action);
                result.mask |= 1u << control.channel;
            }
        }
        if (looks != 0xF || !needed.empty() || !result.mask) return {};
        return result;
    }
    // 7.4: the owner's watched list is unchanged and gameplay is still allowed (during a handler
    // update, as read at the start of that update).
    bool contract_current() const {
        if (!contract || !owner) return false;
        const auto controls = guest.watched_controls(contract->handler);
        if (!controls || *controls != contract->controls) return false;
        return update_gameplay_available ? *update_gameplay_available : guest.gameplay_routes().has_value();
    }
    bool map_channels() {
        const auto add = [&](uint32_t channel, ActionId action) {
            const auto [it, inserted] = channel_actions.emplace(channel, action);
            return inserted || it->second == action;
        };
        if (contract)
            for (const auto& control : contract->controls) {
                const bool look = mag_game_controls_look_token(control.token);
                const ActionId action = look ? control.token : mag_game_controls_token_action(control.token);
                if ((look || (mag_game_controls_action(action) && !optional_action(action))) && !add(control.channel, action))
                    return false;
            }
        for (const auto& action : mag_game_controls_actions())
            if (action.system_channel >= 0 && !add(uint32_t(action.system_channel), action.id)) return false;
        return true;
    }

    // 7.5, mouse as the right stick: outside the soldier's aim states the mouse pushes the stick as
    // far as it moves fast. The mouse becomes the look device at once, so the first poll counts.
    void update_stick_look(const MagGuestOwner& local) {
        stick_look = gameplay_eligible && !local.mouse_supported && !look_held_by_menu && profile.mode != InputMode::Controller;
        stick_x = stick_y = 0;
        if (!stick_look || !mouse_motion()) return;
        const double interval = previous_seal_ns && seal_ns > previous_seal_ns
                                    ? std::clamp(double(seal_ns - previous_seal_ns) / 1e9, MinStickInterval, MaxStickInterval)
                                    : FirstStickInterval;
        const auto look = mag_controls_mouse_look(frame->mouse_dx, frame->mouse_dy, profile.mouse, AimMode::Hip);
        stick_x = std::clamp(look.yaw / interval / FullStickRate, -1.0, 1.0);
        stick_y = std::clamp(look.pitch / interval / FullStickRate, -1.0, 1.0);
        mag_controls_accept_mouse(*sample_key);
        frame->policy = mag_controls_current_policy();
    }
    // The deflection a look direction reads: turnleft, turnright, aimup, aimdown (look_tokens order).
    double stick_value(uint32_t look) const {
        switch (mag_game_controls_look_bit(look)) {
        case 1: return std::max(0.0, -stick_x);
        case 2: return std::max(0.0, stick_x);
        case 4: return std::max(0.0, stick_y);
        case 8: return std::max(0.0, -stick_y);
        default: return 0.0;
        }
    }

    // 7.2 step 3: attach the sealed sample for the local owner. False stops the handler update.
    bool attach(const std::optional<std::vector<uint32_t>>& gameplay) {
        const auto local = guest.local_owner();
        if (!local) return false;
        if (lifecycle.exhausted || lifecycle.is_tombstoned(local->handler) || !removal_known_clear(local->handler)) {
            owner = local;
            cancel("input handler awaiting valid registration");
            return false;
        }
        auto built = build_contract(*local);
        if (owner && !mag_guest_same_owner(*owner, *local)) {
            cancel("local input owner changed");
            owner = local;
            return false;
        }
        owner = local;
        gameplay_routes = gameplay;
        contract = std::move(built);
        // Before the first spawn the pre-battle lobby and the first deploy screen are menus: the
        // keyboard pad serves them as it serves every other menu (X Cross, Z Square, Q/E L1/R1...).
        gameplay_eligible = gameplay && contract && local->enabled && !local->pre_spawn;
        channel_actions.clear();
        clear_receipts();
        mouse_eligible = false;
        if (!map_channels()) {
            cancel("ambiguous native action channel");
            return false;
        }
        mag_controls_set_supported_context(true);
        if (capture && profile.mode != InputMode::Controller) capture(true);
        const auto context = gameplay_eligible ? InputContext::OnFoot : InputContext::Menu;
        const auto neutral = guest.neutral_look(local->handler);
        const uint64_t before = mag_controls_current_generation();
        frame = mag_controls_attach_sample(*sample_key, InputInterpretation{context, neutral});
        // A context change is not a device reset: keep controller tracking across it.
        if (last_context && *last_context != context && controller.generation == before)
            controller.generation = frame->capture_generation;
        last_context = context;
        seen_generation = frame->capture_generation;
        update_stick_look(*local);
        if (!gameplay_eligible) {
            reset_host_caches();
            gameplay_receipts.clear();
        } else if (!menu_unread.empty()) {
            spend_menu_presses();
        }
        // The same request as each handler update makes (route): attaching happens every poll, and
        // asking for the lock here while a menu holds look would take it back every poll for the
        // handler update to release again, so the lock flapped for as long as the menu stayed.
        if (mouse_capture) mouse_capture(gameplay_eligible && !look_held_by_menu && profile.mode != InputMode::Controller);
        {
            static std::string last;
            char b[200];
            std::snprintf(b, sizeof b, "state=%u/%08X downed=%d pre_spawn=%d ctx=%s gameplay=%d contract=%d enabled=%d", unsigned(local->state_id),
                          guest.reads().u32(local->state).value_or(0), int(local->downed), int(local->pre_spawn),
                          gameplay_eligible ? "OnFoot" : "Menu", int(gameplay.has_value()), int(contract.has_value()), int(local->enabled));
            if (last != b) { last = b; std::fprintf(stderr, "[lab-rx] t=%lld attach %s mode=fix\n", lab_ms(), b); }
        }
        attached_tick = tick;
        status = gameplay_eligible ? "on-foot Rust input attached" : "native menu buttons attached";
        return true;
    }

    // 7.2 step 5: make the handler a recognition consumer and, for the owner's handler, refresh
    // mouse eligibility from the pad record.
    void route(uint32_t handler, uint32_t pad) {
        if (!owner || !frame_valid()) return;
        if (contains(system_routes, handler)) recognition_handler = handler;
        if (!owner_handler(handler)) return;
        const auto current = guest.resolve_owner(handler, true);
        if (!current || !mag_guest_same_owner(*current, *owner)) {
            cancel("native input owner changed");
            return;
        }
        owner = current;
        if (gameplay_eligible) handler_owner = current;
        const auto consumed = guest.reads().u32(uint64_t(pad) + PadConsumed);
        if (!consumed) throw std::runtime_error("pad record unreadable");
        // A menu or other UI that takes the look channels before the player's handler holds the
        // mouse: release the lock (the cursor is free) until look comes back.
        // Look channels a seat handler consumed in its own update are the seat's, not a menu's.
        const bool look_held = contract && (*consumed & contract->look_mask & ~seat_consumed);
        mouse_eligible = gameplay_eligible && current->mouse_supported && contract && !look_held;
        if (look_held) stick_look = false;
        // Mouse look stops at once, but the lock follows the menu only once its hold on look has been
        // stable for a few handler updates: menus flicker the look channels while they animate, and
        // each flicker would otherwise release and re-take the lock.
        if (look_held == look_held_seen) {
            if (look_held_streak < MenuLookStableUpdates) ++look_held_streak;
        } else {
            look_held_seen = look_held;
            look_held_streak = 1;
        }
        if (look_held_seen != look_held_by_menu && look_held_streak >= MenuLookStableUpdates) {
            look_held_by_menu = look_held_seen;
            if (gameplay_eligible)
                status = look_held_by_menu ? "on-foot input attached; a menu holds look" : "on-foot Rust input attached";
        }
        const bool capture_mouse = gameplay_eligible && !look_held_by_menu && profile.mode != InputMode::Controller;
        if (mouse_capture) mouse_capture(capture_mouse);
        if (mouse_eligible && mouse_motion()) mag_controls_accept_mouse(*sample_key);
        if (look_trace.on) {
            auto& t = look_trace;
            ++t.routes; t.eligible += gameplay_eligible; t.look_held += look_held; t.held_by_menu += look_held_by_menu;
            t.mouse_eligible += mouse_eligible; t.stick += stick_look; t.seated += seated();
            (capture_mouse ? t.capture_on : t.capture_off)++;
            if (mouse_motion()) { ++t.motion_frames; t.dx += frame->mouse_dx; t.dy += frame->mouse_dy; }
            trace_look();
        }
        frame->policy = mag_controls_current_policy();
    }

    // ---- Recognition (7.3) -------------------------------------------------------------------

    Tuning read_tuning(uint32_t tuning) const {
        auto r = guest.reads();
        const uint64_t t = tuning;
        const auto curve = r.u32(t + 4);
        const auto low = r.f32(t + 8), high = r.f32(t + 0x0C), epsilon = r.f32(t + 0x10), threshold = r.f32(t + 0x14);
        const auto press = r.f32(t + 0x18), gap = r.f32(t + 0x1C), repeat = r.f32(t + 0x20), interval = r.f32(t + 0x24),
                   refocus = r.f32(t + 0x28);
        if (!curve || !low || !high || !epsilon || !threshold || !press || !gap || !repeat || !interval || !refocus)
            throw std::runtime_error("tuning record unreadable");
        if (!(*low < *high) || !(*threshold >= 0.0f && *threshold <= 1.0f) || !(*epsilon >= 0.0f))
            throw std::runtime_error("invalid tuning record");
        Tuning result{*curve, *low, *high, *epsilon, *threshold, {}};
        result.config = {duration_ns(*press), duration_ns(*gap), duration_ns(*repeat), duration_ns(*interval),
                         duration_ns(*refocus), double(*threshold), double(*epsilon)};
        return result;
    }
    // The game's own reading of the raw channel value, as a recognizer input in [0, 1].
    static double native_value(const Tuning& tuning, double raw) {
        const float normalized = std::clamp((float(raw) - tuning.low) / (tuning.high - tuning.low), 0.0f, 1.0f);
        return value_curve(tuning.curve, std::isnan(normalized) ? 0.0f : normalized);
    }

    bool recognize(MagGameControlsCpu& cpu) {
        // Step 1.
        const uint32_t handler = address(cpu.gpr[23]);
        if (!recognition_handler || recognition_handler != handler || handler_depth != 1 || !owner || !frame_valid())
            return false;
        if (!guest.owner_matches(*owner)) {
            cancel("native input routing changed");
            return false;
        }
        // Step 2.
        const uint32_t n = address(cpu.gpr[29]);
        if (!n || n > 0xFFFFFFE3 || cpu.gpr[31] != uint64_t(n) + 8 || cpu.gpr[3] != uint64_t(n) + 8) return false;
        auto r = guest.reads();
        const auto token = r.u32(n), binding = r.u32(uint64_t(n) + RecordBinding), tuning = r.u32(uint64_t(n) + RecordTuning);
        if (!token || !binding || !tuning || !*binding || *binding > 0xFFFFFFF3 || !*tuning || *tuning > 0xFFFFFFD7) return false;
        const auto channel = r.u32(uint64_t(*binding) + 8);
        if (!channel || *channel >= 32) return false;
        // A watched token that names a catalog action is that action wherever it is watched; any
        // other token takes the action of its channel.
        const ActionId named = mag_game_controls_token_action(*token);
        ActionId action = mag_game_controls_action(named) ? named : 0;
        if (!action) {
            const auto mapped = channel_actions.find(*channel);
            if (mapped == channel_actions.end()) return false;
            action = mapped->second;
        }

        // Step 3.
        const auto* catalog = mag_game_controls_action(action);
        const bool system = catalog && catalog->system_channel >= 0;
        const bool gameplay = owner_handler(handler);
        const bool registered = !gameplay && contains(registered_routes, handler);
        if (system) {
            if (!contains(system_routes, handler)) {
                cancel("native system route changed");
                return false;
            }
        } else {
            // Confirm and Back are menu buttons (2.3). Menus read them from the keyboard pad (8.2); on
            // foot the only handlers watching them are closed menus that stay routed (the pause menu's),
            // which must not see them: X on foot opened the player card in the hidden pause menu.
            if (catalog && catalog->menu_prompt) return false;
            if (!gameplay_eligible) return false;
            if (!contract_current()) {
                gameplay_eligible = mouse_eligible = false;
                return false;
            }
            // While a menu holds look on foot the pad serves every handler but the player's (8.2).
            if (!gameplay && look_held_by_menu) return false;
            // Situational actions may be watched by any system route (a vehicle's or a menu's own
            // handler); other actions only by the owner, a gameplay route or a handler registered
            // with the input manager (a mounted weapon's or a seat's, the pad-button reader), which
            // gets what the player's handler takes on that channel, as a controller's button
            // presses both.
            if (gameplay ? mag_game_controls_token_action(*token) != action
                         : !(gameplay_routes && contains(*gameplay_routes, handler)) && !registered &&
                               !(optional_action(action) && contains(system_routes, handler)))
                return false;
            // In a seat the right mouse button belongs to the seat (its R2, step 4): Aim's own
            // controls, the player's and any other handler's, are left to the game.
            if (seated() && action == mag_game_controls_seat_alias(SeatSecondary)) return false;
        }

        // Step 4. While the player is downed, the player's own Bleed out also reads Confirm's keys
        // (2.3). One action per press: its own keys first, then the alias's, otherwise the action the
        // record last used, so a press finishes on the action it began on.
        // A driver's handler reads Move forward's and Move backward's keys (W, S) on its triggers; a
        // seat handler's control on R2 also reads Aim's keys (the right mouse button).
        const ActionId alias = gameplay && owner->downed                           ? mag_game_controls_downed_alias(action)
                             : registered && drive_handlers.count(handler) && !system ? mag_game_controls_drive_alias(action)
                             : registered && seat_handlers.count(handler) && !system ? mag_game_controls_seat_alias(action)
                                                                                    : 0;
        if (alias)
            if (!mag_controls_offer(*sample_key, action)) {
                const auto prior = entries.find(n);
                if (mag_controls_offer(*sample_key, alias) || (prior != entries.end() && prior->second.action == alias))
                    action = alias;
            }
        const bool mouse_look = gameplay && mag_game_controls_look_token(action) && mouse_eligible &&
                                frame->policy.look_owner == LookOwner::Mouse;
        // 7.5: outside the soldier's aim states the mouse is the right stick.
        const bool stick = stick_look && !system && mag_game_controls_look_token(action) &&
                           frame->policy.look_owner == LookOwner::Mouse;
        const auto offer = mag_controls_offer(*sample_key, action);
        const bool kbm = profile.mode == InputMode::KBM;
        const bool remapped = profile.controller_remap && catalog && catalog->digital_controller;
        const bool deny_native = ((gameplay || system) && kbm) || remapped;
        auto existing = entries.find(n);
        if (!offer && existing == entries.end() && !mouse_look && !stick && !deny_native) return false;

        // Steps 5 to 7.
        const Tuning tune = read_tuning(*tuning);
        const bool preserve = !remapped && !kbm && frame->policy.controller_connected;
        double value = preserve && !mouse_look && !stick ? native_value(tune, cpu.fpr[1]) : 0.0;
        const bool native_contribution = preserve && value > 0;
        if (offer && offer->down && !mouse_look) value = 1.0;
        if (mouse_look) value = 0.0;
        if (stick) value = stick_value(action);

        // Step 8.
        MagGuestTuple current;
        if (!r.bytes(uint64_t(n) + RecordTuple, current.data(), current.size())) return false;
        RecognitionEntry& entry = renew_entry(n, handler, *token, *binding, *tuning, current, system, preserve, action, tune);

        // Step 9.
        const bool blocked = cpu.gpr[5] != 0 || entry.start_blocked;
        entry.start_blocked = false;
        const auto recognized = mag_controls_recognize(*sample_key, handler, entry.id, action, value, blocked, tune.config,
                                                       native_contribution);
        if (!recognized) {
            // Another thread withdrew the frame (a focus or mouse-capture change began a new capture
            // generation): the game recognizes this control, and the entry starts afresh next time.
            entries.erase(n);
            return false;
        }
        const auto& result = *recognized;

        // Step 10.
        const uint32_t bits = game_event_bits(result.events);
        if (gameplay && (*channel == 5 || *channel == 12) && (bits || value > 0))
            std::fprintf(stderr, "[lab-rx] t=%lld rec token=%08X ch=%u action=%08X value=%.2f offer=%d bits=%04X state=%u\n", lab_ms(), *token, *channel,
                         action, value, offer ? int(offer->down) + 1 : 0, bits, unsigned(owner->state_id));
        const auto tuple = mag_guest_tuple(float(result.value), float(double(result.elapsed_ns) / 1e9),
                                           uint32_t(result.phase), bits);
        if (!r.write_bytes(uint64_t(n) + RecordTuple, tuple.data(), tuple.size()))
            throw std::runtime_error("cannot publish the control tuple");
        entry.tuple = tuple;
        current_control = n;

        // Steps 11 and 12. A system button the game also reads natively is not the adapter's to give
        // back. On a gameplay route handler other than the owner's, the tuple is the only effect.
        const bool accept = offer && gameplay && gameplay_eligible && !blocked && result.value > 0;
        if (system) {
            if (native_contribution) system_caches.erase(n);
            else system_caches[n] = {handler, *token, *binding, *tuning, tuple};
            if (offer) system_receipts[action] = *offer;
        } else if (gameplay) {
            host_caches[n] = {*owner, handler, *token, *binding, *tuning, tuple};
            if (offer) gameplay_receipts[action] = *offer;
        } else if (optional_action(action) || registered) {
            // Given back like a system button when the adapter lets go (6.5). A press a registered
            // handler used is completed with the player's, at the end of the actor update (7.5),
            // so it never waits to act later on foot.
            system_caches[n] = {handler, *token, *binding, *tuning, tuple};
            if (registered && offer) gameplay_receipts[action] = *offer;
        }
        if (accept) mag_controls_accept_action(*sample_key, action, *offer);

        // Registers last, so an error above leaves the game's own recognition untouched.
        cpu.gpr[3] = bits;
        cpu.gpr[11] = 0;
        return true;
    }

    // 7.3 step 8: a fresh engine control for a record the adapter has not seen in this exact form.
    RecognitionEntry& renew_entry(uint32_t n, uint32_t handler, uint32_t token, uint32_t binding, uint32_t tuning,
                                  const MagGuestTuple& current, bool system, bool preserve, ActionId action,
                                  const Tuning& tune) {
        auto it = entries.find(n);
        if (it != entries.end() && it->second.handler == handler && it->second.token == token &&
            it->second.binding == binding && it->second.tuning == tuning && it->second.tuple == current &&
            it->second.action == action)
            return it->second;
        if (it == entries.end() && entries.size() >= MaxRecognitionEntries)
            throw std::runtime_error("recognition entry capacity exhausted");
        RecognitionEntry entry{handler, token, binding, tuning, current, ++next_control_id, false, action};
        const uint32_t phase = mag_guest_tuple_phase(current);
        const float held = mag_guest_tuple_value(current), seconds = mag_guest_tuple_seconds(current);
        // A system button the game still considers held must be released before it acts.
        entry.start_blocked = system && (phase == 6 || phase == 7) && held > 0.0f;
        // Adopt a hold the game already recognised from the controller, without new events.
        if (preserve && phase >= 2 && phase <= 5 && std::isfinite(held) && held > 0.0f && held <= 1.0f &&
            held >= tune.threshold && std::isfinite(seconds) && seconds >= 0.0f)
            mag_controls_seed_recognizer(*sample_key, handler, entry.id, action,
                                         {held, duration_ns(seconds), ControlPhase(phase), 0}, tune.config);
        return entries[n] = entry;
    }

    void control_callback(MagGameControlsCpu& cpu) {
        const uint32_t n = address(cpu.gpr[29]);
        if (!current_control || n != current_control || frame_valid()) return;
        if (!write_neutral(n)) throw std::runtime_error("cannot release the control tuple");
        host_caches.erase(n);
        cpu.gpr[0] = 0;
        current_control = 0;
    }

    // ---- Actor update and mouse look (7.5) ---------------------------------------------------

    void actor_enter(const MagGameControlsCpu& cpu) {
        if (++actor_depth != 1) return;
        if (!owner || address(cpu.gpr[3]) != owner->logic) return;
        if (!frame || attached_tick != tick) {
            cancel("local actor has no current input poll");
            return;
        }
        if (!gameplay_eligible) return;
        if (!contract_current()) {
            cancel(MapChanged);
            return;
        }
        const auto current = guest.resolve_owner(owner->handler, true);
        if (!current || !mag_guest_same_owner(*current, *owner)) {
            cancel("unsupported or changed local input context");
            return;
        }
        actor_owner = current;
    }
    void actor_exit() {
        if (!actor_depth || --actor_depth) return;
        if (actor_owner && frame_valid() && guest.owner_matches(*actor_owner)) observe(gameplay_receipts);
        gameplay_receipts.clear();
        latch_cancel();
        actor_owner.reset();
    }

    void prepare(const MagGameControlsCpu& cpu) {
        if (!actor_owner || actor_depth != 1 || !frame_valid() || address(cpu.gpr[30]) != actor_owner->logic) return;
        if (!contract_current()) {
            cancel(MapChanged);
            return;
        }
        const auto current = guest.resolve_owner(actor_owner->handler, true);
        if (!current || !mag_guest_same_owner(*current, *actor_owner) || current->state != address(cpu.gpr[31])) {
            cancel("local state changed before mouse preparation");
            return;
        }
        owner = actor_owner = current;
        if (!mouse_eligible || !current->mouse_supported) {
            latch_cancel();
            return;
        }
        if (mouse_motion()) mag_controls_accept_mouse(*sample_key);
        ++look_trace.prepares;
        if (mouse_motion()) ++look_trace.prepare_motion;
        const auto look = mag_controls_mouse_look(frame->mouse_dx, frame->mouse_dy, profile.mouse, AimMode::Hip);
        const MagMouseAimIdentity identity{session, poll_number, tick, frame->capture_generation, current->handle(),
                                           current->actor, current->logic, current->controller, current->state, current->aim};
        // Screen-right motion turns the game's yaw the other way.
        if (!latch.prepare(identity, -look.yaw, look.pitch, cpu.fpr[31], true, frame->policy.look_owner == LookOwner::Mouse)) {
            latch_identity.reset();
            return;
        }
        latch_identity = identity;
        ++look_trace.latched;
        // 7.5: the wake input keeps a soldier at rest running the look consumer while the mouse moves.
        float yaw_input = 0.0f;
        if (look.yaw != 0 || look.pitch != 0) {
            wake_positive = !wake_positive;
            yaw_input = wake_positive ? MouseWakeYawInput : -MouseWakeYawInput;
        }
        auto r = guest.reads();
        if (!r.write_f32(uint64_t(current->actor) + ActorYawInput, yaw_input) || !r.write_f32(uint64_t(current->actor) + ActorPitchInput, 0.0f))
            throw std::runtime_error("cannot write the stick look rates");
        status = "on-foot mouse prepared";
    }

    void yaw_rate_enter(const MagGameControlsCpu& cpu) {
        if (++yaw_rate != 1) return;
        const uint64_t caller = actor_owner && actor_owner->aim == MagMouseAimState::Sprint ? SprintYawCaller : BasicYawCaller;
        yaw_rate_observed = actor_owner && address(cpu.gpr[3]) == actor_owner->actor && cpu.lr == caller;
    }
    void sprint_pitch_rate_enter(const MagGameControlsCpu& cpu) {
        if (++sprint_pitch_rate != 1) return;
        sprint_pitch_observed = actor_owner && actor_owner->aim == MagMouseAimState::Sprint &&
                                address(cpu.gpr[3]) == actor_owner->actor && cpu.lr == SprintPitchCaller;
    }
    static void scope_exit(unsigned& depth, bool& observed) {
        if (depth && !--depth) observed = false;
    }
    bool modifier_ready() const { return latch_identity && actor_owner && guest.owner_matches(*actor_owner); }
    void yaw_damage(const MagGameControlsCpu& cpu) {
        if (yaw_rate_observed && modifier_ready())
            latch.observe_modifier(*latch_identity, MagMouseModifierSite::YawRateDamage, cpu.fpr[1]);
    }
    void sprint_pitch_damage(const MagGameControlsCpu& cpu) {
        if (sprint_pitch_observed && modifier_ready() && address(cpu.gpr[29]) == actor_owner->actor)
            latch.observe_modifier(*latch_identity, MagMouseModifierSite::SprintPitchDamage, cpu.fpr[1]);
    }

    // BasicYaw, BasicPitch, SprintYaw, SprintPitch: add the latched displacement once.
    void consume(Hook event, MagGameControlsCpu& cpu) {
        ++look_trace.consumer_seams;
        if (!latch_identity || !actor_owner || !frame_valid()) return;
        const auto current = guest.resolve_owner(actor_owner->handler, true);
        if (!current || *current != *actor_owner) return;
        if (!contract_current()) {
            cancel(MapChanged);
            return;
        }
        const auto site = event == Hook::BasicYaw ? MagMouseAimSite::BasicYaw
                        : event == Hook::BasicPitch ? MagMouseAimSite::BasicPitch
                        : event == Hook::SprintYaw ? MagMouseAimSite::SprintYaw : MagMouseAimSite::SprintPitch;
        const unsigned actor_register = event == Hook::BasicPitch ? 31 : 3;
        if (address(cpu.gpr[actor_register]) != actor_owner->actor) return;
        const auto delta = latch.consume(*latch_identity, site, event == Hook::BasicPitch ? cpu.fpr[1] : 1.0);
        if (!delta) return;
        if (*delta != 0) ++look_trace.consumed;
        if (event == Hook::BasicYaw) {
            auto r = guest.reads();
            const uint64_t request = uint64_t(actor_owner->actor) + ActorYawRequest;
            const auto yaw = r.f32(request);
            if (!yaw || !r.write_f32(request, float(single_sum(*yaw, *delta))))
                throw std::runtime_error("cannot update the yaw request");
        } else if (event == Hook::BasicPitch) {
            cpu.fpr[31] = single_sum(cpu.fpr[31], *delta);
        } else {
            cpu.fpr[1] = single_sum(cpu.fpr[1], *delta);
        }
    }

    // ---- Aim assist (7.6) --------------------------------------------------------------------

    bool associated(uint32_t camera, std::optional<uint32_t> record) const {
        if (!mag_guest_pointer_like(camera) || camera > 0xFFFFFDD3) return false;
        auto r = guest.reads();
        if (r.u32(camera) != std::optional<uint32_t>(CameraVtable) || !owner || !guest.owner_matches(*owner)) return false;
        const auto target = r.u32(uint64_t(camera) + CameraTarget);
        return target && *target == owner->actor && (!record || *record == camera + CameraAssistRecord);
    }
    bool owns_actor(uint32_t actor) {
        if (owner && guest.owner_matches(*owner)) return actor == owner->actor;
        const auto found = guest.owner_of_actor(actor);
        if (!found) return false;
        owner = found;
        return true;
    }
    AssistEntry& assist_entry(uint32_t camera, bool& created) {
        created = false;
        if (const auto it = assist.find(camera); it != assist.end()) return it->second;
        if (assist.size() >= MaxAssistEntries) {
            const auto victim = std::find_if(assist.begin(), assist.end(), [&](const auto& entry) {
                return std::none_of(cleanup_scopes.begin(), cleanup_scopes.end(),
                                    [&](const auto& scope) { return scope && scope->camera == entry.first; });
            });
            if (victim == assist.end()) throw std::runtime_error("aim assist tracking capacity exhausted");
            assist.erase(victim);
        }
        created = true;
        return assist[camera];
    }
    // Whether assist stays off for this camera; remembers a denial until a clean cleanup.
    bool denied(uint32_t camera, std::optional<uint32_t> record) {
        if (!associated(camera, record)) return false;
        bool created;
        AssistEntry& entry = assist_entry(camera, created);
        const uint32_t own_record = camera + CameraAssistRecord, handle = owner->handle();
        const auto policy = mag_controls_current_policy();
        if (created || entry.record != own_record || entry.actor_handle != handle)
            entry = {own_record, handle, policy.kbm_used_this_process};
        if (!mag_controls_assist_allowed(policy)) entry.dirty = true;
        return entry.dirty;
    }
    bool assist_early_result(Hook event, MagGameControlsCpu& cpu) {
        const uint32_t record = address(cpu.gpr[3]), actor = address(cpu.gpr[4]);
        if (!owns_actor(actor) || record <= CameraAssistRecord || !denied(record - CameraAssistRecord, record)) return false;
        if (event == Hook::AssistAdhesion) {
            auto r = guest.reads();
            if (!r.write_f32(address(cpu.gpr[6]), 0.0f) || !r.write_f32(address(cpu.gpr[7]), 0.0f))
                throw std::runtime_error("cannot clear assist adhesion outputs");
        }
        cpu.gpr[3] = 0;
        return true;
    }
    void assist_cleanup_enter(const MagGameControlsCpu& cpu) {
        if (cleanup_scopes.size() >= MaxAssistScopes) throw std::runtime_error("aim assist cleanup scope too deep");
        const uint32_t record = address(cpu.gpr[3]), camera = address(cpu.gpr[4]);
        std::optional<AssistScope> scope;
        if (mag_guest_pointer_like(camera) && camera <= 0xFFFFFDD3) {
            const auto target = guest.reads().u32(uint64_t(camera) + CameraTarget);
            if (target && owns_actor(*target) && associated(camera, record)) {
                denied(camera, record);
                scope = AssistScope{camera, record};
            }
        }
        cleanup_scopes.push_back(scope);
    }
    void assist_cleanup_exit() {
        if (cleanup_scopes.empty()) return;
        const auto scope = cleanup_scopes.back();
        cleanup_scopes.pop_back();
        if (!scope || !associated(scope->camera, scope->record) || !guest.clean_assist_record(scope->record)) return;
        if (const auto it = assist.find(scope->camera); it != assist.end()) it->second.dirty = false;
    }
    void assist_friction(MagGameControlsCpu& cpu) {
        if (denied(address(cpu.gpr[3]), std::nullopt)) cpu.fpr[1] = 1.0;
    }

    // ---- Dispatch ----------------------------------------------------------------------------

    static bool lifecycle_hook(Hook event) {
        switch (event) {
        case Hook::HandlerRemove: case Hook::HandlerRemoveExit: case Hook::HandlerDestroy: case Hook::HandlerDestroyed:
        case Hook::HandlerConstruct: case Hook::HandlerConstructed: case Hook::HandlerAdd: case Hook::HandlerAdded:
        case Hook::HandlerRefresh: case Hook::HandlerRefreshed:
            return true;
        default:
            return false;
        }
    }

    bool dispatch(Hook event, MagGameControlsCpu& cpu) {
        // The first thread to start an update tick owns the session; others are ignored entirely.
        if (event == Hook::OuterEnter && !thread) thread = cpu.thread;
        if (!thread || cpu.thread != *thread) return false;
        if (lifecycle_hook(event)) {
            lifecycle_event(event, cpu);
            return false;
        }
        if (event == Hook::OuterEnter) {
            outer_enter();
            return false;
        }
        if (!outer) return false;
        check_capture_generation();
        switch (event) {
        case Hook::OuterExit: outer_exit(); break;
        case Hook::PollEnter: poll_enter(); break;
        case Hook::PollExit: poll_exit(); break;
        case Hook::SampleSeal: sample_seal(); break;
        case Hook::HandlerEnter: handler_enter(cpu); break;
        case Hook::HandlerExit: handler_exit(); break;
        case Hook::ControlLoop: current_control = 0; break;
        case Hook::ControlRecognize: return recognize(cpu);
        case Hook::ControlCallback: control_callback(cpu); break;
        case Hook::ActorEnter: actor_enter(cpu); break;
        case Hook::ActorExit: actor_exit(); break;
        case Hook::Prepare: prepare(cpu); break;
        case Hook::YawRateEnter: yaw_rate_enter(cpu); break;
        case Hook::YawRateExit: scope_exit(yaw_rate, yaw_rate_observed); break;
        case Hook::YawDamage: yaw_damage(cpu); break;
        case Hook::SprintPitchRateEnter: sprint_pitch_rate_enter(cpu); break;
        case Hook::SprintPitchRateExit: scope_exit(sprint_pitch_rate, sprint_pitch_observed); break;
        case Hook::SprintPitchDamage: sprint_pitch_damage(cpu); break;
        case Hook::BasicYaw: case Hook::BasicPitch: case Hook::SprintYaw: case Hook::SprintPitch: consume(event, cpu); break;
        case Hook::AssistEligibility: case Hook::AssistAdhesion: return assist_early_result(event, cpu);
        case Hook::AssistCleanupEnter: assist_cleanup_enter(cpu); break;
        case Hook::AssistCleanupExit: assist_cleanup_exit(); break;
        case Hook::AssistFrictionYaw: case Hook::AssistFrictionPitch: assist_friction(cpu); break;
        default: break;
        }
        return false;
    }
};

MagGameControlsAdapter::MagGameControlsAdapter(MagGameControlsMemory memory, uint64_t session, const ControlsProfile& profile,
                                               void (*capture)(bool), void (*mouse_capture)(bool)) {
    mag_game_controls_validate_profile(profile);
    if (!session) throw std::invalid_argument("game controls need a session");
    if (!memory.read || !memory.write) throw std::invalid_argument("game controls need guest memory access");
    state_ = std::make_unique<State>(memory, session, profile, capture, mouse_capture);
    mag_controls_configure(profile);
    mag_controls_set_supported_context(false);
    state_->seen_generation = mag_controls_current_generation();
    state_->register_shared_native_actions();
}

MagGameControlsAdapter::~MagGameControlsAdapter() {
    auto& s = *state_;
    try {
        mag_input_capture_set_keyboard_owner(0);
    } catch (...) {
    }
    try {
        s.abort_capture_poll();
        s.retire();
    } catch (...) {
    }
    s.cancel_quietly("stopped");
}

bool MagGameControlsAdapter::dispatch(MagControlsHook event, MagGameControlsCpu cpu) noexcept {
    auto& s = *state_;
    try {
        return s.dispatch(event, cpu);
    } catch (const std::exception& error) {
        // Name the reason, so a log shows which guest record or engine rule refused.
        try {
            s.error_status = std::string(InvalidState) + ": " + error.what();
            s.cancel_quietly(s.error_status.c_str());
        } catch (...) {
            s.cancel_quietly(InvalidState);
        }
    } catch (...) {
        s.cancel_quietly(InvalidState);
    }
    return false;
}

const char* MagGameControlsAdapter::status() const noexcept { return state_->status; }

void MagGameControlsAdapter::set_census(void (*sink)(const char*)) {
    state_->census_sink = sink;
    state_->census_seen.clear();
}

void MagGameControlsAdapter::request_profile(const ControlsProfile& profile) {
    mag_game_controls_validate_profile(profile);
    std::lock_guard lock(state_->pending_mutex);
    state_->pending = profile;
}
