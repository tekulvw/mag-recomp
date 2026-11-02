#pragma once
#include "player_controls.h"
#include <span>
namespace mag {
void mag_controls_set_shared_native_actions(std::span<const ActionId>);
// Portable semantics only; the private adapter translates native event/cache words.
enum class ControlPhase : uint32_t { Idle, ReleasedTap, Down, SecondDown, Pressed, SecondPressed, Blocked, Refocus };
enum ControlEvent : uint32_t {
    ControlDown=1u<<0, ControlUp=1u<<1, ControlChange=1u<<2,
    ControlPress=1u<<3, ControlRelease=1u<<4, ControlTap=1u<<5,
    ControlDoubleTap=1u<<6, ControlSingleTap=1u<<7, ControlRepeat=1u<<8,
    ControlTapDown=1u<<9, ControlTapPress=1u<<10, ControlTapRelease=1u<<11,
    ControlWhileDown=1u<<12, ControlWhilePressed=1u<<13
};
struct ControlRecognitionConfig {
    uint64_t press_delay_ns{},double_tap_gap_ns{},repeat_delay_ns{},repeat_interval_ns{},refocus_delay_ns{};
    double down_threshold{0.5},change_epsilon{};
};
struct ControlRecognition {
    double value{};
    uint64_t elapsed_ns{};
    ControlPhase phase=ControlPhase::Idle;
    uint32_t events{};
};
struct ControlOffer {uint64_t pulse_id{},physical_duration_ns{};bool down{};uint32_t source_mask{};};
std::optional<ControlOffer> mag_controls_offer(InputFrameKey,ActionId);
ControlRecognition mag_controls_recognize(InputFrameKey,uint64_t consumer,uint64_t control,ActionId,
    double normalized_value,bool blocked,const ControlRecognitionConfig&);
// Explicit completion receipt; pure reads/recognition do not consume a pulse.
void mag_controls_observe(InputFrameKey,ActionId,const ControlOffer&);
void mag_controls_accept_action(InputFrameKey,ActionId,const ControlOffer&);
void mag_controls_accept_mouse(InputFrameKey);
void mag_controls_forget_consumer(uint64_t consumer);
}
