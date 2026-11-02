#include "player_controls_settings.h"
#include "player_controls_store.h"
#include "input_ffi.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <stdexcept>
namespace mag {
namespace {
std::mutex mutex;
std::optional<ControlsSettingsRegistration> registration;
void validate(const ControlsSettingsRegistration& r,const ControlsProfile& p) {
    mag_controls_validate_profile(p);
    for(auto b:p.bindings) {
        auto action=std::find_if(r.actions.begin(),r.actions.end(),[&](const auto& a){return a.id==b.action;});
        if(action==r.actions.end() || (b.contexts&~action->contexts) || !(action->sources&(1u<<unsigned(b.control.source))))
            throw std::invalid_argument("This binding is not supported by the current runtime.");
    }
    if(r.validate)r.validate(p);
}
std::vector<ActionId> catalog(const ControlsSettingsRegistration& r) {
    std::vector<ActionId> ids;for(const auto& a:r.actions)ids.push_back(a.id);return ids;
}
}
void mag_controls_settings_register(ControlsSettingsRegistration r) {
    if(r.actions.empty() || r.path.empty())throw std::invalid_argument("Controls settings require an action catalog and profile path.");
    std::vector<ActionId> seen;
    for(const auto& a:r.actions) {
        if(!a.id || a.label.empty() || !a.contexts || a.contexts&~31u || !a.sources || a.sources&~7u || std::find(seen.begin(),seen.end(),a.id)!=seen.end())
            throw std::invalid_argument("Invalid controls action descriptor.");
        seen.push_back(a.id);
    }
    validate(r,r.defaults);std::lock_guard lock(mutex);registration=std::move(r);
}
void mag_controls_settings_unregister(){std::lock_guard lock(mutex);registration.reset();}
bool mag_controls_settings_available(){std::lock_guard lock(mutex);return registration.has_value();}
ControlsSettingsRegistration mag_controls_settings_registration(){std::lock_guard lock(mutex);if(!registration)throw std::runtime_error("Controls are unavailable in this runtime.");return *registration;}
ControlsProfile mag_controls_settings_load(){
    const auto r=mag_controls_settings_registration();
    auto p=std::filesystem::exists(r.path)?mag_controls_load_profile(r.path,catalog(r)):r.defaults;
    // Schema1/native profiles did not store editable controller defaults. Fill
    // them only while Native still owns digital buttons; explicit unbound stays empty.
    if(r.upgrade)r.upgrade(p);
    if(!p.controller_remap) {
        std::erase_if(p.bindings,[](auto b){return b.control.source==InputSource::Controller;});
        for(auto b:r.defaults.bindings)if(b.control.source==InputSource::Controller)p.bindings.push_back(b);
    }
    validate(r,p);return p;
}
void mag_controls_settings_save(const ControlsProfile& p){
    const auto r=mag_controls_settings_registration();validate(r,p);
    if(!mag_controls_save_profile(r.path,p,catalog(r)))throw std::runtime_error("Could not save controls. The previous profile was preserved.");
    if(r.apply)r.apply(p);
    std::lock_guard lock(mutex);if(registration && registration->path==r.path)registration->mode_override.reset();
}
ActionId mag_controls_settings_edit(ControlsProfile& p,ControlBinding binding,unsigned slot,bool clear,bool replace){
    const auto r=mag_controls_settings_registration();
    auto action=std::find_if(r.actions.begin(),r.actions.end(),[&](const auto& a){return a.id==binding.action;});
    if(action==r.actions.end() || unsigned(binding.control.source)>2 || !(action->sources&(1u<<unsigned(binding.control.source))) || binding.contexts&~action->contexts)
        throw std::invalid_argument("This action does not support that input device or context.");
    std::vector<MagInputBinding> input;for(auto b:p.bindings)input.push_back({uint32_t(b.control.source),b.control.code,b.action,b.contexts,uint32_t(b.behavior)});
    std::array<MagInputBinding,1024> output{};uint32_t count=0,conflict=0;
    const auto status=mag_input_edit_binding(input.data(),uint32_t(input.size()),{uint32_t(binding.control.source),binding.control.code,binding.action,binding.contexts,uint32_t(binding.behavior)},slot,uint32_t(clear)|(uint32_t(replace)<<1),output.data(),uint32_t(output.size()),&count,&conflict);
    if(status==7)return conflict;
    if(status)throw std::invalid_argument("That binding could not be assigned.");
    auto next=p;next.bindings.clear();
    for(uint32_t i=0;i<count;++i){auto b=output[i];next.bindings.push_back({{InputSource(b.source),b.code},b.action,b.contexts,BindingBehavior(b.behavior)});}
    if(binding.control.source==InputSource::Controller)next.controller_remap=true;
    validate(r,next);p=std::move(next);return 0;
}
void mag_controls_settings_reset(ControlsProfile& p,bool controller){
    const auto r=mag_controls_settings_registration();auto next=p;
    std::erase_if(next.bindings,[&](auto b){return (b.control.source==InputSource::Controller)==controller;});
    for(auto b:r.defaults.bindings)if((b.control.source==InputSource::Controller)==controller)next.bindings.push_back(b);
    if(controller){next.controller=r.defaults.controller;next.controller_remap=r.defaults.controller_remap;}else next.mouse=r.defaults.mouse;
    validate(r,next);p=std::move(next);
}
std::string mag_controls_settings_control_name(PhysicalControl c){
    if(c.source==InputSource::Mouse)return c.code==0?"Left mouse":c.code==1?"Right mouse":c.code==2?"Middle mouse":"Mouse "+std::to_string(c.code+1);
    if(c.source==InputSource::Controller){
        static const char* names[]={"Cross / A","Circle / B","Square / X","Triangle / Y","Select / Back","Guide","Start / Menu","L3","R3","L1 / LB","R1 / RB","D-pad Up","D-pad Down","D-pad Left","D-pad Right"};
        if(c.code<std::size(names))return names[c.code];
        if(c.code==32)return "L2 / LT";if(c.code==33)return "R2 / RT";return "Controller "+std::to_string(c.code);
    }
    if(c.code>=4 && c.code<=29)return std::string(1,char('A'+c.code-4));
    if(c.code>=30 && c.code<=38)return std::to_string(c.code-29);
    switch(c.code){case 39:return "0";case 40:return "Return";case 41:return "Escape";case 42:return "Backspace";case 43:return "Tab";case 44:return "Space";case 79:return "Right";case 80:return "Left";case 81:return "Down";case 82:return "Up";case 224:return "Left Control";case 225:return "Left Shift";case 226:return "Left Alt";case 228:return "Right Control";case 229:return "Right Shift";case 230:return "Right Alt";default:return "Key "+std::to_string(c.code);}
}
}
