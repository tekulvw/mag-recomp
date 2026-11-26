// Synthetic checks of the game controls adapter (docs/native-controls.md) against the real Rust
// input engine. The guest is a fake big-endian memory holding only the records of section 2; no
// game file or lifted code is used. Each scenario runs in its own process (the engine is global).
#include "mag_game_controls_adapter.h"
#include "mag_game_controls_catalog.h"
#include "mag_game_controls_guest.h"
#include "player_controls_rust.h"
#include "player_input_capture.h"
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
using namespace mag;
using Hook = MagControlsHook;

namespace {
void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
uint64_t time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- Fake guest memory ------------------------------------------------------------------------

struct Memory {
    std::map<uint32_t, uint8_t> bytes;  // Mapped bytes only; anything else is unreadable.
    unsigned reads = 0;
    void (*on_read)(uint32_t address) = nullptr;  // Lets a test act "from another thread" mid-event.
    void map(uint32_t address, uint32_t size) {
        for (uint32_t i = 0; i < size; ++i) bytes.try_emplace(address + i, 0);
    }
    void put32(uint32_t address, uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes[address + i] = uint8_t(value >> (24 - 8 * i));
    }
    void putf(uint32_t address, float value) { put32(address, std::bit_cast<uint32_t>(value)); }
    void put8(uint32_t address, uint8_t value) { bytes[address] = value; }
    uint32_t get32(uint32_t address) const {
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value = value << 8 | bytes.at(address + i);
        return value;
    }
    float getf(uint32_t address) const { return std::bit_cast<float>(get32(address)); }
    static bool read(void* user, uint32_t address, void* output, size_t size) {
        auto& m = *static_cast<Memory*>(user);
        ++m.reads;
        if (m.on_read) m.on_read(address);
        for (size_t i = 0; i < size; ++i) {
            const auto it = m.bytes.find(uint32_t(address + i));
            if (it == m.bytes.end()) return false;
            static_cast<uint8_t*>(output)[i] = it->second;
        }
        return true;
    }
    static bool write(void* user, uint32_t address, const void* input, size_t size) {
        auto& m = *static_cast<Memory*>(user);
        for (size_t i = 0; i < size; ++i)
            if (!m.bytes.count(uint32_t(address + i))) return false;
        for (size_t i = 0; i < size; ++i) m.bytes[uint32_t(address + i)] = static_cast<const uint8_t*>(input)[i];
        return true;
    }
    MagGameControlsMemory view() { return {this, read, write}; }
    // A doubly linked list at `head` through `nodes` (each node: next +0, prev +4).
    void link(uint32_t head, const std::vector<uint32_t>& nodes) {
        uint32_t previous = head;
        for (uint32_t node : nodes) {
            put32(previous, node);
            put32(node + 4, previous);
            previous = node;
        }
        put32(previous, head);
        put32(head + 4, previous);
    }
};

// ---- The synthetic world (section 2) ---------------------------------------------------------

constexpr uint32_t ObjectManager = 0x01FB3D90, PoolPointer = 0x01D39078, InputManager = 0x01FB8E60,
                   UiManager = 0x01FB82C0, UiHandler = UiManager + 0x28, Pause = 0x01DC8100, Processing = 0x01DB3B18;
constexpr uint32_t BaseVtable = 0x01A57350, ActorVtable = 0x01A48888, ControllerVtable = 0x01A6AC30,
                   LogicVtable = 0x01A6AE18, BasicStateVtable = 0x01A6DD28, CameraVtable = 0x01A644E0;
constexpr uint32_t Index = 3, Generation = 7, Handle = Index << 16 | Generation;
constexpr uint32_t Pool = 0x10000000, Actor = 0x10001000, Controller = 0x10002000, Handler = Controller + 12,
                   Logic = 0x10003000, State = 0x10004000, BaseDescriptor = 0x10005000, Registered = 0x10006000,
                   Watched = 0x10010000, Bindings = 0x10020000, Tuning = 0x10030000, Pad = 0x10040000,
                   Camera = 0x10050000, OtherCamera = 0x10051000, OtherActor = 0x10052000, Packages = 0x10060000,
                   Elements = 0x10070000, Outputs = 0x10080000;
constexpr uint32_t Record = 0x8;  // Control record within a watched node.

constexpr uint32_t Forward = 0xB164D156, Backward = 0x93115EC5, Left = 0x44533986, Right = 0xF3468EB5,
                   Fire = 0x58DE09CF, Aim = 0xC07AA75A, Jump = 0xA7B30616, Sprint = 0xEF8055B7, Reload = 0xA2B8CE55,
                   Interact = 0x47CC8C92, Stance = 0x541512ED, CycleWeapons = 0x2C4380C4,
                   CycleEquipment = 0xCB535315, Melee = 0xDB59E9A9, Start = 0x163AAB24;
constexpr uint32_t AimAlias = 0x68C3570D;  // Watched token standing for Aim.
constexpr uint32_t LookTokens[] = {0xC8A3E21D, 0x941734A2, 0x661CB595, 0xC34F3A66};

// Owner controls: gameplay actions on channels 4..11 and 14..19, look axes on 20..23, Start on its
// system channel 12. Channels 0..3 and 12..13 belong to the system buttons.
struct Control {
    uint32_t token, channel;
};
const std::vector<Control> OwnerControls = {
    {Forward, 4}, {Backward, 5}, {Left, 6}, {Right, 7}, {Fire, 8}, {AimAlias, 9},
    {Jump, 10}, {Sprint, 11}, {Reload, 14}, {Interact, 15}, {Stance, 16}, {CycleWeapons, 17},
    {CycleEquipment, 18}, {Melee, 19}, {LookTokens[0], 20}, {LookTokens[1], 21}, {LookTokens[2], 22},
    {LookTokens[3], 23}, {Start, 12}};

uint32_t node_of(size_t i) { return Watched + uint32_t(i) * 0x40; }
uint32_t record_of(uint32_t token) {
    for (size_t i = 0; i < OwnerControls.size(); ++i)
        if (OwnerControls[i].token == token) return node_of(i) + Record;
    throw std::runtime_error("unknown token");
}

void handler_base(Memory& m, uint32_t handler, uint32_t vtable, bool enabled) {
    m.map(handler, 0x4C);
    m.put32(handler, vtable);
    m.link(handler + 4, {});
    m.put8(handler + 0x40, enabled);
}

void build_world(Memory& m) {
    m.map(ObjectManager + 0x44, 4);
    m.put32(ObjectManager + 0x44, Handle);
    m.put32(PoolPointer, Pool);
    m.map(Pool, 0x100);
    m.put32(Pool + 0x28 + 4 * Index, Actor);
    // Actor, controller, logic, movement state.
    m.map(Actor, 0x700);
    m.put32(Actor, ActorVtable);
    m.put32(Actor + 4, Handle);
    m.put32(Actor + 0x590, Controller);
    m.put32(Actor + 0x594, Logic);
    m.put32(Actor + 0x670, State);
    m.map(Controller, 12);
    m.put32(Controller, ControllerVtable);
    m.put32(Controller + 4, Actor);
    m.map(Logic, 8);
    m.put32(Logic, LogicVtable);
    m.put32(Logic + 4, Actor);
    m.map(State, 0x14);
    m.put32(State, BasicStateVtable);
    m.put32(State + 4, Actor);
    m.put8(State + 0x10, 1);
    // Base handler vtable and its update descriptor.
    m.put32(BaseVtable + 8, BaseDescriptor);
    m.put32(BaseDescriptor, 0x003A2C84);
    m.put32(BaseDescriptor + 4, 0x01B35B20);
    // The owner's handler with its watched controls.
    handler_base(m, Handler, BaseVtable, true);
    m.putf(Tuning + 4, 0);
    m.map(Tuning, 0x2C);
    m.putf(Tuning + 0x0C, 1.0f);
    m.putf(Tuning + 0x14, 0.5f);
    std::vector<uint32_t> nodes;
    for (size_t i = 0; i < OwnerControls.size(); ++i) {
        const uint32_t node = node_of(i), binding = Bindings + uint32_t(i) * 0x10;
        m.map(node, 0x24);
        m.put32(node + Record, OwnerControls[i].token);
        m.put32(node + Record + 4, binding);
        m.put32(node + Record + 8, Tuning);
        m.map(binding, 0x0C);
        m.put32(binding + 8, OwnerControls[i].channel);
        nodes.push_back(node);
    }
    m.link(Handler + 4, nodes);
    // Input manager: registered handlers (the UI manager's and the owner's), channel values.
    m.map(InputManager + 0x2C, 8);
    m.map(Registered, 0x20);
    m.put32(Registered + 8, UiHandler);
    m.put32(Registered + 0x18, Handler);
    m.link(InputManager + 0x2C, {Registered, Registered + 0x10});
    m.map(InputManager + 0x7E0, 0x80);
    // UI manager: its embedded handler and an empty package vector.
    handler_base(m, UiHandler, 0x01A4EAB8, true);
    m.put32(0x01A4EAB8 + 8, 0x01AE1868);
    m.put32(0x01AE1868, 0x00F15B14);
    m.put32(0x01AE1868 + 4, 0x01B35B20);
    m.put32(UiManager + 0x88, 0);
    m.put32(UiManager + 0x8C, 0);
    m.map(Pause + 0x9C0, 4);
    m.map(Processing + 0x2F0, 8);
    m.map(Pad, 0x84);
    // Cameras with clean assist records: one following the player, one following another actor.
    for (uint32_t camera : {Camera, OtherCamera}) {
        m.map(camera, 0x230);
        m.put32(camera, CameraVtable);
        m.put32(camera + 0x22C, camera == Camera ? Actor : OtherActor);
        m.putf(camera + 0x1A0 + 8, 1.0f);
        m.putf(camera + 0x1A0 + 0x3C, 1.0f);
    }
    m.map(Outputs, 8);
    m.putf(Outputs, 5.0f);
    m.putf(Outputs + 4, 5.0f);
}

// ---- Seam driver -----------------------------------------------------------------------------

struct Cpu {
    std::array<uint64_t, 32> gpr{};
    std::array<double, 32> fpr{};
    uint64_t lr = 0, thread = 1;
    MagGameControlsCpu view() { return {std::span<uint64_t, 32>(gpr), std::span<double, 32>(fpr), lr, thread}; }
};

int capture_requests = 0, mouse_requests = 0, mouse_lock_requests = 0;
bool capture_on = false, mouse_on = false;
void on_capture(bool on) {
    ++capture_requests;
    capture_on = on;
}
void on_mouse(bool on) {
    ++mouse_requests;
    mouse_lock_requests += on;
    mouse_on = on;
}

struct Harness {
    Memory m;
    Cpu cpu;
    std::unique_ptr<MagGameControlsAdapter> adapter;
    uint64_t polls = 0;
    explicit Harness(ControlsProfile profile = mag_game_controls_default_profile()) {
        build_world(m);
        adapter = std::make_unique<MagGameControlsAdapter>(m.view(), 1, profile, on_capture, on_mouse);
    }
    bool fire(Hook event) { return adapter->dispatch(event, cpu.view()); }
    std::string status() const { return adapter->status(); }
    void begin_tick() {
        fire(Hook::OuterEnter);
        fire(Hook::PollEnter);
        ++polls;
        fire(Hook::SampleSeal);
    }
    void enter_handler(uint32_t handler = Handler) {
        cpu.gpr[3] = handler;
        cpu.gpr[4] = Pad;
        fire(Hook::HandlerEnter);
    }
    bool recognize(uint32_t token, uint32_t handler = Handler, double raw = 0) {
        return recognize_record(record_of(token), handler, raw);
    }
    bool recognize_record(uint32_t record, uint32_t handler, double raw = 0) {
        fire(Hook::ControlLoop);
        cpu.gpr[23] = handler;
        cpu.gpr[29] = record;
        cpu.gpr[3] = cpu.gpr[31] = record + 8;
        cpu.gpr[5] = 0;
        cpu.gpr[11] = 0xDEAD;
        cpu.fpr[1] = raw;
        return fire(Hook::ControlRecognize);
    }
    void end_poll() {
        fire(Hook::HandlerExit);
        fire(Hook::PollExit);
    }
    void enter_actor() {
        cpu.gpr[3] = Logic;
        fire(Hook::ActorEnter);
    }
    void end_tick() {
        fire(Hook::ActorExit);
        fire(Hook::OuterExit);
    }
    // One tick that attaches the sample and does nothing else; returns the status after the
    // handler update (the actor update may report its own state afterwards).
    std::string idle_tick() {
        begin_tick();
        enter_handler();
        const std::string attached = status();
        end_poll();
        enter_actor();
        end_tick();
        return attached;
    }
    InputFrameKey key() const { return {1, polls, 0}; }
    MagGuestTuple tuple_at(uint32_t record) const {
        MagGuestTuple t;
        for (size_t i = 0; i < t.size(); ++i) t[i] = m.bytes.at(record + 0x0C + uint32_t(i));
        return t;
    }
    MagGuestTuple tuple(uint32_t token) const { return tuple_at(record_of(token)); }
};
constexpr const char* Attached = "on-foot Rust input attached";
constexpr const char* Invalid = "rejected unavailable or invalid state";

// ---- Scenarios -------------------------------------------------------------------------------

void owner_resolution() {
    Memory base;
    build_world(base);
    const auto check = [&](const char* why, bool expected, auto change, bool allow_disabled = false) {
        Memory m = base;
        change(m);
        const auto owner = MagGameControlsGuest(m.view()).resolve_owner(Handler, allow_disabled);
        if (owner.has_value() != expected) throw std::runtime_error(why);
    };
    const auto none = [](Memory&) {};
    check("well-formed owner rejected", true, none);
    {
        const auto owner = MagGameControlsGuest(base.view()).resolve_owner(Handler, false);
        require(owner->actor == Actor && owner->logic == Logic && owner->state == State && owner->index == Index &&
                    owner->generation == Generation && owner->mouse_supported && owner->aim == MagMouseAimState::Basic,
                "owner record fields");
        const auto local = MagGameControlsGuest(base.view()).local_owner();
        require(local && *local == *owner, "local owner differs from the handler's owner");
    }
    check("wrong actor vtable accepted", false, [](Memory& m) { m.put32(Actor, 0x01A00000); });
    check("wrong controller vtable accepted", false, [](Memory& m) { m.put32(Controller, 0x01A00000); });
    check("wrong handler vtable accepted", false, [](Memory& m) { m.put32(Handler, 0x01A00000); });
    check("unregistered handler accepted", false, [](Memory& m) { m.link(InputManager + 0x2C, {Registered}); });
    check("doubly registered handler accepted", false, [](Memory& m) {
        m.map(Registered + 0x20, 0x10);
        m.put32(Registered + 0x28, Handler);
        m.link(InputManager + 0x2C, {Registered, Registered + 0x10, Registered + 0x20});
    });
    check("bad controller back pointer accepted", false, [](Memory& m) { m.put32(Controller + 4, Logic); });
    check("bad logic back pointer accepted", false, [](Memory& m) { m.put32(Logic + 4, Controller); });
    check("remote actor accepted", false, [](Memory& m) { m.put32(Actor + 0x6B4, 1); });
    check("stale generation accepted", false, [](Memory& m) { m.put32(Actor + 4, Handle + 1); });
    check("nonzero slot accepted", false, [](Memory& m) { m.put32(Handler + 0x3C, 1); });
    check("disabled handler accepted", false, [](Memory& m) { m.put8(Handler + 0x40, 0); });
    check("allowed disabled handler rejected", true, [](Memory& m) { m.put8(Handler + 0x40, 0); }, true);
    {
        Memory m = base;
        m.put32(State, 0x01A6BDF8);  // Sprint vtable with a basic state id: owned, unsupported.
        const auto owner = MagGameControlsGuest(m.view()).resolve_owner(Handler, false);
        require(owner && !owner->mouse_supported, "mismatched state pair kept mouse support");
        m.put8(State + 0x10, 23);
        require(MagGameControlsGuest(m.view()).resolve_owner(Handler, false)->aim == MagMouseAimState::Sprint, "sprint state");
    }
    {
        Memory m = base;
        MagGameControlsGuest guest(m.view());
        const auto owner = *guest.resolve_owner(Handler, false);
        require(guest.owner_matches(owner), "owner no longer matches");
        m.put32(Actor + 0x594, State);
        require(!guest.owner_matches(owner), "broken logic link still matches");
    }
}

void malformed_lists() {
    Memory base;
    build_world(base);
    {
        Memory m = base;
        const auto controls = MagGameControlsGuest(m.view()).watched_controls(Handler);
        require(controls && controls->size() == OwnerControls.size(), "well-formed watched list unreadable");
        require((*controls)[6].token == Jump && (*controls)[6].channel == 10 && (*controls)[6].record == record_of(Jump),
                "watched control fields");
    }
    {
        Memory m = base;  // Third node points back at the first: a cycle that never reaches the head.
        m.put32(node_of(2), node_of(0));
        require(!MagGameControlsGuest(m.view()).watched_controls(Handler), "cyclic list read as a list");
        require(!MagGameControlsGuest(m.view()).neutral_look(Handler), "cyclic list gave a look answer");
    }
    {
        Memory m = base;
        m.put32(node_of(3) + 4, node_of(1));
        require(!MagGameControlsGuest(m.view()).watched_controls(Handler), "bad prev link read as a list");
    }
    {
        Memory m = base;
        m.put32(Handler + 8, node_of(0));  // Head's prev must be the last node.
        require(!MagGameControlsGuest(m.view()).watched_controls(Handler), "bad head prev read as a list");
    }
    {
        Memory m = base;
        m.link(Handler + 4, {});
        const auto controls = MagGameControlsGuest(m.view()).watched_controls(Handler);
        require(controls && controls->empty(), "empty list not empty");
    }
    {
        Memory m = base;  // A node whose binding is zero is skipped, a non-pointer binding fails.
        m.put32(record_of(Jump) + 4, 0);
        require(MagGameControlsGuest(m.view()).watched_controls(Handler)->size() == OwnerControls.size() - 1, "unmapped node kept");
        m.put32(record_of(Jump) + 4, 0x1234);
        require(!MagGameControlsGuest(m.view()).watched_controls(Handler), "non-pointer binding accepted");
    }
    {
        Memory m = base;  // 257 nodes exceed the walk limit.
        std::vector<uint32_t> nodes;
        for (uint32_t i = 0; i < 257; ++i) {
            m.map(0x11000000 + i * 0x10, 0x10);
            nodes.push_back(0x11000000 + i * 0x10);
        }
        m.link(Handler + 4, nodes);
        require(!MagGameControlsGuest(m.view()).watched_controls(Handler), "overlong list accepted");
        nodes.pop_back();
        m.link(Handler + 4, nodes);
        require(MagGameControlsGuest(m.view()).watched_controls(Handler).has_value(), "256-node list rejected");
    }
    {
        Memory m = base;
        m.put32(Registered + 0x10 + 4, Registered + 0x10);
        require(!MagGameControlsGuest(m.view()).registration_count(Handler), "malformed registered list counted");
        require(!MagGameControlsGuest(m.view()).resolve_owner(Handler, true), "owner resolved over a malformed list");
    }
    {
        Memory m = base;  // Removal pending: only for the handler being processed; unreadable is unknown.
        MagGameControlsGuest guest(m.view());
        require(guest.removal_pending(Handler) == std::optional<bool>(false), "idle handler pending");
        m.put32(Processing + 0x2F0, Handler);
        m.put8(Processing + 0x2F4, 1);
        require(guest.removal_pending(Handler) == std::optional<bool>(true), "deferred removal not seen");
        m.bytes.erase(Processing + 0x2F4);
        require(!guest.removal_pending(Handler), "unreadable deferral known");
    }
    {
        Memory m = base;  // Neutral look reads all four channels.
        MagGameControlsGuest guest(m.view());
        require(guest.neutral_look(Handler) == std::optional<bool>(true), "neutral look");
        m.putf(InputManager + 0x7E0 + 4 * 21, 0.25f);
        require(guest.neutral_look(Handler) == std::optional<bool>(false), "deflected look");
        m.put32(record_of(LookTokens[3]) + 4, 0);
        require(!guest.neutral_look(Handler), "three look channels answered");
    }
}

// One UI package (flags at +0x40, element records from +4 to +8) in the UI manager's package
// vector, holding `count` empty element records.
constexpr uint32_t PackageVector = 0x10068000;
void ui_package(Memory& m, uint32_t count, bool active) {
    m.map(Packages, 0x44);
    m.put32(Packages + 4, Elements);
    m.put32(Packages + 8, Elements + count * 0x28);
    m.put32(Packages + 0x40, active ? 2 : 0);
    m.put32(PackageVector, Packages);
    m.put32(UiManager + 0x88, PackageVector);
    m.put32(UiManager + 0x8C, PackageVector + 4);
    m.map(Elements, count * 0x28);
}
// Element `i` of package 0: a UI element with the given vtable pair and an embedded handler.
uint32_t ui_element(Memory& m, uint32_t i, uint32_t primary, uint32_t secondary, bool enabled) {
    const uint32_t element = 0x10090000 + i * 0x100, properties = element + 0x80;
    m.put32(Elements + i * 0x28 + 0x24, element);
    m.map(element, 0xC4);
    m.put32(element, primary);
    m.put32(element + 4, secondary);
    m.link(element + 8, {});
    m.put8(element + 4 + 0x40, enabled);
    m.put32(element + 0x50, properties);
    m.put32(properties + 0x40, 2);
    m.put32(secondary + 8, BaseDescriptor);
    return element;
}

void routes() {
    Memory base;
    build_world(base);
    {
        MagGameControlsGuest guest(base.view());
        const auto gameplay = guest.gameplay_routes();
        require(gameplay && gameplay->empty(), "no UI must allow gameplay without routes");
        const auto system = guest.system_routes();
        require(system && *system == std::vector<uint32_t>{Handler}, "system routes must list the owner's handler only");
    }
    {
        Memory m = base;
        m.put32(Pause + 0x9C0, 1);
        require(!MagGameControlsGuest(m.view()).gameplay_routes(), "paused game allowed gameplay");
        m.bytes.erase(Pause + 0x9C0);
        require(!MagGameControlsGuest(m.view()).gameplay_routes(), "unreadable pause allowed gameplay");
    }
    Memory ui = base;
    ui_package(ui, 2, true);
    {
        Memory m = ui;
        ui_element(m, 0, 0x01A00000, 0x01A00040, true);
        require(!MagGameControlsGuest(m.view()).gameplay_routes(), "unknown element type allowed gameplay");
    }
    {
        Memory m = ui;  // Unknown type behind an inactive package is not examined.
        ui_element(m, 0, 0x01A00000, 0x01A00040, true);
        m.put32(Packages + 0x40, 0);
        require(MagGameControlsGuest(m.view()).gameplay_routes().has_value(), "inactive package examined");
    }
    {
        Memory m = ui;
        const uint32_t element = ui_element(m, 0, 0x01A4FCB8, 0x01A4FCF8, true);
        m.put32(0x01A4FCF8 + 12, 0x01AE28D8);
        m.put32(0x01AE28D8, 0x00F2AD8C);
        m.put32(0x01AE28D8 + 4, 0x01B35B20);
        const auto gameplay = MagGameControlsGuest(m.view()).gameplay_routes();
        require(gameplay && *gameplay == std::vector<uint32_t>{element + 4}, "scripted element did not route");
        const auto system = MagGameControlsGuest(m.view()).system_routes();
        require(system && system->size() == 2 && (*system)[1] == element + 4, "scripted element missing from system routes");
        m.put32(0x01AE28D8, 0x00F2AD90);
        require(!MagGameControlsGuest(m.view()).gameplay_routes(), "scripted element with a foreign callback routed");
    }
    {
        Memory m = ui;
        const uint32_t element = ui_element(m, 0, 0x01A49828, 0x01A49864, true);
        require(MagGameControlsGuest(m.view()).gameplay_routes()->empty(), "idle plain element blocked gameplay");
        m.map(0x100A0000, 8);
        m.link(element + 8, {0x100A0000});
        require(!MagGameControlsGuest(m.view()).gameplay_routes(), "plain element with watched controls allowed gameplay");
        m.link(element + 8, {});
        m.put32(element + 4 + 0x48, 1);
        require(!MagGameControlsGuest(m.view()).gameplay_routes(), "plain element with a mapped mask allowed gameplay");
        m.put8(element + 4 + 0x40, 0);
        require(MagGameControlsGuest(m.view()).gameplay_routes()->empty(), "disabled plain element blocked gameplay");
    }
    {
        Memory m = base;  // The UI manager's handler must be registered exactly once.
        m.link(InputManager + 0x2C, {Registered + 0x10});
        require(!MagGameControlsGuest(m.view()).system_routes(), "unregistered UI handler accepted");
    }
    {
        Memory m = base;  // An enabled handler listed twice fails.
        m.map(Registered + 0x20, 0x10);
        m.put32(Registered + 0x28, Handler);
        m.link(InputManager + 0x2C, {Registered, Registered + 0x10, Registered + 0x20});
        require(!MagGameControlsGuest(m.view()).system_routes(), "doubly listed handler accepted");
    }
}

void full_tick() {
    Harness h;
    h.idle_tick();
    require(h.status() == Attached, "first tick did not attach on foot");
    require(capture_on && mouse_on, "desktop and mouse capture not requested");
    mag_controls_push_event({{InputSource::Keyboard, 44}, true, time_ns()});  // Space: Jump.
    mag_controls_push_event({{InputSource::Mouse, 1}, true, time_ns()});      // Right button: Aim.

    h.begin_tick();
    h.enter_handler();
    require(h.recognize(Jump), "Jump recognition stayed native");
    const auto tuple = h.tuple(Jump);
    const uint32_t bits = uint32_t(tuple[12]) << 24 | uint32_t(tuple[13]) << 16 | uint32_t(tuple[14]) << 8 | tuple[15];
    require(mag_guest_tuple_value(tuple) == 1.0f, "published value");
    // With no press delay a held key is Pressed at once: Down and Press in the same update.
    require(mag_guest_tuple_phase(tuple) == 4, "published phase is not Pressed");
    require((bits & 0x0002) && (bits & 0x0080), "published events lack Down and Press");
    require(!(bits & ~0x3FFFu), "published events outside the game's bits");
    require(h.cpu.gpr[3] == bits && h.cpu.gpr[11] == 0, "registers do not carry the published events");
    // The watched alias token is recognized as Aim.
    require(mag_game_controls_token_action(AimAlias) == Aim, "alias table");
    require(h.recognize(AimAlias) && mag_guest_tuple_value(h.tuple(AimAlias)) == 1.0f, "Aim alias not recognized");
    // Native path: nothing offered, no entry, Auto mode, no remap.
    const auto reload_before = h.tuple(Reload);
    require(!h.recognize(Reload), "unoffered Reload replaced");
    require(h.tuple(Reload) == reload_before, "native path wrote a tuple");
    require(h.cpu.gpr[11] == 0xDEAD, "native path wrote registers");
    // A system button on the owner's handler also stays native without an offer.
    require(!h.recognize(Start), "unoffered Start replaced");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    require(!mag_controls_assist_allowed(mag_controls_current_policy()), "accepted KBM action left aim assist on");

    // Release: Up is published, and the observed release is not offered again.
    mag_controls_push_event({{InputSource::Keyboard, 44}, false, time_ns()});
    h.begin_tick();
    h.enter_handler();
    require(h.recognize(Jump), "Jump release stayed native");
    require(mag_guest_tuple_value(h.tuple(Jump)) == 0.0f && (h.cpu.gpr[3] & 0x0004), "release not published as Up");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    h.begin_tick();
    h.enter_handler();
    require(!mag_controls_offer(h.key(), Jump), "observed Jump release was offered again");
    require(h.recognize(Jump), "known Jump entry no longer recognized");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    require(h.status() == Attached, "status after ticks");
}

// A scripted UI element routed during gameplay, watching Jump's channel. Returns its handler.
uint32_t route_handler(Memory& m, uint32_t& record) {
    ui_package(m, 1, true);
    const uint32_t element = ui_element(m, 0, 0x01A4FCB8, 0x01A4FCF8, true);
    m.put32(0x01A4FCF8 + 12, 0x01AE28D8);
    m.put32(0x01AE28D8, 0x00F2AD8C);
    m.put32(0x01AE28D8 + 4, 0x01B35B20);
    const uint32_t node = 0x100B0000, binding = 0x100B0100;
    m.map(node, 0x24);
    m.map(binding, 0x0C);
    m.put32(binding + 8, 10);
    record = node + Record;
    m.put32(record, 0x12345678);
    m.put32(record + 4, binding);
    m.put32(record + 8, Tuning);
    m.link(element + 8, {node});
    return element + 4;
}

void route_recognition() {
    Harness h;
    uint32_t record = 0;
    const uint32_t route = route_handler(h.m, record);
    require(h.idle_tick() == Attached, "UI route blocked gameplay attachment");
    mag_controls_push_event({{InputSource::Keyboard, 44}, true, time_ns()});
    h.begin_tick();
    h.enter_handler();
    h.fire(Hook::HandlerExit);
    h.enter_handler(route);
    require(h.recognize_record(record, route), "route handler recognition stayed native");
    const auto published = h.tuple_at(record);
    require(mag_guest_tuple_value(published) == 1.0f && mag_guest_tuple_phase(published) == 4, "route tuple");
    h.fire(Hook::HandlerExit);
    require(mag_controls_assist_allowed(mag_controls_current_policy()), "route recognition accepted gameplay input");
    // The owner's own Jump is held by the adapter; the route's control is not.
    h.enter_handler();
    require(h.recognize(Jump), "owner Jump stayed native");
    h.fire(Hook::HandlerExit);
    h.cpu.gpr[3] = Handler;
    h.fire(Hook::HandlerRefresh);  // Native map change: cancel and give back held controls.
    h.fire(Hook::HandlerRefreshed);
    require(h.status() == std::string("native control map changed; waiting for fresh input"), "refresh did not cancel");
    require(h.tuple(Jump) == mag_guest_neutral_tuple(), "cancel kept the owner's Jump");
    require(h.tuple_at(record) == published, "cancel neutralised a route control the adapter never held");
    h.fire(Hook::PollExit);
    h.enter_actor();
    h.end_tick();
}

void mouse_look() {
    Harness h;
    h.idle_tick();
    mag_controls_set_window_capture(true, true);
    mag_controls_push_relative_mouse(100, 0);
    h.m.putf(Actor + 0x184, 0.7f);
    h.m.putf(Actor + 0x18C, -0.4f);
    h.m.putf(Actor + 0x188, 0.25f);

    h.begin_tick();
    h.enter_handler();
    h.end_poll();
    h.enter_actor();
    h.cpu.gpr[30] = Logic;
    h.cpu.gpr[31] = State;
    h.cpu.fpr[31] = 1.0 / 30;
    h.fire(Hook::Prepare);
    require(h.status() == std::string("on-foot mouse prepared"), "mouse not prepared");
    // Mouse motion: the stick's look rates are replaced, yaw by the wake input (first one positive).
    require(h.m.getf(Actor + 0x184) == 0.001f && h.m.getf(Actor + 0x18C) == 0.0f, "look-rate inputs not replaced");
    h.cpu.gpr[3] = Actor;
    h.cpu.lr = 0x0061E7B4;
    h.fire(Hook::YawRateEnter);
    h.cpu.fpr[1] = 1.0;
    h.fire(Hook::YawDamage);
    h.fire(Hook::YawRateExit);
    h.cpu.gpr[3] = Actor;
    h.fire(Hook::BasicYaw);
    const float expected = float(0.25f + -(100 * MouseLookSettings{}.radians_per_count));
    require(h.m.getf(Actor + 0x188) == expected, "yaw request did not receive the flipped mouse yaw");
    h.fire(Hook::BasicYaw);
    require(h.m.getf(Actor + 0x188) == expected, "mouse yaw consumed twice");
    // No vertical motion: basic pitch is left alone.
    h.cpu.gpr[31] = Actor;
    h.cpu.fpr[31] = 0.5;
    h.fire(Hook::BasicPitch);
    require(h.cpu.fpr[31] == 0.5, "pitch changed without vertical motion");
    h.end_tick();
    // Without new motion the latch is gone after the tick.
    h.m.putf(Actor + 0x188, 0.25f);
    h.begin_tick();
    h.enter_handler();
    h.end_poll();
    h.enter_actor();
    h.cpu.gpr[3] = Actor;
    h.fire(Hook::BasicYaw);
    require(h.m.getf(Actor + 0x188) == 0.25f, "stale mouse sample reused");
    h.end_tick();

    // The wake input flips sign at every prepared frame with motion (vertical motion too), so the
    // turn it adds cancels; a prepared frame without motion writes zero and keeps the sign.
    const auto prepared = [&](double dx, double dy) {
        if (dx || dy) mag_controls_push_relative_mouse(dx, dy);
        h.m.putf(Actor + 0x184, 0.7f);
        h.m.putf(Actor + 0x18C, -0.4f);
        h.begin_tick();
        h.enter_handler();
        h.end_poll();
        h.enter_actor();
        h.cpu.gpr[30] = Logic;
        h.cpu.gpr[31] = State;
        h.cpu.fpr[31] = 1.0 / 30;
        h.fire(Hook::Prepare);
        require(h.status() == std::string("on-foot mouse prepared"), "mouse not prepared");
        require(h.m.getf(Actor + 0x18C) == 0.0f, "pitch look-rate input not cleared");
        const float yaw = h.m.getf(Actor + 0x184);
        h.end_tick();
        return yaw;
    };
    require(prepared(0, 40) == -0.001f, "vertical motion did not wake with the flipped sign");
    require(prepared(0, 0) == 0.0f, "a frame without motion wrote a wake input");
    require(prepared(-30, 0) == 0.001f, "the wake sign did not keep alternating");
}

void aim_assist() {
    Harness h;
    h.idle_tick();
    mag_controls_push_event({{InputSource::Keyboard, 44}, true, time_ns()});
    h.begin_tick();
    h.enter_handler();
    require(h.recognize(Jump), "Jump not recognized");
    h.end_poll();
    h.enter_actor();
    const auto eligibility = [&](uint32_t camera) {
        h.cpu.gpr[3] = camera + 0x1A0;
        h.cpu.gpr[4] = camera == Camera ? Actor : OtherActor;
        return h.fire(Hook::AssistEligibility);
    };
    require(eligibility(Camera) && h.cpu.gpr[3] == 0, "KBM use left assist eligible");
    require(!eligibility(OtherCamera) && h.cpu.gpr[3] == OtherCamera + 0x1A0, "foreign camera denied");
    h.cpu.gpr[3] = Camera + 0x1A0;
    h.cpu.gpr[4] = Actor;
    h.cpu.gpr[6] = Outputs;
    h.cpu.gpr[7] = Outputs + 4;
    require(h.fire(Hook::AssistAdhesion) && h.cpu.gpr[3] == 0, "adhesion not denied");
    require(h.m.getf(Outputs) == 0.0f && h.m.getf(Outputs + 4) == 0.0f, "adhesion outputs not cleared");
    h.cpu.gpr[3] = Camera;
    h.cpu.fpr[1] = 0.3;
    h.fire(Hook::AssistFrictionYaw);
    require(h.cpu.fpr[1] == 1.0, "friction not neutralised");
    h.cpu.gpr[3] = OtherCamera;
    h.cpu.fpr[1] = 0.3;
    h.fire(Hook::AssistFrictionPitch);
    require(h.cpu.fpr[1] == 0.3, "foreign camera friction changed");
    h.end_tick();

    // Controller mode allows assist again, but the remembered denial lasts until a clean cleanup.
    auto profile = mag_game_controls_default_profile();
    profile.mode = InputMode::Controller;
    h.adapter->request_profile(profile);
    h.begin_tick();
    require(h.status() != std::string(Attached), "profile change did not cancel");
    h.enter_handler();
    h.end_poll();
    h.enter_actor();
    require(mag_controls_assist_allowed(mag_controls_current_policy()), "controller mode denies assist");
    require(eligibility(Camera), "remembered denial dropped before cleanup");
    h.m.putf(Camera + 0x1A0 + 8, 0.5f);  // Assist motion still in progress.
    h.cpu.gpr[3] = Camera + 0x1A0;
    h.cpu.gpr[4] = Camera;
    h.fire(Hook::AssistCleanupEnter);
    h.fire(Hook::AssistCleanupExit);
    require(eligibility(Camera), "unclean cleanup cleared the denial");
    h.m.putf(Camera + 0x1A0 + 8, 1.0f);
    h.cpu.gpr[3] = Camera + 0x1A0;
    h.cpu.gpr[4] = Camera;
    h.fire(Hook::AssistCleanupEnter);
    h.fire(Hook::AssistCleanupExit);
    require(!eligibility(Camera), "clean cleanup kept the denial");
    h.end_tick();
}

void lifecycle() {
    Harness h;
    h.idle_tick();
    require(h.status() == Attached, "first tick did not attach");
    // Removal of the owner's handler cancels and tombstones it.
    h.cpu.gpr[4] = Handler;
    h.fire(Hook::HandlerRemove);
    h.fire(Hook::HandlerRemoveExit);
    require(h.status() == std::string("owned input handler removed"), "removal did not cancel");
    require(h.idle_tick() == "input handler awaiting valid registration", "tombstoned handler attached");
    require(h.status() == std::string("local actor has no current input poll"), "actor update without a frame");
    // Re-registration that proves a single registration lifts the tombstone.
    const auto add = [&]() {
        h.m.link(InputManager + 0x2C, {Registered});
        h.cpu.gpr[3] = InputManager;
        h.cpu.gpr[4] = Handler;
        h.fire(Hook::HandlerAdd);
        h.m.link(InputManager + 0x2C, {Registered, Registered + 0x10});
        h.cpu.gpr[3] = Handler;
        h.fire(Hook::HandlerAdded);
        h.cpu.gpr[3] = Handler;
        h.fire(Hook::HandlerRefresh);
        h.fire(Hook::HandlerRefreshed);
    };
    add();
    h.idle_tick();
    require(h.status() == Attached, "proven registration did not lift the tombstone");
    // An addition while another handler is being removed (deferred) is not proof.
    h.cpu.gpr[4] = Handler;
    h.fire(Hook::HandlerRemove);
    h.m.put32(Processing + 0x2F0, Handler);
    h.m.put8(Processing + 0x2F4, 1);
    add();
    h.m.put32(Processing + 0x2F0, 0);
    h.m.put8(Processing + 0x2F4, 0);
    require(h.idle_tick() == "input handler awaiting valid registration", "pending removal lifted the tombstone");
    add();
    h.idle_tick();
    require(h.status() == Attached, "second proven registration did not attach");
    // Reconstruction: the handler must read as freshly constructed before re-registration counts.
    h.cpu.gpr[3] = Handler;
    h.cpu.gpr[4] = 0;
    h.cpu.gpr[5] = 0;
    h.fire(Hook::HandlerConstruct);
    require(h.status() == std::string("owned input handler reconstructed"), "reconstruction did not cancel");
    h.fire(Hook::HandlerConstructed);  // Still holds its old watched list: not fresh.
    add();
    require(h.idle_tick() == "input handler awaiting valid registration", "unfresh construction lifted the tombstone");
    const uint32_t first = h.m.get32(Handler + 4), last = h.m.get32(Handler + 8);
    h.cpu.gpr[3] = Handler;
    h.cpu.gpr[4] = 0;  // Priority and slot of the new construction.
    h.cpu.gpr[5] = 0;
    h.fire(Hook::HandlerConstruct);
    h.m.link(Handler + 4, {});
    h.fire(Hook::HandlerConstructed);
    h.m.put32(Handler + 4, first);
    h.m.put32(Handler + 8, last);
    h.m.put32(first + 4, Handler + 4);
    h.m.put32(last, Handler + 4);
    add();
    require(h.idle_tick() == Attached, "fresh construction and registration did not attach");
    // Removal of an untracked handler changes nothing.
    h.cpu.gpr[4] = 0x10123450;
    h.fire(Hook::HandlerRemove);
    require(h.status() == Attached, "foreign handler removal cancelled");
    // A refresh of the owner's handler without a pending addition cancels.
    h.cpu.gpr[3] = Handler;
    h.fire(Hook::HandlerRefresh);
    h.fire(Hook::HandlerRefreshed);
    require(h.status() == std::string("native control map changed; waiting for fresh input"), "refresh did not cancel");
}

void threads_and_errors() {
    Harness h;
    // Nothing happens before the first update tick or from another thread.
    h.enter_handler();
    require(h.status() == std::string("waiting for input poll"), "event before the first tick handled");
    h.cpu.thread = 1;
    h.fire(Hook::OuterEnter);
    h.cpu.thread = 2;
    h.fire(Hook::OuterEnter);
    h.fire(Hook::PollEnter);
    h.fire(Hook::SampleSeal);
    h.enter_handler();
    require(h.status() == std::string("waiting for input poll"), "second thread attached the frame");
    h.cpu.thread = 1;
    h.fire(Hook::OuterExit);
    // Outside an update tick the owning thread is ignored too.
    h.fire(Hook::PollEnter);
    h.fire(Hook::SampleSeal);
    h.enter_handler();
    require(h.status() == std::string("waiting for input poll"), "event outside a tick handled");
    h.fire(Hook::HandlerExit);
    h.fire(Hook::PollExit);
    // A normal tick, then an invalid tuning record (low >= high) is an error: cancel and native.
    h.idle_tick();
    require(h.status() == Attached, "owner thread did not attach");
    h.m.putf(Tuning + 8, 1.0f);
    mag_controls_push_event({{InputSource::Keyboard, 44}, true, time_ns()});
    h.begin_tick();
    h.enter_handler();
    const auto before = h.tuple(Jump);
    require(!h.recognize(Jump), "invalid tuning replaced recognition");
    require(h.status() == std::string(Invalid) + ": invalid tuning record", "invalid tuning did not cancel with its reason");
    require(h.cpu.gpr[11] == 0xDEAD && h.tuple(Jump) == before, "failed recognition changed state");
    require(!capture_on && !mouse_on, "cancel kept desktop capture");
    // After the cancel the rest of the tick is inert.
    require(!h.recognize(Jump), "cancelled frame recognized");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    // Removing the guest's readability of the local actor stops attachment without an error.
    h.m.putf(Tuning + 8, 0.0f);
    h.m.bytes.erase(ObjectManager + 0x44);
    h.begin_tick();
    h.enter_handler();
    h.end_poll();
    require(h.status() == std::string("no supported local gameplay handler"), "missing owner not reported");
    h.end_tick();
    // Destroying the adapter stops it.
    h.adapter.reset();
}
}  // namespace


