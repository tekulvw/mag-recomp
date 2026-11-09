#pragma once
/* The game controls adapter (docs/native-controls.md, sections 4 to 8). It is driven by seam events
 * on the game's update thread and talks to the portable input engine through player_controls.h. */
#include "mag_game_controls_hooks.h"
#include "player_controls.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

// The PPU registers visible at a seam. Writable where the specification says so.
struct MagGameControlsCpu {
    std::span<uint64_t, 32> gpr;
    std::span<double, 32> fpr;
    uint64_t lr{}, thread{};
};
// Checked guest memory access. A failed read or write returns false and never fabricates data.
struct MagGameControlsMemory {
    void* user{};
    bool (*read)(void* user, uint32_t address, void* output, size_t size){};
    bool (*write)(void* user, uint32_t address, const void* input, size_t size){};
};

class MagGameControlsAdapter {
    struct State;
    std::unique_ptr<State> state_;
public:
    // capture and mouse_capture request desktop keyboard/mouse capture and relative mouse mode.
    MagGameControlsAdapter(MagGameControlsMemory, uint64_t session, const mag::ControlsProfile&,
                           void (*capture)(bool) = nullptr, void (*mouse_capture)(bool) = nullptr);
    ~MagGameControlsAdapter();
    MagGameControlsAdapter(const MagGameControlsAdapter&) = delete;
    MagGameControlsAdapter& operator=(const MagGameControlsAdapter&) = delete;
    // Returns true only for early-result and replacement seams the adapter took over.
    bool dispatch(MagControlsHook, MagGameControlsCpu) noexcept;
    const char* status() const noexcept;
    // Validates and copies; installed at the next outermost update tick.
    void request_profile(const mag::ControlsProfile&);
};
