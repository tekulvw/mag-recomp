#include "player_input_capture.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
namespace {
struct Capture {
    std::mutex mutex;
    std::atomic<bool> collecting{false};
    std::array<void*,4> controllers{};
    uint64_t last_sample=0,active_sample=0,backend_sequence=0;
    bool backend_seen=false,invalid=false;
    unsigned axes_seen=0;
    MagInputControllerSample sample{};
};
Capture& capture() {static Capture state;return state;}
void clear_sample(Capture& c) {
    c.sample={};c.sample.input_sample_sequence=c.active_sample;c.sample.backend_sequence=c.backend_sequence;
    c.axes_seen=0;
}
bool current(const Capture& c,void* controller) {
    return controller && c.active_sample && c.backend_seen && c.controllers[0]==controller;
}
}
extern "C" {
int mag_input_capture_poll_begin(uint64_t sequence) {
    auto& c=capture();std::lock_guard lock(c.mutex);
    if(!sequence || sequence<=c.last_sample || c.active_sample)return 0;
    c.active_sample=sequence;c.last_sample=sequence;c.backend_seen=false;c.invalid=false;clear_sample(c);
    c.collecting.store(true,std::memory_order_release);
    return 1;
}
int mag_input_capture_poll_complete(uint64_t sequence,MagInputControllerSample* out) {
    auto& c=capture();std::lock_guard lock(c.mutex);
    if(!out || !c.active_sample || sequence!=c.active_sample)return 0;
    c.active_sample=0;
    c.collecting.store(false,std::memory_order_release);
    if(!c.backend_seen || c.invalid || (c.sample.connected && c.axes_seen!=0x3f))return 0;
    *out=c.sample;return 1;
}
void mag_input_capture_poll_abort(uint64_t sequence) {
    auto& c=capture();std::lock_guard lock(c.mutex);
    if(c.active_sample==sequence) {c.active_sample=0;c.collecting.store(false,std::memory_order_release);}
}
void mag_input_capture_controller_open(void* controller,unsigned port) {
    auto& c=capture();std::lock_guard lock(c.mutex);
    if(port>=c.controllers.size())return;
    if(port==0 && c.controllers[0]!=controller && c.active_sample && c.backend_seen)c.invalid=true;
    c.controllers[port]=controller;
}
void mag_input_capture_controller_close(void* controller) {
    auto& c=capture();std::lock_guard lock(c.mutex);
    for(unsigned port=0;port<c.controllers.size();++port)if(c.controllers[port]==controller) {
        c.controllers[port]=nullptr;
        if(port==0)clear_sample(c);
    }
}
void mag_input_capture_backend_begin() {
    auto& c=capture();if(!c.collecting.load(std::memory_order_acquire))return;std::lock_guard lock(c.mutex);
    if(!c.active_sample)return;
    if(c.backend_sequence==std::numeric_limits<uint64_t>::max()) {c.invalid=true;return;}
    ++c.backend_sequence;
    c.backend_seen=true;c.invalid=false;clear_sample(c);
}
void mag_input_capture_controller_attached(void* controller,int attached) {
    auto& c=capture();if(!c.collecting.load(std::memory_order_acquire))return;std::lock_guard lock(c.mutex);
    if(!current(c,controller))return;
    if(attached)c.sample.connected=1;else clear_sample(c);
}
void mag_input_capture_controller_axis(void* controller,unsigned axis,int32_t raw) {
    auto& c=capture();if(!c.collecting.load(std::memory_order_acquire))return;std::lock_guard lock(c.mutex);
    if(!current(c,controller) || !c.sample.connected || axis>MAG_INPUT_RIGHT_TRIGGER)return;
    double value;
    if(axis>=MAG_INPUT_LEFT_TRIGGER)value=double(std::clamp(raw,0,32767))/32767;
    else {raw=std::clamp(raw,-32768,32767);value=double(raw)/(raw<0?32768:32767);}
    switch(axis) {
    case MAG_INPUT_MOVE_X:c.sample.move_x=value;break;
    case MAG_INPUT_MOVE_Y:c.sample.move_y=value;break;
    case MAG_INPUT_LOOK_X:c.sample.look_x=value;break;
    case MAG_INPUT_LOOK_Y:c.sample.look_y=value;break;
    case MAG_INPUT_LEFT_TRIGGER:c.sample.left_trigger=value;break;
    case MAG_INPUT_RIGHT_TRIGGER:c.sample.right_trigger=value;break;
    }
    c.axes_seen|=1u<<axis;
}
void mag_input_capture_controller_button(void* controller,unsigned button,int down) {
    auto& c=capture();if(!c.collecting.load(std::memory_order_acquire))return;std::lock_guard lock(c.mutex);
    if(!current(c,controller) || !c.sample.connected || button>=32)return;
    if(down)c.sample.buttons|=1u<<button;else c.sample.buttons&=~(1u<<button);
}
}
