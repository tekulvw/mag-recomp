#include "mag_game_controls_guest.h"
#include "mag_game_controls_catalog.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <set>
using namespace mag_guest;

namespace {
constexpr unsigned MaxListNodes = 256, MaxPackages = 256, MaxElementRecords = 2048;
constexpr uint32_t ElementRecordStride = 0x28;

constexpr uint32_t ActorVtables[] = {0x01A48888, 0x01A6AF98};
constexpr uint32_t ControllerVtables[] = {0x01A6AC30, 0x01A6AC98};
constexpr uint32_t LogicVtable = 0x01A6AE18;

struct MouseState {
    uint8_t id;
    uint32_t vtable;
    MagMouseAimState aim;
};
constexpr MouseState MouseStates[] = {
    {1, 0x01A6DD28, MagMouseAimState::Basic}, {11, 0x01A6D1F8, MagMouseAimState::Basic},
    {18, 0x01A6DB48, MagMouseAimState::Prone}, {23, 0x01A6BDF8, MagMouseAimState::Sprint}};

enum class ElementKind { Plain, Scripted };
struct ElementType {
    uint32_t primary, secondary;
    ElementKind kind;
};
constexpr ElementType ElementTypes[] = {
    {0x01A49828, 0x01A49864, ElementKind::Plain}, {0x01A49CD8, 0x01A49D14, ElementKind::Plain},
    {0x01A4A218, 0x01A4A254, ElementKind::Plain}, {0x01A4A340, 0x01A4A380, ElementKind::Plain},
    {0x01A4A520, 0x01A4A55C, ElementKind::Plain}, {0x01A64ED8, 0x01A64F14, ElementKind::Plain},
    {0x01A64F88, 0x01A64FC4, ElementKind::Plain}, {0x01A650B0, 0x01A650EC, ElementKind::Plain},
    {0x01A4FCB8, 0x01A4FCF8, ElementKind::Scripted}, {0x01A64E28, 0x01A64E68, ElementKind::Scripted},
    {0x01A64D38, 0x01A64D78, ElementKind::Scripted}};
constexpr uint32_t ScriptedCallbackDescriptor = 0x01AE28D8, ScriptedCallbackEntry = 0x00F2AD8C;
constexpr uint32_t UiHandlerVtable = 0x01A4EAB8, UiUpdateDescriptor = 0x01AE1868, UiUpdateEntry = 0x00F15B14;

template <class T, size_t N>
bool contains(const T (&values)[N], T value) {
    return std::find(std::begin(values), std::end(values), value) != std::end(values);
}

// Nodes of a doubly linked list at `head`, in order, or nullopt when the list is not well formed
// (section 1): every node pointer-like and distinct, every prev link pointing at its predecessor,
// the head's prev pointing at the last node, and the walk back at the head within 256 nodes.
std::optional<std::vector<uint32_t>> list_nodes(MagGuestReads& r, uint32_t head) {
    const auto first = r.u32(head), last = r.u32(uint64_t(head) + 4);
    if (!first || !last) return {};
    std::vector<uint32_t> nodes;
    std::set<uint32_t> seen;
    uint32_t previous = head, node = *first;
    while (node != head) {
        if (nodes.size() >= MaxListNodes || !mag_guest_pointer_like(node) || !seen.insert(node).second) return {};
        const auto back = r.u32(uint64_t(node) + 4), next = r.u32(node);
        if (!back || *back != previous || !next) return {};
        nodes.push_back(node);
        previous = node;
        node = *next;
    }
    if (*last != previous) return {};
    return nodes;
}

// A descriptor pointer stored at `slot` leads to the (entry, table of contents) pair given.
bool descriptor_is(MagGuestReads& r, uint64_t slot, uint32_t entry, uint32_t toc, uint32_t location = 0) {
    const auto descriptor = r.u32(slot);
    if (!descriptor || !mag_guest_pointer_like(*descriptor) || (location && *descriptor != location)) return false;
    const auto e = r.u32(*descriptor), t = r.u32(uint64_t(*descriptor) + 4);
    return e && t && *e == entry && *t == toc;
}

bool base_update(MagGuestReads& r, uint32_t handler) {
    const auto vtable = r.u32(handler);
    return vtable && mag_guest_pointer_like(*vtable) && descriptor_is(r, uint64_t(*vtable) + 8, BaseUpdateEntry, SharedToc);
}

std::optional<std::vector<MagGuestControl>> watched_controls(MagGuestReads& r, uint32_t handler) {
    if (!mag_guest_pointer_like(handler) || handler > 0xFFFFFFEF) return {};
    const auto nodes = list_nodes(r, handler + HandlerList);
    if (!nodes) return {};
    std::vector<MagGuestControl> controls;
    for (uint32_t node : *nodes) {
        const uint64_t record = uint64_t(node) + 8;
        const auto token = r.u32(record + RecordToken), binding = r.u32(record + RecordBinding);
        if (!token || !binding) return {};
        if (!*binding) continue;  // Unmapped control.
        if (!mag_guest_pointer_like(*binding)) return {};
        const auto channel = r.u32(uint64_t(*binding) + 8);
        if (!channel || *channel >= 32) return {};
        controls.push_back({*token, *channel, *binding, uint32_t(record)});
    }
    return controls;
}

std::optional<unsigned> registration_count(MagGuestReads& r, uint32_t handler) {
    const auto nodes = list_nodes(r, InputManager + 0x2C);
    if (!nodes) return {};
    unsigned count = 0;
    for (uint32_t node : *nodes) {
        const auto registered = r.u32(uint64_t(node) + 8);
        if (!registered) return {};
        count += *registered == handler;
    }
    return count;
}

// The local actor's handle and pool entry: (index<<16|generation, actor).
struct LocalActor {
    uint32_t handle, actor;
};
std::optional<LocalActor> local_actor(MagGuestReads& r) {
    const auto handle = r.u32(ObjectManager + 0x44);
    if (!handle || !(*handle >> 16) || !(*handle & 0xFFFF)) return {};
    const auto pool = r.u32(ActorPoolPointer);
    if (!pool || !mag_guest_pointer_like(*pool)) return {};
    const auto actor = r.u32(uint64_t(*pool) + 0x28 + 4ull * (*handle >> 16));
    if (!actor || !mag_guest_pointer_like(*actor)) return {};
    return LocalActor{*handle, *actor};
}

template <class T>
bool equals(const std::optional<T>& value, T expected) {
    return value && *value == expected;
}

std::optional<MagGuestOwner> resolve_owner(MagGuestReads& r, uint32_t handler, bool allow_disabled) {
    if (!mag_guest_pointer_like(handler) || handler < 0x1000C) return {};
    MagGuestOwner owner;
    owner.handler = handler;
    owner.controller = handler - ControllerHandler;
    const auto local = local_actor(r);
    if (!local) return {};
    owner.index = local->handle >> 16;
    owner.generation = local->handle & 0xFFFF;
    owner.actor = local->actor;
    const uint64_t actor = owner.actor;
    if (!equals(r.u32(actor + 4), local->handle)) return {};

    const auto actor_vtable = r.u32(actor), controller_vtable = r.u32(owner.controller);
    if (!actor_vtable || !contains(ActorVtables, *actor_vtable) || !controller_vtable ||
        !contains(ControllerVtables, *controller_vtable))
        return {};
    if (!equals(r.u32(uint64_t(owner.controller) + 4), owner.actor) ||
        !equals(r.u32(actor + ActorController), owner.controller) || !equals(r.u32(actor + ActorRemote), 0u) ||
        !equals(r.u32(handler), BaseHandlerVtable))
        return {};

    const auto logic = r.u32(actor + ActorLogic), state = r.u32(actor + ActorState);
    if (!logic || !state || !mag_guest_pointer_like(*logic) || !mag_guest_pointer_like(*state)) return {};
    owner.logic = *logic;
    owner.state = *state;
    if (!equals(r.u32(owner.logic), LogicVtable) || !equals(r.u32(uint64_t(owner.logic) + 4), owner.actor)) return {};
    const auto enabled = r.u8(uint64_t(handler) + HandlerEnabled);
    if (!enabled) return {};
    owner.enabled = *enabled != 0;
    if (!owner.enabled && !allow_disabled) return {};
    if (!equals(r.u32(uint64_t(handler) + HandlerPriority), 0u) || !equals(r.u32(uint64_t(handler) + HandlerSlot), 0u))
        return {};
    if (!equals(registration_count(r, handler), 1u)) return {};

    const auto state_id = r.u8(uint64_t(owner.state) + 0x10);
    const auto state_vtable = r.u32(owner.state);
    if (!state_id || !state_vtable || !mag_guest_pointer_like(*state_vtable) ||
        !equals(r.u32(uint64_t(owner.state) + 4), owner.actor))
        return {};
    owner.state_id = *state_id;
    for (const auto& supported : MouseStates)
        if (supported.id == *state_id && supported.vtable == *state_vtable) {
            owner.mouse_supported = true;
            owner.aim = supported.aim;
        }
    return owner;
}

// Visits the element of every active record in every active UI package (5.4 and 5.5 bounds).
// Returns false when the package or element vectors are malformed or `visit` rejects an element.
template <class Visit>
bool visit_active_elements(MagGuestReads& r, uint32_t max_element, Visit visit) {
    const auto begin = r.u32(UiManager + 0x88), end = r.u32(UiManager + 0x8C);
    if (!begin || !end || *begin > *end) return false;
    const uint32_t size = *end - *begin;
    if (size % 4 || size / 4 > MaxPackages || (size && !mag_guest_pointer_like(*begin))) return false;
    unsigned records = 0;
    for (uint32_t i = 0; i < size / 4; ++i) {
        const auto package = r.u32(uint64_t(*begin) + 4ull * i);
        if (!package || !mag_guest_pointer_like(*package)) return false;
        const auto flags = r.u32(uint64_t(*package) + 0x40);
        if (!flags) return false;
        if (!(*flags & 2)) continue;
        const auto first = r.u32(uint64_t(*package) + 4), past = r.u32(uint64_t(*package) + 8);
        if (!first || !past || *first > *past) return false;
        const uint32_t bytes = *past - *first;
        if (bytes % ElementRecordStride || (bytes && !mag_guest_pointer_like(*first))) return false;
        records += bytes / ElementRecordStride;
        if (records > MaxElementRecords) return false;
        for (uint32_t offset = 0; offset < bytes; offset += ElementRecordStride) {
            const auto element = r.u32(uint64_t(*first) + offset + 0x24);
            if (!element) return false;
            if (!*element) continue;  // Empty slot.
            if (!mag_guest_pointer_like(*element) || *element > max_element) return false;
            const auto properties = r.u32(uint64_t(*element) + 0x50);
            if (!properties || !mag_guest_pointer_like(*properties)) return false;
            const auto active = r.u32(uint64_t(*properties) + 0x40);
            if (!active) return false;
            if (!(*active & 2)) continue;
            if (!visit(*element)) return false;
        }
    }
    return true;
}

const ElementType* element_type(uint32_t primary, uint32_t secondary) {
    for (const auto& type : ElementTypes)
        if (type.primary == primary && type.secondary == secondary) return &type;
    return nullptr;
}

// One active UI element under the gameplay rules of 5.4. Appends a scripted element's handler.
bool gameplay_element(MagGuestReads& r, uint32_t element, std::vector<uint32_t>& routes) {
    const auto primary = r.u32(element), secondary = r.u32(uint64_t(element) + 4);
    if (!primary || !secondary) return false;
    const auto* type = element_type(*primary, *secondary);
    if (!type || !descriptor_is(r, uint64_t(*secondary) + 8, BaseUpdateEntry, SharedToc)) return false;
    const uint64_t handler = uint64_t(element) + 4;
    const auto enabled = r.u8(handler + HandlerEnabled);
    if (!enabled) return false;
    if (!*enabled) return true;
    const auto fixed = r.u32(handler + HandlerFixedMask), mapped = r.u32(handler + HandlerMappedMask);
    if (!fixed || !mapped) return false;
    if (type->kind == ElementKind::Scripted) {
        if (!descriptor_is(r, uint64_t(*secondary) + 12, ScriptedCallbackEntry, SharedToc, ScriptedCallbackDescriptor) ||
            !list_nodes(r, uint32_t(handler + HandlerList)))
            return false;
        routes.push_back(uint32_t(handler));
        return true;
    }
    const uint64_t head = handler + HandlerList;
    return equals(r.u32(head), uint32_t(head)) && equals(r.u32(head + 4), uint32_t(head)) && !*fixed && !*mapped;
}

// 5.5 base route: accepted when disabled, listed once when enabled with readable list and masks.
bool base_route(MagGuestReads& r, uint32_t handler, std::vector<uint32_t>& listed) {
    if (!mag_guest_pointer_like(handler) || handler > 0xFFFFFFB3 || !base_update(r, handler)) return false;
    const auto enabled = r.u8(uint64_t(handler) + HandlerEnabled);
    if (!enabled) return false;
    if (!*enabled) return true;
    if (!list_nodes(r, handler + HandlerList) || !r.u32(uint64_t(handler) + HandlerFixedMask) ||
        !r.u32(uint64_t(handler) + HandlerMappedMask))
        return false;
    if (std::find(listed.begin(), listed.end(), handler) != listed.end()) return false;
    listed.push_back(handler);
    return true;
}
}  // namespace