// A menu that takes the look channels before the player's handler releases the mouse lock while
// keyboard capture stays, once its hold has lasted three handler updates; look coming back locks
// the mouse again the same way. A one- or two-update flicker (a menu animating) changes nothing.
void menu_holds_look() {
    Harness h;
    h.idle_tick();
    auto update = [&](uint32_t consumed) {
        h.m.put32(Pad + 0x80, consumed);
        h.begin_tick();
        h.enter_handler();
        h.fire(Hook::HandlerExit);
        h.end_poll();
        h.end_tick();
    };
    constexpr uint32_t Look = 0xFu << 20;  // the look axes, channels 20..23
    update(0);
    require(capture_on && mouse_on, "gameplay locks the mouse");
    update(Look);
    update(Look);
    update(0);  // a flicker while the menu animates
    require(mouse_on, "a two-update flicker released the mouse lock");
    update(Look);
    update(Look);
    require(mouse_on, "the lock followed the menu before its hold was stable");
    update(Look);
    require(capture_on && !mouse_on, "a stable menu hold on look did not release the mouse lock");
    require(h.status() == std::string("on-foot input attached; a menu holds look"), "menu status not reported");
    // Each poll attaches again before the handler update: while the hold lasts nothing may ask for
    // the lock back, or the cursor would be captured and released once per frame.
    const int locks = mouse_lock_requests;
    update(Look);
    update(Look);
    require(!mouse_on && mouse_lock_requests == locks, "a held menu asked for the mouse lock again");
    update(0);
    update(Look);  // flicker on the way out
    update(0);
    update(0);
    require(!mouse_on, "the lock came back before look was stably free");
    update(0);
    require(capture_on && mouse_on, "look returning did not lock the mouse again");
    require(h.status() == std::string("on-foot Rust input attached"), "status did not return to gameplay");
}

