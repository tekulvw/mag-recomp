#include "mag_game_controls_mouse_aim.h"
#include <cmath>
#include <limits>
namespace {
bool supported(MagMouseAimState state) {
    return state == MagMouseAimState::Basic || state == MagMouseAimState::Prone || state == MagMouseAimState::Sprint;
}
bool angular(double value) { return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(); }
bool modifier(double value) { return angular(value) && value >= 0; }
}  // namespace
bool MagGameControlsMouseAim::prepare(const MagMouseAimIdentity& id, double yaw, double pitch, double dt,
                                      bool rotation_allowed, bool mouse_owned) {
    if (active_ || !process_session_ || id.process_session != process_session_ ||
        !id.input_sample || id.input_sample <= last_sample_) return false;
    // A blocked or invalid sample is discarded, never held for a later consumer or another actor
    // generation; a later update tick cannot renew it.
    last_sample_ = id.input_sample;
    if (!id.outer_epoch || !id.capture_generation || !id.actor_handle || !id.actor || !id.logic ||
        !id.controller || !id.state || !supported(id.state_kind) || !rotation_allowed || !mouse_owned ||
        !std::isfinite(dt) || dt <= 0 || !angular(yaw) || !angular(pitch)) return false;
    active_ = id;
    yaw_ = yaw;
    pitch_ = pitch;
    yaw_modifier_.reset();
    sprint_pitch_modifier_.reset();
    yaw_consumed_ = false;
    pitch_consumed_ = false;
    return true;
}
bool MagGameControlsMouseAim::observe_modifier(const MagMouseAimIdentity& id, MagMouseModifierSite site, double value) {
    if (!active_ || *active_ != id || !modifier(value)) return false;
    if (site == MagMouseModifierSite::YawRateDamage) {
        if (yaw_consumed_ || yaw_modifier_) return false;
        yaw_modifier_ = value;
        return true;
    }
    if (site == MagMouseModifierSite::SprintPitchDamage && id.state_kind == MagMouseAimState::Sprint) {
        if (pitch_consumed_ || sprint_pitch_modifier_) return false;
        sprint_pitch_modifier_ = value;
        return true;
    }
    return false;
}
std::optional<double> MagGameControlsMouseAim::consume(const MagMouseAimIdentity& id, MagMouseAimSite site,
                                                       double basic_pitch_damage_modifier) {
    if (!active_ || *active_ != id) return {};
    const bool sprint = id.state_kind == MagMouseAimState::Sprint;
    bool yaw = false;
    switch (site) {
    case MagMouseAimSite::BasicYaw: if (sprint) return {}; yaw = true; break;
    case MagMouseAimSite::BasicPitch: if (sprint) return {}; break;
    case MagMouseAimSite::SprintYaw: if (!sprint) return {}; yaw = true; break;
    case MagMouseAimSite::SprintPitch: if (!sprint) return {}; break;
    default: return {};
    }
    bool& consumed = yaw ? yaw_consumed_ : pitch_consumed_;
    if (consumed) return {};
    consumed = true;
    const std::optional<double> scale = yaw ? yaw_modifier_
        : sprint ? sprint_pitch_modifier_ : std::optional<double>(basic_pitch_damage_modifier);
    if (!scale || !modifier(*scale)) return {};
    const double value = (yaw ? yaw_ : pitch_) * *scale;
    if (!angular(value) || value == 0) return {};
    return value;
}
void MagGameControlsMouseAim::end(const MagMouseAimIdentity& id) {
    if (active_ && *active_ == id) cancel();
}
void MagGameControlsMouseAim::cancel() {
    active_.reset();
    yaw_modifier_.reset();
    sprint_pitch_modifier_.reset();
    yaw_ = 0;
    pitch_ = 0;
    yaw_consumed_ = true;
    pitch_consumed_ = true;
}
