#pragma once
/* Guest readers of the game controls adapter (docs/native-controls.md, sections 1, 2 and 5). Every
 * read goes through the adapter's checked memory callbacks. A failed read, a malformed list or an
 * exhausted read budget makes the whole answer unavailable; it never becomes a value. */
#include "mag_game_controls_adapter.h"
#include "mag_game_controls_mouse_aim.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Fixed guest addresses and game-chosen values (section 2).
namespace mag_guest {
constexpr uint32_t ObjectManager = 0x01FB3D90, ActorPoolPointer = 0x01D39078;
constexpr uint32_t InputManager = 0x01FB8E60, UiManager = 0x01FB82C0;
constexpr uint32_t PauseState = 0x01DC8100, ProcessingState = 0x01DB3B18;
constexpr uint32_t UiHandler = UiManager + 0x28;
constexpr uint32_t BaseHandlerVtable = 0x01A57350;
constexpr uint32_t BaseUpdateEntry = 0x003A2C84, SharedToc = 0x01B35B20;
constexpr uint32_t CameraVtable = 0x01A644E0;
constexpr uint32_t CameraTarget = 0x22C, CameraAssistRecord = 0x1A0;
// Actor fields.
constexpr uint32_t ActorYawInput = 0x184, ActorYawRequest = 0x188, ActorPitchInput = 0x18C;
constexpr uint32_t ActorController = 0x590, ActorLogic = 0x594, ActorState = 0x670, ActorRemote = 0x6B4;
// Handler fields.
constexpr uint32_t HandlerList = 4, HandlerPriority = 0x38, HandlerSlot = 0x3C, HandlerEnabled = 0x40;
constexpr uint32_t HandlerFixedMask = 0x44, HandlerMappedMask = 0x48;
constexpr uint32_t ControllerHandler = 12;
// Pad record: channels consumed by earlier handlers this poll.
constexpr uint32_t PadConsumed = 0x80;
// Control record fields (relative to the record, which is watched node +8).
constexpr uint32_t RecordToken = 0, RecordBinding = 4, RecordTuning = 8, RecordTuple = 0x0C;
}  // namespace mag_guest

// A 32-bit value usable as a guest object address.
constexpr bool mag_guest_pointer_like(uint64_t value) {
    return value > 0xFFFF && value <= 0xFFFFFFFFull && value % 4 == 0;
}

// One resolution's view of guest memory: big-endian reads with the 25 000-read budget of section 1.
// Writes are not budgeted.
class MagGuestReads {
    MagGameControlsMemory memory_;
    unsigned remaining_;
public:
    static constexpr unsigned Budget = 25000;
    explicit MagGuestReads(const MagGameControlsMemory& memory, unsigned budget = Budget)
        : memory_(memory), remaining_(budget) {}
    // Raw bytes in guest order. Fails for an address range that leaves the 32-bit space.
    bool bytes(uint64_t address, void* output, size_t size);
    std::optional<uint8_t> u8(uint64_t address);
    std::optional<uint16_t> u16(uint64_t address);
    std::optional<uint32_t> u32(uint64_t address);
    std::optional<float> f32(uint64_t address);
    bool write_bytes(uint64_t address, const void* input, size_t size);
    bool write_u32(uint64_t address, uint32_t value);
    bool write_f32(uint64_t address, float value);
};

// The local player's input owner (5.1).
struct MagGuestOwner {
    uint32_t index{}, generation{};
    uint32_t actor{}, logic{}, controller{}, handler{}, state{};
    uint8_t state_id{};
    bool enabled{}, mouse_supported{};
    MagMouseAimState aim = MagMouseAimState::Basic;
    uint32_t handle() const { return generation << 16 | index; }
    friend bool operator==(const MagGuestOwner&, const MagGuestOwner&) = default;
};
// Same actor, handle, controller, logic and handler; the movement state may differ.
bool mag_guest_same_owner(const MagGuestOwner&, const MagGuestOwner&);

// A watched control with a mapped binding (5.2).
struct MagGuestControl {
    uint32_t token{}, channel{}, binding{}, record{};
    friend bool operator==(const MagGuestControl&, const MagGuestControl&) = default;
};

// The four published words at control record +0x0C, kept in guest byte order.
using MagGuestTuple = std::array<uint8_t, 16>;
MagGuestTuple mag_guest_tuple(float value, float seconds, uint32_t phase, uint32_t events);
MagGuestTuple mag_guest_neutral_tuple();
// The first word of a tuple as a float, and its phase word.
float mag_guest_tuple_value(const MagGuestTuple&);
float mag_guest_tuple_seconds(const MagGuestTuple&);
uint32_t mag_guest_tuple_phase(const MagGuestTuple&);

// Each member is one resolution with its own read budget.
class MagGameControlsGuest {
    MagGameControlsMemory memory_;
public:
    explicit MagGameControlsGuest(const MagGameControlsMemory& memory) : memory_(memory) {}
    MagGuestReads reads() const { return MagGuestReads(memory_); }

    // 5.1: owner for a handler; allow_disabled admits a disabled handler.
    std::optional<MagGuestOwner> resolve_owner(uint32_t handler, bool allow_disabled) const;
    // 5.1: the owner reached from the object manager's local actor, allowing a disabled handler.
    std::optional<MagGuestOwner> local_owner() const;
    // 5.1: cheap revalidation of a known owner's identity and links.
    bool owner_matches(const MagGuestOwner&) const;
    // The owner whose controller belongs to `actor`, when that resolves back to `actor`.
    std::optional<MagGuestOwner> owner_of_actor(uint32_t actor) const;
    // 5.2: mapped watched controls, in list order.
    std::optional<std::vector<MagGuestControl>> watched_controls(uint32_t handler) const;
    // 2.2: nodes naming the handler in the registered handler list.
    std::optional<unsigned> registration_count(uint32_t handler) const;
    // 5.3: unknown (nullopt) is to be treated as pending.
    std::optional<bool> removal_pending(uint32_t handler) const;
    // 5.4: handlers that may receive host input while gameplay is allowed; nullopt when not allowed.
    std::optional<std::vector<uint32_t>> gameplay_routes() const;
    // 5.5: handlers that may receive system buttons; nullopt when the routing contract fails.
    std::optional<std::vector<uint32_t>> system_routes() const;
    // 5.6: whether all four look channels of the handler read exactly zero.
    std::optional<bool> neutral_look(uint32_t handler) const;
    // The handler's vtable update descriptor is the base one.
    bool base_update(uint32_t handler) const;
    // The handler reads as freshly constructed with this priority and slot (6.6).
    bool freshly_constructed(uint32_t handler, uint32_t priority, uint32_t slot) const;
    // 2.2: a clean aim-assist record.
    bool clean_assist_record(uint32_t record) const;
};