// A menu that holds look while the player stays on foot (the in-match pause menu, the loadout
// screen) is served by the keyboard pad like any menu: the pad goes live once the hold is stable,
// the adapter takes over nothing on the menu's own handlers (their controls read the pad), the
// player's handler keeps its keys, and the pad is muted again once look returns.
void menu_holding_look_gets_the_pad() {
    Harness h;
    uint32_t record = 0;
    const uint32_t menu = route_handler(h.m, record);  // a scripted menu element on Jump's channel
    require(h.idle_tick() == Attached, "first tick did not attach on foot");
    const auto muted = [] { return mag_input_capture_keyboard_pad_muted() != 0; };
    require(muted(), "on foot the pad is muted");
    constexpr uint32_t Look = 0xFu << 20;
    auto update = [&](uint32_t consumed) {
        h.m.put32(Pad + 0x80, consumed);
        h.begin_tick();
        h.enter_handler();
        h.fire(Hook::HandlerExit);
        h.end_poll();
        h.end_tick();
    };
    update(Look);
    update(Look);
    update(Look);
    require(h.status() == std::string("on-foot input attached; a menu holds look"), "the menu hold was not taken");
    mag_controls_push_event({{InputSource::Keyboard, 44}, true, time_ns()});  // Space: Jump.
    h.begin_tick();  // the poll after the hold became stable
    require(!muted() && mag_input_capture_keyboard_owned(), "a menu holding look on foot did not get the pad");
    h.m.put32(Pad + 0x80, Look);
    h.enter_handler();
    require(h.recognize(Jump), "the player's handler lost its keys under the menu");
    h.fire(Hook::HandlerExit);
    h.enter_handler(menu);
    const auto before = h.tuple_at(record);
    require(!h.recognize_record(record, menu) && h.tuple_at(record) == before, "the adapter took over the menu's control");
    h.fire(Hook::HandlerExit);
    h.end_poll();
    h.end_tick();
    mag_controls_push_event({{InputSource::Keyboard, 44}, false, time_ns()});
    update(0);
    update(0);
    update(0);
    h.begin_tick();
    require(muted(), "look returning did not mute the pad again");
    h.end_poll();
    h.end_tick();
}

