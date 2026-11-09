// Synthetic checks of the game controls adapter (docs/native-controls.md) against the real Rust
// input engine. The guest is a fake big-endian memory holding only the records of section 2; no
// game file or lifted code is used. Each scenario runs in its own process (the engine is global).
#include "mag_game_controls_adapter.h"
#include "mag_game_controls_catalog.h"
#include "mag_game_controls_guest.h"
#include "player_controls_rust.h"
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

int capture_requests = 0, mouse_requests = 0;
bool capture_on = false, mouse_on = false;
void on_capture(bool on) {
    ++capture_requests;
    capture_on = on;
}
void on_mouse(bool on) {
    ++mouse_requests;
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
    require(h.m.getf(Actor + 0x184) == 0.0f && h.m.getf(Actor + 0x18C) == 0.0f, "look-rate inputs not cleared");
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
    require(h.status() == Invalid, "invalid tuning did not cancel");
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

int main(int argc, char** argv) {
    const std::map<std::string, void (*)()> scenarios = {
        {"owner", owner_resolution}, {"lists", malformed_lists}, {"routes", routes}, {"tick", full_tick},
        {"route", route_recognition},
        {"mouse", mouse_look},       {"assist", aim_assist},     {"lifecycle", lifecycle},
        {"threads", threads_and_errors}};
    try {
        if (argc != 2 || !scenarios.count(argv[1])) throw std::runtime_error("usage: game_controls_adapter <scenario>");
        scenarios.at(argv[1])();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << argv[0] << ": " << error.what() << '\n';
        return 1;
    }
}