bool MagGuestReads::bytes(uint64_t address, void* output, size_t size) {
    if (!remaining_ || !memory_.read || !size || address > 0xFFFFFFFFull || size > 0x100000000ull - address)
        return false;
    --remaining_;
    return memory_.read(memory_.user, uint32_t(address), output, size);
}
std::optional<uint8_t> MagGuestReads::u8(uint64_t address) {
    uint8_t value;
    if (!bytes(address, &value, 1)) return {};
    return value;
}
std::optional<uint16_t> MagGuestReads::u16(uint64_t address) {
    uint8_t b[2];
    if (!bytes(address, b, 2)) return {};
    return uint16_t(b[0] << 8 | b[1]);
}
std::optional<uint32_t> MagGuestReads::u32(uint64_t address) {
    uint8_t b[4];
    if (!bytes(address, b, 4)) return {};
    return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
}
std::optional<float> MagGuestReads::f32(uint64_t address) {
    const auto word = u32(address);
    if (!word) return {};
    return std::bit_cast<float>(*word);
}
bool MagGuestReads::write_bytes(uint64_t address, const void* input, size_t size) {
    if (!memory_.write || !size || address > 0xFFFFFFFFull || size > 0x100000000ull - address) return false;
    return memory_.write(memory_.user, uint32_t(address), input, size);
}
bool MagGuestReads::write_u32(uint64_t address, uint32_t value) {
    const uint8_t b[4] = {uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
    return write_bytes(address, b, 4);
}
bool MagGuestReads::write_f32(uint64_t address, float value) {
    return write_u32(address, std::bit_cast<uint32_t>(value));
}

bool mag_guest_same_owner(const MagGuestOwner& a, const MagGuestOwner& b) {
    return a.actor == b.actor && a.index == b.index && a.generation == b.generation &&
           a.controller == b.controller && a.logic == b.logic && a.handler == b.handler;
}

namespace {
void put_word(MagGuestTuple& tuple, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) tuple[at + i] = uint8_t(value >> (24 - 8 * i));
}
uint32_t get_word(const MagGuestTuple& tuple, size_t at) {
    return uint32_t(tuple[at]) << 24 | uint32_t(tuple[at + 1]) << 16 | uint32_t(tuple[at + 2]) << 8 | tuple[at + 3];
}
}  // namespace
MagGuestTuple mag_guest_tuple(float value, float seconds, uint32_t phase, uint32_t events) {
    MagGuestTuple tuple{};
    put_word(tuple, 0, std::bit_cast<uint32_t>(value));
    put_word(tuple, 4, std::bit_cast<uint32_t>(seconds));
    put_word(tuple, 8, phase);
    put_word(tuple, 12, events);
    return tuple;
}
MagGuestTuple mag_guest_neutral_tuple() { return mag_guest_tuple(0.0f, 0.0f, 7, 0); }
float mag_guest_tuple_value(const MagGuestTuple& tuple) { return std::bit_cast<float>(get_word(tuple, 0)); }
float mag_guest_tuple_seconds(const MagGuestTuple& tuple) { return std::bit_cast<float>(get_word(tuple, 4)); }
uint32_t mag_guest_tuple_phase(const MagGuestTuple& tuple) { return get_word(tuple, 8); }