// The legacy keyboard pad (the SDK's fixed key layout: Cross on X, ...) is muted only while a poll
// attaches on foot. In an in-match menu with a local player (paused, the player's handler disabled
// as after death, a UI element that blocks gameplay) the adapter routes the system buttons alone,
// so the pad stays live for the menu's other buttons while the adapter keeps the keyboard (Escape).
void menu_keyboard_pad() {
    Harness h;
    const auto pad_muted = [] { return mag_input_capture_keyboard_pad_muted() != 0; };
    const auto owned = [] { return mag_input_capture_keyboard_owned() != 0; };
    require(!owned() && !pad_muted(), "nothing owned before the first poll");
    require(h.idle_tick() == Attached, "first tick did not attach on foot");
    require(owned() && pad_muted(), "on foot the adapter owns the keyboard and mutes the legacy pad");

    // Paused (the in-match options menu): menu buttons, keyboard still owned, pad live.
    h.m.put32(Pause + 0x9C0, 1);
    require(h.idle_tick() == "native menu buttons attached", "a paused game attached on foot");
    require(owned() && !pad_muted(), "an in-match menu muted the legacy keyboard pad");
    require(capture_on && !mouse_on, "an in-match menu keeps keyboard capture without the mouse lock");
    // The adapter still routes Escape (Start) to the menu's handler: system buttons keep working.
    mag_controls_push_event({{InputSource::Keyboard, 41}, true, time_ns()});
    h.begin_tick();
    h.enter_handler();
    require(h.recognize(Start) && mag_guest_tuple_value(h.tuple(Start)) == 1.0f, "Escape did not press Start in the menu");
    require(!h.recognize(Jump), "a gameplay control was replaced in a menu");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    mag_controls_push_event({{InputSource::Keyboard, 41}, false, time_ns()});
    h.m.put32(Pause + 0x9C0, 0);
    require(h.idle_tick() == Attached && pad_muted(), "unpausing did not mute the pad again");

    // The player's handler disabled (a dead player waiting to deploy): the same.
    h.m.put8(Handler + 0x40, 0);
    require(h.idle_tick() == "native menu buttons attached", "a disabled handler attached on foot");
    require(owned() && !pad_muted(), "a disabled handler muted the legacy keyboard pad");
    h.m.put8(Handler + 0x40, 1);
    require(h.idle_tick() == Attached && pad_muted(), "re-enabling did not mute the pad again");

    // Before the first spawn (the pre-battle lobby and the first deploy screen) the player's
    // movement state is the pre-spawn one: those screens are menus too, so X is the pad's Cross
    // there and Q/E, Z, V and the rest are the pad's buttons, not gameplay keys by channel (the
    // left mouse button pressing R1, R pressing Square...).
    h.m.put32(State, 0x01A6CE38);
    h.m.put8(State + 0x10, 0);
    require(h.idle_tick() == "native menu buttons attached", "the pre-battle lobby attached on foot");
    require(owned() && !pad_muted(), "the pre-battle lobby muted the legacy keyboard pad");
    require(capture_on && !mouse_on, "the pre-battle lobby took the mouse lock");
    mag_controls_push_event({{InputSource::Mouse, 0}, true, time_ns()});  // Fire's button.
    h.begin_tick();
    h.enter_handler();
    require(!h.recognize(Fire) && !h.recognize(Jump), "a gameplay key pressed a button in the pre-battle lobby");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    mag_controls_push_event({{InputSource::Mouse, 0}, false, time_ns()});
    // Only that exact state: id 0 under another vtable is not the pre-spawn state.
    h.m.put32(State, BasicStateVtable);
    require(h.idle_tick() == Attached && pad_muted(), "id 0 of another state was taken for the pre-battle lobby");
    // Spawning (a basic movement state) attaches on foot and mutes the pad again.
    h.m.put8(State + 0x10, 1);
    require(h.idle_tick() == Attached && pad_muted(), "spawning did not attach on foot");

    // After a death the player waits on the respawn screens (the respawn bar, the spawn map, the
    // loadout picker) in the awaiting-spawn movement state. They are menus too: X is the pad's Cross,
    // and nothing the player presses on Cross's channel is taken over (a controller's Cross, or the
    // pad's, reaches the respawn bar natively), even though the bar leaves gameplay routes and look.
    h.m.put32(State, 0x01A6C2F8);
    h.m.put8(State + 0x10, 20);
    require(h.idle_tick() == "native menu buttons attached", "the respawn screen attached on foot");
    require(owned() && !pad_muted(), "the respawn screen muted the legacy keyboard pad");
    require(capture_on && !mouse_on, "the respawn screen took the mouse lock");
    mag_controls_push_event({{InputSource::Keyboard, 44}, true, time_ns()});  // Space: Jump, on Cross's channel.
    h.begin_tick();
    h.enter_handler();
    require(!h.recognize(Jump), "a gameplay key was taken over on the respawn screen");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    mag_controls_push_event({{InputSource::Keyboard, 44}, false, time_ns()});
    // Escape still presses Start there (system buttons stay with the adapter).
    mag_controls_push_event({{InputSource::Keyboard, 41}, true, time_ns()});
    h.begin_tick();
    h.enter_handler();
    require(h.recognize(Start), "Escape did not press Start on the respawn screen");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    mag_controls_push_event({{InputSource::Keyboard, 41}, false, time_ns()});
    // Only that exact state: id 20 under another vtable is not the awaiting-spawn state.
    h.m.put32(State, BasicStateVtable);
    require(h.idle_tick() == Attached && pad_muted(), "id 20 of another state was taken for the respawn screen");
    // Respawning (a basic movement state) attaches on foot and mutes the pad again.
    h.m.put8(State + 0x10, 1);
    require(h.idle_tick() == Attached && pad_muted(), "respawning did not attach on foot");

    // A UI element of unknown type blocks gameplay: the same.
    ui_package(h.m, 1, true);
    ui_element(h.m, 0, 0x01A00000, 0x01A00040, true);
    require(h.idle_tick() == "native menu buttons attached", "an unknown UI element attached on foot");
    require(owned() && !pad_muted(), "an unknown UI element muted the legacy keyboard pad");
    h.m.put32(Packages + 0x40, 0);
    require(h.idle_tick() == Attached && pad_muted(), "closing the UI did not mute the pad again");

    // Without a local player nothing is owned and nothing muted (the front end's own menus).
    h.m.bytes.erase(ObjectManager + 0x44);
    h.begin_tick();
    require(!owned() && !pad_muted(), "no local player kept the keyboard");
    h.enter_handler();
    h.end_poll();
    h.end_tick();
    // Destroying the adapter releases everything.
    h.m.put32(ObjectManager + 0x44, Handle);
    h.idle_tick();
    require(owned() && pad_muted(), "the restored player was not owned");
    h.adapter.reset();
    require(!owned() && !pad_muted(), "destroying the adapter kept the keyboard");
}

