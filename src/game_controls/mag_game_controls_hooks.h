#pragma once
/* Seam events of the opt-in game controls adapter (docs/native-controls.md, section 3).
 * Lifted sources prepared with the `controls` hook stage include this header and call
 * mag_game_controls_hook at each seam, under the MAG_GAME_CONTROLS_HOOKS guard. With no
 * callback installed every call returns false and the game runs unchanged. */
#include <cstdint>
struct ppu_context;
enum class MagControlsHook : uint8_t {
    OuterEnter, OuterExit, PollEnter, PollExit, SampleSeal,
    HandlerEnter, HandlerExit, HandlerRemove, HandlerRemoveExit, HandlerDestroy, HandlerDestroyed,
    HandlerConstruct, HandlerConstructed, HandlerAdd, HandlerAdded, HandlerRefresh, HandlerRefreshed,
    ControlLoop, ControlRecognize, ControlCallback,
    ActorEnter, ActorExit, Prepare,
    YawRateEnter, YawRateExit, YawDamage,
    SprintPitchRateEnter, SprintPitchRateExit, SprintPitchDamage,
    BasicYaw, BasicPitch, SprintYaw, SprintPitch,
    AssistEligibility, AssistAdhesion, AssistFrictionYaw, AssistFrictionPitch,
    AssistCleanupEnter, AssistCleanupExit,
};
// Null by default. Installing a callback does not by itself admit any context: the adapter
// validates ownership, phase, sample and UI eligibility at every event.
using MagControlsHookCallback = bool (*)(MagControlsHook, ppu_context*) noexcept;
void mag_game_controls_install_hooks(MagControlsHookCallback) noexcept;
bool mag_game_controls_hook(MagControlsHook, ppu_context*) noexcept;
// Entry/exit seam: the exit event runs on every return path of the wrapped function.
class MagGameControlsHookScope {
    MagControlsHookCallback callback_{};
    MagControlsHook exit_{};
    ppu_context* context_{};
public:
    MagGameControlsHookScope(MagControlsHook enter, MagControlsHook exit, ppu_context*) noexcept;
    ~MagGameControlsHookScope();
    MagGameControlsHookScope(const MagGameControlsHookScope&) = delete;
    MagGameControlsHookScope& operator=(const MagGameControlsHookScope&) = delete;
};