std::optional<MagGuestOwner> MagGameControlsGuest::resolve_owner(uint32_t handler, bool allow_disabled) const {
    auto r = reads();
    return ::resolve_owner(r, handler, allow_disabled);
}

std::optional<MagGuestOwner> MagGameControlsGuest::local_owner() const {
    auto r = reads();
    const auto local = local_actor(r);
    if (!local) return {};
    const auto controller = r.u32(uint64_t(local->actor) + ActorController);
    if (!controller || uint64_t(*controller) + ControllerHandler > 0xFFFFFFFFull) return {};
    return ::resolve_owner(r, *controller + ControllerHandler, true);
}

bool MagGameControlsGuest::owner_matches(const MagGuestOwner& owner) const {
    auto r = reads();
    const auto local = local_actor(r);
    const uint64_t actor = owner.actor;
    return local && local->handle == (owner.index << 16 | owner.generation) && local->actor == owner.actor &&
           equals(r.u32(actor + 4), local->handle) && equals(r.u32(actor + ActorController), owner.controller) &&
           equals(r.u32(uint64_t(owner.controller) + 4), owner.actor) && equals(r.u32(actor + ActorLogic), owner.logic) &&
           equals(r.u32(uint64_t(owner.logic) + 4), owner.actor);
}

std::optional<MagGuestOwner> MagGameControlsGuest::owner_of_actor(uint32_t actor) const {
    if (!mag_guest_pointer_like(actor)) return {};
    auto r = reads();
    const auto controller = r.u32(uint64_t(actor) + ActorController);
    if (!controller || uint64_t(*controller) + ControllerHandler > 0xFFFFFFFFull) return {};
    auto owner = ::resolve_owner(r, *controller + ControllerHandler, true);
    if (!owner || owner->actor != actor) return {};
    return owner;
}