// A watched control on `handler`'s list (appended), with its own binding to `channel`.
uint32_t watch(Memory& m, uint32_t handler, uint32_t slot, uint32_t token, uint32_t channel) {
    const uint32_t node = 0x100C0000 + slot * 0x100, binding = node + 0x80;
    m.map(node, 0x24);
    m.map(binding, 0x0C);
    m.put32(binding + 8, channel);
    m.put32(node + Record, token);
    m.put32(node + Record + 4, binding);
    m.put32(node + Record + 8, Tuning);
    std::vector<uint32_t> nodes;
    for (uint32_t n = m.get32(handler + 4); n != handler + 4; n = m.get32(n)) nodes.push_back(n);
    nodes.push_back(node);
    m.link(handler + 4, nodes);
    return node + Record;
}
constexpr uint32_t Bleedout = 0x40FEEC35, Confirm = 0x33E32A99, SeatNext = 0x74700E10;

// Situational actions (catalog revision 2) are bound by default but never required: the player's
// handler attaches on foot without them. Watched by their own token, they are recognized from their
// own keys, even on a channel another action uses, wherever the game watches them: the player's
// handler, a gameplay route or another registered handler.
void situational_actions() {
    Harness h;
    require(h.idle_tick() == Attached, "a default profile with situational actions did not attach on foot");
    // Downed: the player's handler also watches Bleedout, on Reload's channel (14).
    const uint32_t bleed = watch(h.m, Handler, 0, Bleedout, 14);
    require(h.idle_tick() == Attached, "a situational token sharing a channel blocked attachment");
    mag_controls_push_event({{InputSource::Keyboard, 40}, true, time_ns()});  // Return: Bleed out.
    h.begin_tick();
    h.enter_handler();
    require(h.recognize_record(bleed, Handler), "Bleedout stayed native");
    require(mag_guest_tuple_value(h.tuple_at(bleed)) == 1.0f, "Bleedout not pressed by Return");
    require(!h.recognize(Reload), "Return pressed Reload, which shares the channel");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    mag_controls_push_event({{InputSource::Keyboard, 40}, false, time_ns()});
    h.idle_tick();

    // A UI handler (a gameplay route) watching Confirm on Jump's channel on foot: neither Space (Jump)
    // nor X presses it. Confirm is a menu button, read from the keyboard pad in menus.
    uint32_t record = 0;
    const uint32_t route = route_handler(h.m, record);
    h.m.put32(record, Confirm);
    require(h.idle_tick() == Attached, "a prompt route blocked attachment");
    mag_controls_push_event({{InputSource::Keyboard, 44}, true, time_ns()});
    h.begin_tick();
    h.enter_handler();
    h.fire(Hook::HandlerExit);
    h.enter_handler(route);
    const auto before = h.tuple_at(record);
    require(!h.recognize_record(record, route) && h.tuple_at(record) == before, "Space pressed the prompt's Confirm");
    h.fire(Hook::HandlerExit);
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    mag_controls_push_event({{InputSource::Keyboard, 44}, false, time_ns()});
    mag_controls_push_event({{InputSource::Keyboard, 27}, true, time_ns()});  // X.
    h.begin_tick();
    h.enter_handler();
    h.fire(Hook::HandlerExit);
    h.enter_handler(route);
    require(!h.recognize_record(record, route) && h.tuple_at(record) == before, "X pressed Confirm on foot");
    h.fire(Hook::HandlerExit);
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    mag_controls_push_event({{InputSource::Keyboard, 27}, false, time_ns()});

    // The command screen is watched on the Select button (13): Tab presses it, as the controller's
    // Select does.
    const uint32_t command = watch(h.m, Handler, 2, 0xC6DCD89F, 13);
    require(h.idle_tick() == Attached, "the command screen token blocked attachment");
    mag_controls_push_event({{InputSource::Keyboard, 43}, true, time_ns()});  // Tab.
    h.begin_tick();
    h.enter_handler();
    require(h.recognize_record(command, Handler) && mag_guest_tuple_value(h.tuple_at(command)) == 1.0f,
            "Tab did not press the command screen");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    mag_controls_push_event({{InputSource::Keyboard, 43}, false, time_ns()});

    // Respawn is watched on the Start button (12) and scope zoom on the D-pad (0): Escape and Up
    // press them, as the controller's Start and D-pad do.
    const uint32_t respawn = watch(h.m, Handler, 3, 0xE88EB86F, 12), zoom = watch(h.m, Handler, 4, 0xF280DD78, 0);
    require(h.idle_tick() == Attached, "respawn and scope zoom tokens blocked attachment");
    mag_controls_push_event({{InputSource::Keyboard, 41}, true, time_ns()});  // Escape.
    mag_controls_push_event({{InputSource::Keyboard, 82}, true, time_ns()});  // Up.
    h.begin_tick();
    h.enter_handler();
    require(h.recognize_record(respawn, Handler) && mag_guest_tuple_value(h.tuple_at(respawn)) == 1.0f,
            "Escape did not press respawn");
    require(h.recognize_record(zoom, Handler) && mag_guest_tuple_value(h.tuple_at(zoom)) == 1.0f,
            "Up did not press scope zoom");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    mag_controls_push_event({{InputSource::Keyboard, 41}, false, time_ns()});
    mag_controls_push_event({{InputSource::Keyboard, 82}, false, time_ns()});

    // Another registered handler (a vehicle's, say) watching Change seats: V presses it while the
    // player is on foot, and the adapter gives it back when it lets go.
    const uint32_t vehicle = 0x100D0000;
    handler_base(h.m, vehicle, BaseVtable, true);
    const uint32_t seat = watch(h.m, vehicle, 1, SeatNext, 3);
    h.m.map(Registered + 0x20, 0x10);
    h.m.put32(Registered + 0x28, vehicle);
    h.m.link(InputManager + 0x2C, {Registered, Registered + 0x10, Registered + 0x20});
    require(h.idle_tick() == Attached, "a registered vehicle handler blocked attachment");
    mag_controls_push_event({{InputSource::Keyboard, 25}, true, time_ns()});  // V.
    h.begin_tick();
    h.enter_handler();
    h.fire(Hook::HandlerExit);
    h.enter_handler(vehicle);
    require(h.recognize_record(seat, vehicle) && mag_guest_tuple_value(h.tuple_at(seat)) == 1.0f,
            "V did not change seats");
    h.cpu.gpr[3] = Handler;
    h.fire(Hook::HandlerRefresh);  // Cancel: the held seat change is given back.
    h.fire(Hook::HandlerRefreshed);
    require(h.tuple_at(seat) == mag_guest_neutral_tuple(), "cancel kept the vehicle's seat change");
    h.fire(Hook::HandlerExit);
    h.fire(Hook::PollExit);
    h.enter_actor();
    h.end_tick();
    mag_controls_push_event({{InputSource::Keyboard, 25}, false, time_ns()});
    // In a menu a situational action is not taken over (the keyboard pad serves the menu).
    h.m.put32(Pause + 0x9C0, 1);
    require(h.idle_tick() == "native menu buttons attached", "paused game attached on foot");
    mag_controls_push_event({{InputSource::Keyboard, 25}, true, time_ns()});
    h.begin_tick();
    h.enter_handler(vehicle);
    require(!h.recognize_record(seat, vehicle), "a menu took over a situational action");
    h.fire(Hook::HandlerExit);
    h.fire(Hook::PollExit);
    h.end_tick();
}

