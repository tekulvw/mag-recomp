#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
namespace mag {
// One atomic stamp carries independent button/readiness and motion epochs.
// Intentional menu relative-mode changes invalidate motion, preserving buttons.
class DesktopCaptureGate {
    std::atomic<uint64_t> state_{};
    static constexpr uint64_t epochMask=(uint64_t(1)<<31)-1;
    static uint64_t advance(uint64_t state,bool buttons,bool motion) {
        return (state&3)|((((state>>2)+uint64_t(buttons))&epochMask)<<2)|((((state>>33)+uint64_t(motion))&epochMask)<<33);
    }
public:
    struct Stamp {uint64_t state{};};
    Stamp stamp() const {return {state_.load(std::memory_order_acquire)};}
    void publish(bool ready,bool relative,bool relative_only=false) {
        auto old=state_.load(std::memory_order_relaxed);
        const uint64_t flags=uint64_t(ready)|(uint64_t(ready && relative)<<1);
        while((old&3)!=flags) {
            const bool buttons=((old&1)!=(flags&1)) || !relative_only;
            const uint64_t next=(advance(old,buttons,true)&~uint64_t(3))|flags;
            if(state_.compare_exchange_weak(old,next,std::memory_order_release,std::memory_order_relaxed))break;
        }
    }
    void invalidate() {
        auto old=state_.load(std::memory_order_relaxed);
        while(!state_.compare_exchange_weak(old,advance(old,true,true),std::memory_order_release,std::memory_order_relaxed)){}
    }
    bool accepts(Stamp receipt,bool motion=false) const {
        const auto current=state_.load(std::memory_order_acquire);
        if(motion)return receipt.state==current && (current&1) && (current&2);
        constexpr uint64_t buttonMask=(epochMask<<2)|1;
        return (receipt.state&buttonMask)==(current&buttonMask) && (current&1);
    }
};
// Window-thread ownership. Every physical device contributes independently;
// only aggregate transitions enter the portable action pipeline.
class DesktopButtonAggregation {
    std::map<uint32_t,std::set<uintptr_t>> held_;
public:
    std::optional<bool> update(uint32_t code,uintptr_t device,bool down) {
        auto& devices=held_[code];const bool before=!devices.empty();
        if(down)devices.insert(device);else devices.erase(device);
        const bool after=!devices.empty();if(before==after)return {};return after;
    }
    void clear() {held_.clear();}
};
}