std::optional<std::vector<MagGuestControl>> MagGameControlsGuest::watched_controls(uint32_t handler) const {
    auto r = reads();
    return ::watched_controls(r, handler);
}

std::optional<unsigned> MagGameControlsGuest::registration_count(uint32_t handler) const {
    auto r = reads();
    return ::registration_count(r, handler);
}

std::optional<bool> MagGameControlsGuest::removal_pending(uint32_t handler) const {
    auto r = reads();
    const auto current = r.u32(ProcessingState + 0x2F0);
    if (!current) return {};
    if (*current != handler) return false;
    const auto deferred = r.u8(ProcessingState + 0x2F4);
    if (!deferred) return {};
    return *deferred != 0;
}

std::optional<std::vector<uint32_t>> MagGameControlsGuest::gameplay_routes() const {
    auto r = reads();
    if (!equals(r.u32(PauseState + 0x9C0), 0u)) return {};
    std::vector<uint32_t> routes;
    if (!visit_active_elements(r, 0xFFFFFFFF, [&](uint32_t element) { return gameplay_element(r, element, routes); }))
        return {};
    return routes;
}

std::optional<std::vector<uint32_t>> MagGameControlsGuest::system_routes() const {
    auto r = reads();
    const auto nodes = list_nodes(r, InputManager + 0x2C);
    if (!nodes) return {};
    std::vector<uint32_t> listed;
    unsigned ui_registrations = 0;
    for (uint32_t node : *nodes) {
        const auto handler = r.u32(uint64_t(node) + 8);
        if (!handler) return {};
        if (*handler == UiHandler) {
            ++ui_registrations;
            continue;
        }
        if (!mag_guest_pointer_like(*handler) || !equals(r.u32(uint64_t(*handler) + HandlerSlot), 0u) ||
            !base_route(r, *handler, listed))
            return {};
    }
    if (ui_registrations != 1 || !equals(r.u32(UiHandler), UiHandlerVtable) ||
        !descriptor_is(r, uint64_t(UiHandlerVtable) + 8, UiUpdateEntry, SharedToc, UiUpdateDescriptor))
        return {};
    if (!visit_active_elements(r, 0xFFFFFFAB, [&](uint32_t element) { return base_route(r, element + 4, listed); }))
        return {};
    return listed;
}