// After a death the player is downed (movement state 7) until the respawn screens start, and the
// player's handler reads Bleed out on Cross's channel. On the console Cross bleeds out there; the
// keyboard's X is the key a player presses for Cross, but it is bound to Confirm, which the adapter
// never takes over, and the legacy pad is muted on foot: X did nothing, and players pressed X until
// the downed time ran out. While downed, Confirm's keys also press the player's own Bleed out;
// Return still does, and X presses nothing else (no closed menu's Confirm, no Jump).
void downed_x_bleeds_out() {
    Harness h;
    const auto pad_muted = [] { return mag_input_capture_keyboard_pad_muted() != 0; };
    const uint32_t bleed = watch(h.m, Handler, 0, Bleedout, 10);  // Cross's channel, with Jump.
    uint32_t record = 0;
    const uint32_t menu = route_handler(h.m, record);
    h.m.put32(record, Confirm);  // The closed pause menu's Confirm, still routed on foot.
    require(h.idle_tick() == Attached, "first tick did not attach on foot");
    const auto press = [&](uint32_t key, bool down) { mag_controls_push_event({{InputSource::Keyboard, key}, down, time_ns()}); };
    // One tick reading the player's Bleed out, Jump and the closed menu's Confirm.
    const auto tick = [&](const char* when) {
        h.begin_tick();
        h.enter_handler(menu);
        const auto before = h.tuple_at(record);
        require(!h.recognize_record(record, menu) && h.tuple_at(record) == before,
                (std::string("X pressed a closed menu's Confirm ") + when).c_str());
        h.fire(Hook::HandlerExit);
        h.enter_handler();
        require(!h.recognize(Jump), (std::string("X pressed Jump ") + when).c_str());
        const bool taken = h.recognize_record(bleed, Handler);
        const float value = mag_guest_tuple_value(h.tuple_at(bleed));
        h.end_poll();
        h.enter_actor();
        h.end_tick();
        return taken ? value : -1.0f;
    };

    // Alive (a basic movement state), X does not bleed out: Bleed out stays the game's.
    press(27, true);
    require(tick("alive") <= 0.0f, "X pressed Bleed out while alive");
    press(27, false);
    h.idle_tick();

    // Presses of X while alive are spent, never kept for later: going down does not bleed out.
    for (int i = 0; i < 3; ++i) {
        press(27, true);
        tick("alive, repeated");
        press(27, false);
        tick("alive, repeated, released");
    }
    // Downed: the player is still on foot (the pad stays muted), and X bleeds out.
    h.m.put32(State, 0x01A6D5B8);
    h.m.put8(State + 0x10, 7);
    require(h.idle_tick() == Attached && pad_muted(), "the downed state did not attach on foot");
    require(tick("downed, nothing pressed") <= 0.0f, "an X pressed while alive bled out when the player went down");
    press(27, true);
    require(tick("downed") == 1.0f, "X did not bleed out while downed");
    require(tick("downed, held") == 1.0f, "a held X let go of Bleed out");
    press(27, false);
    require(tick("downed, released") == 0.0f, "releasing X did not release Bleed out");
    h.idle_tick();
    // Return still bleeds out there.
    press(40, true);
    require(tick("downed, Return") == 1.0f, "Return did not bleed out while downed");
    press(40, false);
    require(tick("downed, Return released") == 0.0f, "releasing Return did not release Bleed out");
    h.idle_tick();
    // X again after Return: the alias takes the record back.
    press(27, true);
    require(tick("downed, X after Return") == 1.0f, "X did not bleed out after Return");
    press(27, false);
    tick("downed, X after Return released");
    h.idle_tick();

    // Only that exact state: id 7 under another vtable is not the downed state.
    h.m.put32(State, BasicStateVtable);
    h.idle_tick();
    press(27, true);
    require(tick("in id 7 of another state") <= 0.0f, "X bled out in id 7 of another state");
    press(27, false);
    h.idle_tick();
}

