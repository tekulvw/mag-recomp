#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
namespace mag {
// A single atomic stamp carries readiness and generation. Callback receipt may
// run on a device queue; publication and delivery belong to the window thread.
class DesktopCaptureGate {
    std::atomic<uint64_t> state_{};
public:
    struct Stamp {uint64_t state{};};
    Stamp stamp() const {return {state_.load(std::memory_order_acquire)};}
    void publish(bool ready,bool relative) {
        const auto old=state_.load(std::memory_order_relaxed);
        const uint64_t flags=uint64_t(ready)|(uint64_t(ready && relative)<<1);
        if((old&3)!=flags)state_.store(((old&~uint64_t(3))+4)|flags,std::memory_order_release);
    }
    void invalidate() {state_.fetch_add(4,std::memory_order_acq_rel);}
    bool accepts(Stamp receipt,bool motion=false) const {
        const auto current=state_.load(std::memory_order_acquire);
        return receipt.state==current && (current&1) && (!motion || (current&2));
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