std::optional<bool> MagGameControlsGuest::neutral_look(uint32_t handler) const {
    auto r = reads();
    const auto controls = ::watched_controls(r, handler);
    if (!controls) return {};
    unsigned present = 0;
    bool neutral = true;
    for (const auto& control : *controls) {
        const unsigned bit = mag_game_controls_look_bit(control.token);
        if (!bit) continue;
        const auto value = r.f32(InputManager + 0x7E0 + 4ull * control.channel);
        if (!value) return {};
        present |= bit;
        neutral = neutral && *value == 0.0f;
    }
    if (present != 0xF) return {};
    return neutral;
}

bool MagGameControlsGuest::base_update(uint32_t handler) const {
    auto r = reads();
    return mag_guest_pointer_like(handler) && ::base_update(r, handler);
}

bool MagGameControlsGuest::freshly_constructed(uint32_t handler, uint32_t priority, uint32_t slot) const {
    if (!mag_guest_pointer_like(handler)) return false;
    auto r = reads();
    const uint64_t h = handler, head = h + HandlerList;
    const auto enabled = r.u8(h + HandlerEnabled);
    return equals(r.u32(h), BaseHandlerVtable) && equals(r.u32(head), uint32_t(head)) &&
           equals(r.u32(head + 4), uint32_t(head)) && equals(r.u32(h + HandlerPriority), priority) &&
           equals(r.u32(h + HandlerSlot), slot) && enabled && *enabled &&
           equals(r.u32(h + HandlerFixedMask), 0u) && equals(r.u32(h + HandlerMappedMask), 0u);
}

bool MagGameControlsGuest::clean_assist_record(uint32_t record) const {
    auto r = reads();
    const uint64_t a = record;
    return equals(r.u32(a), 0u) && equals(r.u32(a + 4), 0u) && equals(r.f32(a + 8), 1.0f) &&
           equals(r.f32(a + 0x30), 0.0f) && equals(r.f32(a + 0x34), 0.0f) && equals(r.f32(a + 0x38), 0.0f) &&
           equals(r.f32(a + 0x3C), 1.0f);
}