std::vector<std::string> census_lines;
// The closed pause menu stays routed on foot: its controller element watches Start (which opens it),
// Confirm and Back. X on foot pressed its Confirm, so the menu opened with the player card already
// open, and Kill Self did nothing until the card was closed. Confirm and Back are menu buttons: the
// adapter never takes them over, on foot or in a menu that holds look; menus read X and C from the
// keyboard pad. Escape still presses that element's Start.
void closed_menu_never_sees_confirm() {
    Harness h;
    uint32_t record = 0;
    const uint32_t menu = route_handler(h.m, record);
    h.m.put32(record, Confirm);
    const uint32_t back = watch(h.m, menu, 1, 0x75453127, 7);  // os_cancel
    const uint32_t start = watch(h.m, menu, 2, 0x2E8DE676, 12);  // the menu's Start
    require(h.idle_tick() == Attached, "first tick did not attach on foot");
    const auto confirm_before = h.tuple_at(record), back_before = h.tuple_at(back);
    mag_controls_push_event({{InputSource::Keyboard, 27}, true, time_ns()});  // X.
    mag_controls_push_event({{InputSource::Keyboard, 42}, true, time_ns()});  // Backspace.
    h.begin_tick();
    h.enter_handler(menu);
    require(!h.recognize_record(record, menu) && h.tuple_at(record) == confirm_before,
            "X on foot pressed a closed menu's Confirm");
    require(!h.recognize_record(back, menu) && h.tuple_at(back) == back_before,
            "Backspace on foot pressed a closed menu's Back");
    h.fire(Hook::HandlerExit);
    h.enter_handler();
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    mag_controls_push_event({{InputSource::Keyboard, 27}, false, time_ns()});
    mag_controls_push_event({{InputSource::Keyboard, 42}, false, time_ns()});
    h.idle_tick();
    mag_controls_push_event({{InputSource::Keyboard, 41}, true, time_ns()});  // Escape.
    h.begin_tick();
    h.enter_handler(menu);
    require(h.recognize_record(start, menu) && mag_guest_tuple_value(h.tuple_at(start)) == 1.0f,
            "Escape did not press the menu's Start");
    h.fire(Hook::HandlerExit);
    h.enter_handler();
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    mag_controls_push_event({{InputSource::Keyboard, 41}, false, time_ns()});
}

void census_line(const char* line) { census_lines.push_back(line); }

// The census describes each distinct handler update once, and changes nothing.
void census() {
    Harness h;
    h.adapter->set_census(census_line);
    h.idle_tick();
    h.idle_tick();
    require(h.status() == Attached, "the census changed attachment");
    require(census_lines.size() == 1, "each distinct handler description is reported once");
    require(census_lines[0].find("census role=owner context=OnFoot enabled=1 state=1/01A6DD28 controls=B164D156:4,93115EC5:5,") == 0,
            "owner description");
    require(census_lines[0].find("C34F3A66:23,163AAB24:12,") != std::string::npos, "every watched token is listed");
    h.m.put32(Pause + 0x9C0, 1);
    h.idle_tick();
    require(census_lines.size() == 2 && census_lines[1].find("role=owner context=Menu") != std::string::npos,
            "a new context is a new description");
    h.m.put32(Pause + 0x9C0, 0);
    h.adapter->set_census(nullptr);
    h.m.put8(Handler + 0x40, 0);
    h.idle_tick();
    require(census_lines.size() == 2, "a removed sink still reported");
}

// The query API for on-screen prompts: bindings by action and context, the last device used and
// the profile revision.
void prompt_queries() {
    Harness h;
    const uint64_t revision = mag_controls_profile_revision();
    const auto bleed = mag_controls_action_bindings(Bleedout, InputContext::OnFoot);
    require(bleed.size() == 1 && bleed[0].control == PhysicalControl{InputSource::Keyboard, 40}, "Bleed out is Return");
    require(mag_controls_action_bindings(Bleedout, InputContext::Menu).empty(), "Bleed out bound in menus");
    const auto start = mag_controls_action_bindings(Start, InputContext::Menu);
    require(start.size() == 1 && start[0].control.code == 41, "controller bindings listed without a remap");
    require(!mag_controls_controller_remapped(), "default profile remaps the controller");
    auto remap = mag_game_controls_default_profile();
    remap.controller_remap = true;
    mag_controls_configure(remap);
    require(mag_controls_profile_revision() > revision, "configure did not advance the revision");
    require(mag_controls_action_bindings(Start, InputContext::Menu).size() == 2 && mag_controls_controller_remapped(),
            "remapped controller bindings not listed");
    require(mag_controls_last_device() == InputDevice::None, "a device before any press");
    mag_controls_push_event({{InputSource::Mouse, 0}, true, time_ns()});
    require(mag_controls_last_device() == InputDevice::KeyboardMouse, "a click is keyboard and mouse");
    mag_controls_push_event({{InputSource::Mouse, 0}, false, time_ns()});
    mag_controls_push_event({{InputSource::Controller, 0}, true, time_ns(), 1});
    require(mag_controls_last_device() == InputDevice::Controller, "a button press is the controller");
    mag_controls_push_event({{InputSource::Keyboard, 4}, false, time_ns()});
    require(mag_controls_last_device() == InputDevice::Controller, "a release changed the device");
}

// 7.4: during a handler update, gameplay availability is the one read when the update began; a
// pause that starts inside the update is seen at the next handler update, and outside handler
// updates (the actor update) it is read anew.
void update_snapshot() {
    Harness h;
    h.idle_tick();
    mag_controls_push_event({{InputSource::Keyboard, 44}, true, time_ns()});
    h.begin_tick();
    h.enter_handler();
    h.m.put32(Pause + 0x9C0, 1);  // the game pauses while the owner's handler updates
    require(h.recognize(Jump), "a pause inside the update revoked gameplay before the next update");
    h.fire(Hook::HandlerExit);
    h.enter_handler();
    require(!h.recognize(Jump), "the next handler update kept gameplay while paused");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    // Gameplay again; a pause after the poll is caught by the actor update itself.
    h.m.put32(Pause + 0x9C0, 0);
    h.idle_tick();
    h.begin_tick();
    h.enter_handler();
    h.end_poll();
    h.m.put32(Pause + 0x9C0, 1);
    h.enter_actor();
    require(h.status() == std::string("native control map changed; waiting for fresh input"),
            "the actor update used a handler update's gameplay snapshot");
    h.end_tick();
}

// Another thread (the window's) may withdraw the attached frame at any moment: a focus or mouse
// capture change starts a new capture generation. Mid-recognition that is not an error: the game
// recognizes the control, nothing is published, and the next poll attaches as usual.
void withdrawn_frame() {
    Harness h;
    require(h.idle_tick() == Attached, "first tick did not attach on foot");
    mag_controls_push_event({{InputSource::Keyboard, 44}, true, time_ns()});  // Space: Jump.
    h.begin_tick();
    h.enter_handler();
    const auto before = h.tuple(Jump);
    static bool withdrawn;
    withdrawn = false;
    h.m.on_read = [](uint32_t address) {
        // While the adapter reads the tuning record (after its frame check), the window loses focus.
        if (!withdrawn && address == Tuning + 0x18) {
            withdrawn = true;
            mag_controls_set_window_capture(false, false);
        }
    };
    require(!h.recognize(Jump), "a withdrawn frame was published");
    require(withdrawn, "the withdrawal did not happen mid-recognition");
    h.m.on_read = nullptr;
    require(h.status().find("rejected") == std::string::npos, "a withdrawn frame was treated as an error");
    require(h.tuple(Jump) == before && h.cpu.gpr[11] == 0xDEAD, "a withdrawn frame changed the control or registers");
    require(!h.recognize(Jump), "the rest of the poll was not left to the game");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    mag_controls_set_window_capture(true, false);
    require(h.idle_tick() == Attached, "the next poll did not attach");
    require(capture_on, "desktop capture was dropped");
}

