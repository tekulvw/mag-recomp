#pragma once
/* Host actions (docs/native-controls.md, "Host actions"): MAG's own bound actions, such as Push to
 * talk, that the host reads and the game never sees. The tracker follows the same physical events
 * and window focus as the input engine, so a host action is held only while the game window has
 * input focus (not while another app or the settings panel owns it), and a control already held
 * when focus returns counts only after it is released once. Both input engines (the portable C++
 * one and the Rust bridge) keep one, under their own lock. */
#include "player_controls.h"
#include <algorithm>
#include <set>

namespace mag {
class HostActionTracker {
public:
    void configure(const ControlsProfile& profile) { profile_ = profile; }
    void focus(bool focused) {
        if (focused == focused_) return;
        focused_ = focused;
        // Losing focus forgets what was held; presses seen before focus returns are blocked.
        if (!focused) down_.clear(), blocked_.clear();
    }
    void event(PhysicalControl control, bool down) {
        if (!down) {
            down_.erase(control);
            blocked_.erase(control);
            return;
        }
        if (blocked_.count(control)) return;
        if (!focused_) {
            blocked_.insert(control);
            return;
        }
        down_.insert(control);
    }
    HostActionState state(ActionId action) const {
        InputPolicy permission;
        permission.mode = profile_.mode;
        const bool held = focused_ && std::any_of(profile_.bindings.begin(), profile_.bindings.end(), [&](const auto& b) {
            // Controller bindings count only while the profile remaps the controller (the game's
            // own buttons stand otherwise), and only sources the input mode allows.
            return b.action == action && down_.count(b.control) &&
                   (b.control.source != InputSource::Controller || profile_.controller_remap) &&
                   mag_controls_gameplay_source_allowed(permission, b.control.source);
        });
        return {held, focused_};
    }

private:
    ControlsProfile profile_;
    bool focused_ = true;  // as InputCaptureState: focused until a capture backend says otherwise
    std::set<PhysicalControl> down_, blocked_;
};
}  // namespace mag
