#pragma once
#include <cstdint>
#include <optional>

// Aim kind of a mouse-capable movement state (docs/native-controls.md, 2.2). Values are the
// game's state ids for the representative state of each kind.
enum class MagMouseAimState : uint8_t { Basic = 1, Prone = 18, Sprint = 23 };
enum class MagMouseAimSite { BasicYaw, BasicPitch, SprintYaw, SprintPitch };
enum class MagMouseModifierSite { YawRateDamage, SprintPitchDamage };

struct MagMouseAimIdentity {
    uint64_t process_session{}, input_sample{}, outer_epoch{}, capture_generation{};
    uint32_t actor_handle{}, actor{}, logic{}, controller{}, state{};
    MagMouseAimState state_kind = MagMouseAimState::Basic;
    friend bool operator==(const MagMouseAimIdentity&, const MagMouseAimIdentity&) = default;
};

// One input sample's mouse displacement, delivered once to each of the game's yaw and pitch
// consumers. The simulation thread owns it. It carries angular displacement, never an absolute
// angle or a stick rate. A sample rejected for state, context or identity is retired at once; a
// foreign session or a nested preparation cannot replace or retire the active one.
class MagGameControlsMouseAim {
    uint64_t process_session_{}, last_sample_{};
    std::optional<MagMouseAimIdentity> active_;
    double yaw_{}, pitch_{};
    std::optional<double> yaw_modifier_, sprint_pitch_modifier_;
    bool yaw_consumed_{}, pitch_consumed_{};
public:
    explicit MagGameControlsMouseAim(uint64_t process_session) : process_session_(process_session) {}
    bool prepare(const MagMouseAimIdentity&, double yaw, double pitch, double dt,
                 bool rotation_allowed, bool mouse_owned);
    bool observe_modifier(const MagMouseAimIdentity&, MagMouseModifierSite, double);
    std::optional<double> consume(const MagMouseAimIdentity&, MagMouseAimSite,
                                  double basic_pitch_damage_modifier = 1);
    void end(const MagMouseAimIdentity&);
    void cancel();
    bool active() const { return active_.has_value(); }
};