// A mounted weapon (a bunker turret) has its own handler, registered with the input manager like the
// player's: it watches the right stick's four look channels and the fire buttons with its own tokens,
// and the game updates it before the player's handler, so it consumes the look channels first. The
// soldier's look consumers do not run in the seat's movement state. Players found that in a bunker
// turret the mouse did not aim and the left button did not fire on keyboard and mouse: the adapter
// left the turret's controls native and read the turret's hold on look as a menu's.
constexpr uint32_t SeatState = 0x01A6C938;  // a vehicle seat's movement state: no soldier aim seams
struct Seat {
    uint32_t handler, fire, rockets, left, right, up, down;
};
// A registered seat handler with tokens of its own: fire on Fire's button (8), rockets on Cycle weapons'
// (17, R2 in this world) and the four look directions on the look channels.
Seat add_seat(Memory& m) {
    const uint32_t handler = 0x100D0000, vtable = 0x01A70000;
    handler_base(m, handler, vtable, true);
    m.put32(vtable + 8, BaseDescriptor);
    Seat seat{handler, watch(m, handler, 1, 0x0B00F001, 8), watch(m, handler, 2, 0x0B00F002, 17),
              watch(m, handler, 3, 0x0B00F003, 20), watch(m, handler, 4, 0x0B00F004, 21),
              watch(m, handler, 5, 0x0B00F005, 22), watch(m, handler, 6, 0x0B00F006, 23)};
    m.map(Registered + 0x20, 0x10);
    m.put32(Registered + 0x28, handler);
    m.link(InputManager + 0x2C, {Registered, Registered + 0x10, Registered + 0x20});
    return seat;
}
void seat_state(Memory& m) {
    m.put32(State, SeatState);
    m.put8(State + 0x10, 14);
}
constexpr uint32_t Look = 0xFu << 20;  // the look channels, 20..23, as the pad record's consumed mask

void mounted_weapon() {
    Harness h;
    const Seat seat = add_seat(h.m);
    seat_state(h.m);
    require(h.idle_tick() == Attached, "a seat did not attach on foot");
    mag_controls_set_window_capture(true, true);
    const auto value = [&](uint32_t record) { return mag_guest_tuple_value(h.tuple_at(record)); };
    struct Read {
        bool fire_taken, aim_taken;
        float fire, rockets, left, right, up, down, owner_fire;
    };
    // One poll as the game runs it: the seat's handler first (it consumes the look channels), then
    // the player's.
    const auto poll = [&]() {
        Read out{};
        h.m.put32(Pad + 0x80, 0);
        h.begin_tick();
        h.enter_handler(seat.handler);
        out.fire_taken = h.recognize_record(seat.fire, seat.handler);
        h.recognize_record(seat.rockets, seat.handler);
        for (uint32_t record : {seat.left, seat.right, seat.up, seat.down}) h.recognize_record(record, seat.handler);
        h.m.put32(Pad + 0x80, Look);
        h.fire(Hook::HandlerExit);
        h.enter_handler();
        h.recognize(Fire);
        out.aim_taken = h.recognize(AimAlias);
        out.fire = value(seat.fire), out.rockets = value(seat.rockets), out.left = value(seat.left);
        out.right = value(seat.right), out.up = value(seat.up), out.down = value(seat.down);
        out.owner_fire = mag_guest_tuple_value(h.tuple(Fire));
        h.end_poll();
        h.enter_actor();
        h.end_tick();
        return out;
    };

    // The left mouse button fires the seat's own weapon (and the player's Fire, as R1 would).
    mag_controls_push_event({{InputSource::Mouse, 0}, true, time_ns()});
    auto read = poll();
    require(read.fire_taken && read.fire == 1.0f, "the left mouse button did not fire the turret");
    require(read.owner_fire == 1.0f, "the player's Fire stopped following the left mouse button");
    mag_controls_push_event({{InputSource::Mouse, 0}, false, time_ns()});
    read = poll();
    require(read.fire == 0.0f, "the turret kept firing after the release");
    // Its hold on the look channels is the seat's, not a menu's: the mouse stays locked.
    for (int i = 0; i < 4; ++i) read = poll();
    require(capture_on && mouse_on, "the seat's look channels released the mouse lock");
    require(h.status() == std::string(Attached), "the seat was reported as a menu holding look");

    // The mouse drives the right stick: fast to the right pushes it fully, slowly up part way.
    mag_controls_push_relative_mouse(1000, 0);
    read = poll();
    require(read.right == 1.0f && read.left == 0.0f && read.up == 0.0f && read.down == 0.0f,
            "fast mouse motion to the right did not push the turret's stick right");
    mag_controls_push_relative_mouse(0, -5);  // counts are down-positive
    read = poll();
    require(read.up > 0.0f && read.up < 1.0f && read.down == 0.0f && read.right == 0.0f,
            "slow upward motion did not push the turret's stick part way up");
    read = poll();
    require(read.up == 0.0f && read.right == 0.0f, "a mouse at rest did not centre the stick");
    mag_controls_push_relative_mouse(-1000, 0);
    read = poll();
    require(read.left == 1.0f && read.right == 0.0f, "leftward motion did not push the stick left");

    // The right mouse button fires the secondary weapon (R2) and is no Aim press in the seat; the
    // key bound to R2's action fires it too.
    mag_controls_push_event({{InputSource::Mouse, 1}, true, time_ns()});
    read = poll();
    require(read.rockets == 1.0f, "the right mouse button did not fire the turret's secondary weapon");
    require(!read.aim_taken, "the right mouse button pressed the player's Aim in a seat");
    mag_controls_push_event({{InputSource::Mouse, 1}, false, time_ns()});
    read = poll();
    require(read.rockets == 0.0f, "the secondary weapon kept firing after the release");
    mag_controls_push_event({{InputSource::Keyboard, 20}, true, time_ns()});  // Q: Cycle weapons
    read = poll();
    require(read.rockets == 1.0f, "Cycle weapons' key did not fire the secondary weapon");
    mag_controls_push_event({{InputSource::Keyboard, 20}, false, time_ns()});
    poll();

    // A cancel gives the held stick and buttons back.
    mag_controls_push_relative_mouse(1000, 0);
    h.m.put32(Pad + 0x80, 0);
    h.begin_tick();
    h.enter_handler(seat.handler);
    h.recognize_record(seat.right, seat.handler);
    require(value(seat.right) == 1.0f, "stick not pushed before the cancel");
    h.cpu.gpr[3] = Handler;
    h.fire(Hook::HandlerRefresh);
    h.fire(Hook::HandlerRefreshed);
    require(h.tuple_at(seat.right) == mag_guest_neutral_tuple(), "a cancel kept the turret's stick pushed");
    h.fire(Hook::HandlerExit);
    h.fire(Hook::PollExit);
    h.end_tick();

    // A menu that takes the look channels before the seat still holds look: the seat consumed
    // nothing of its own, so the lock follows the menu.
    for (int i = 0; i < 4; ++i) {
        h.m.put32(Pad + 0x80, Look);
        h.begin_tick();
        h.enter_handler(seat.handler);
        h.fire(Hook::HandlerExit);
        h.enter_handler();
        h.end_poll();
        h.enter_actor();
        h.end_tick();
    }
    require(capture_on && !mouse_on, "a menu holding look before the seat did not release the mouse");
    // A paused game is a menu: nothing is taken over.
    h.m.put32(Pad + 0x80, 0);
    h.m.put32(Pause + 0x9C0, 1);
    require(h.idle_tick() == "native menu buttons attached", "paused game attached on foot");
    mag_controls_push_event({{InputSource::Mouse, 0}, true, time_ns()});
    h.begin_tick();
    h.enter_handler(seat.handler);
    require(!h.recognize_record(seat.fire, seat.handler), "a menu let the left mouse button fire the turret");
    h.fire(Hook::HandlerExit);
    h.fire(Hook::PollExit);
    h.end_tick();
    mag_controls_push_event({{InputSource::Mouse, 0}, false, time_ns()});
}

// On foot (a soldier aim state) the mouse aims the soldier and the right button aims: no stick, and
// a registered handler reading the fire buttons gets the left button as R1 would press it.
void registered_handler_on_foot() {
    Harness h;
    const Seat seat = add_seat(h.m);
    require(h.idle_tick() == Attached, "a registered handler blocked attachment");
    mag_controls_set_window_capture(true, true);
    mag_controls_push_event({{InputSource::Mouse, 0}, true, time_ns()});
    mag_controls_push_event({{InputSource::Mouse, 1}, true, time_ns()});
    mag_controls_push_relative_mouse(1000, 0);
    h.begin_tick();
    h.enter_handler(seat.handler);
    require(h.recognize_record(seat.fire, seat.handler) && mag_guest_tuple_value(h.tuple_at(seat.fire)) == 1.0f,
            "the left mouse button did not press a registered handler's R1");
    require(!h.recognize_record(seat.right, seat.handler), "the mouse pushed the stick in a soldier aim state");
    h.fire(Hook::HandlerExit);
    h.enter_handler();
    require(h.recognize(AimAlias) && mag_guest_tuple_value(h.tuple(AimAlias)) == 1.0f, "the right button stopped aiming on foot");
    h.end_poll();
    h.enter_actor();
    h.end_tick();
    mag_controls_push_event({{InputSource::Mouse, 0}, false, time_ns()});
    mag_controls_push_event({{InputSource::Mouse, 1}, false, time_ns()});
}

// Controller mode leaves a seat to the controller: the mouse pushes no stick.
void mounted_weapon_controller_mode() {
    ControlsProfile profile = mag_game_controls_default_profile();
    profile.mode = InputMode::Controller;
    Harness h(profile);
    const Seat seat = add_seat(h.m);
    seat_state(h.m);
    require(h.idle_tick() == Attached, "a seat did not attach on foot");
    mag_controls_set_window_capture(true, true);
    mag_controls_push_relative_mouse(1000, 0);
    h.begin_tick();
    h.enter_handler(seat.handler);
    require(!h.recognize_record(seat.right, seat.handler), "Controller mode let the mouse push the stick");
    h.fire(Hook::HandlerExit);
    h.end_poll();
    h.enter_actor();
    h.end_tick();
}

int main(int argc, char** argv) {
    const std::map<std::string, void (*)()> scenarios = {
        {"owner", owner_resolution}, {"lists", malformed_lists}, {"routes", routes}, {"tick", full_tick},
        {"route", route_recognition},
        {"mouse", mouse_look},       {"assist", aim_assist},     {"lifecycle", lifecycle},
        {"threads", threads_and_errors}, {"snapshot", update_snapshot}, {"menu", menu_holds_look},
        {"withdrawn", withdrawn_frame},
        {"menu-pad", menu_keyboard_pad}, {"menu-look-pad", menu_holding_look_gets_the_pad}, {"situational", situational_actions}, {"closed-menu-confirm", closed_menu_never_sees_confirm}, {"downed-x", downed_x_bleeds_out}, {"census", census},
        {"prompt-queries", prompt_queries}, {"mounted", mounted_weapon},
        {"registered-on-foot", registered_handler_on_foot}, {"mounted-controller-mode", mounted_weapon_controller_mode}};
    try {
        if (argc != 2 || !scenarios.count(argv[1])) throw std::runtime_error("usage: game_controls_adapter <scenario>");
        scenarios.at(argv[1])();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << argv[0] << ": " << error.what() << '\n';
        return 1;
    }
}
