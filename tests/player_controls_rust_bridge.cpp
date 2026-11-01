#include "player_controls_rust.h"
#include "input_ffi.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace mag;
static void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static uint64_t time_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static void setup(){
    ControlsProfile p;p.mode=InputMode::Auto;
    p.bindings={{{InputSource::Keyboard,26},1},{{InputSource::Keyboard,8},2}};
    mag_controls_configure(p);mag_controls_set_window_capture(true,true);mag_controls_set_supported_context(true);
}
static InputFrameKey frame(uint64_t sequence){InputFrameKey k{17,sequence,0};mag_controls_acquire_frame(k,InputContext::OnFoot);return k;}
static ControlOffer offered(InputFrameKey k,ActionId a){auto o=mag_controls_offer(k,a);require(o.has_value(),"expected current action offer");return *o;}
static void held_frames(){
    setup();mag_controls_push_event({{InputSource::Keyboard,26},true,time_ns()});uint64_t id=0;
    // The game adapter must keep the same input lifetime during these output-capability transitions.
    for(uint64_t sequence=1;sequence<=6;++sequence){
        mag_controls_set_supported_context(true);auto k=frame(sequence);auto o=offered(k,1);
        require(o.down,"retirement or capability-preserving update dropped held W");
        if(!id)id=o.pulse_id;require(id==o.pulse_id,"held W changed pulse identity without an edge");
        mag_controls_observe(k,1,o);mag_controls_retire_frame(k);
        require(!mag_controls_offer(k,1),"retired frame remained readable");
    }
    mag_controls_push_event({{InputSource::Keyboard,26},false,time_ns()});auto k=frame(7);
    auto up=offered(k,1);require(!up.down&&up.pulse_id==id,"W release did not follow the original hold");
    mag_controls_observe(k,1,up);mag_controls_retire_frame(k);
    k=frame(8);require(!mag_controls_offer(k,1),"completed W release replayed");mag_controls_retire_frame(k);
}
static void interaction(){
    setup();mag_controls_push_event({{InputSource::Keyboard,8},true,time_ns()});ControlRecognitionConfig config;config.press_delay_ns=0;
    uint64_t duration=0;
    for(uint64_t sequence=1;sequence<=4;++sequence){auto k=frame(sequence);auto o=offered(k,2);
        require(o.down&&o.physical_duration_ns>=duration,"E hold disappeared or duration went backwards");duration=o.physical_duration_ns;
        auto gameplay=mag_controls_recognize(k,10,100,2,1,false,config);
        auto hud=mag_controls_recognize(k,20,200,2,1,false,config);
        require((gameplay.events&ControlWhileDown)&&(hud.events&ControlWhileDown),"gameplay/HUD did not both see continuous E");
        auto repeated=mag_controls_recognize(k,10,100,2,1,false,config);
        require(repeated.events==gameplay.events&&repeated.elapsed_ns==gameplay.elapsed_ns,"repeat getter advanced recognition");
        mag_controls_observe(k,2,o);mag_controls_retire_frame(k);
    }
    mag_controls_push_event({{InputSource::Keyboard,8},false,time_ns()});auto k=frame(5);auto up=offered(k,2);
    require(!up.down,"E release did not become observable");
    auto gameplay=mag_controls_recognize(k,10,100,2,0,false,config);
    auto hud=mag_controls_recognize(k,20,200,2,0,false,config);
    require((gameplay.events&ControlUp)&&(hud.events&ControlUp),"E release failed to reach gameplay and HUD independently");
    mag_controls_observe(k,2,up);mag_controls_retire_frame(k);
}
static void policy_acceptance(){
    setup();require(mag_controls_assist_allowed(mag_controls_current_policy()),"Auto initially denies assist");
    mag_controls_push_event({{InputSource::Keyboard,8},true,time_ns()});auto k=frame(1);auto o=offered(k,2);
    require(mag_controls_assist_allowed(mag_controls_current_policy()),"provisional attachment marked gameplay KBM");
    mag_controls_recognize(k,20,200,2,1,false,{});
    require(mag_controls_assist_allowed(mag_controls_current_policy()),"UI recognition marked gameplay KBM");
    mag_controls_accept_action(k,2,o);
    require(!mag_controls_assist_allowed(mag_controls_current_policy()),"accepted gameplay KBM failed to disable assist");
    require(offered(k,2).down,"accepting policy consumed the action");
    mag_controls_observe(k,2,o);mag_controls_retire_frame(k);
}
static void abi_handles(){
    require(mag_input_abi_version()==1,"unexpected ABI");uint64_t handle=0,generation=0;
    require(mag_input_create(1,nullptr,0,&handle)==0,"C create failed");
    require(mag_input_capture(handle,0x100,&generation)!=0,"invalid capture flags accepted");
    require(mag_input_capture(handle,7,nullptr)!=0,"null output accepted");
    std::atomic<unsigned> errors{};std::vector<std::thread> threads;
    for(unsigned i=0;i<8;++i)threads.emplace_back([&]{for(unsigned n=0;n<100;++n){uint64_t g=0;if(mag_input_capture(handle,7,&g))++errors;}});
    for(auto& thread:threads)thread.join();require(errors==0,"concurrent C calls failed");
    require(mag_input_destroy(handle)==0,"C destroy failed");
    require(mag_input_capture(handle,7,&generation)!=0,"destroyed handle remained usable");
    uint64_t replacement=0;require(mag_input_create(1,nullptr,0,&replacement)==0,"replacement create failed");
    require(replacement!=handle,"destroyed handle was reused without a generation");
    require(mag_input_destroy(replacement)==0,"replacement destroy failed");
}
int main(int argc,char** argv){try{if(argc!=2)return 2;std::string which=argv[1];if(which=="held")held_frames();else if(which=="interaction")interaction();else if(which=="policy")policy_acceptance();else if(which=="abi")abi_handles();else return 2;return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
