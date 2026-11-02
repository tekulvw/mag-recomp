#include "player_controls_settings.h"
#include "player_controls_store.h"
#include <cassert>
#include <fstream>
using namespace mag;
int main(int argc,char**argv) {
    assert(argc==2); ControlsSettingsRegistration reg;
    reg.path=std::filesystem::path(argv[1])/"controls.json";
    reg.actions={{1,"Move",1,3},{2,"Fire",1,7}};
    reg.defaults.bindings={{{InputSource::Keyboard,26},1},{{InputSource::Mouse,0},2},{{InputSource::Controller,10},2}};
    unsigned applied=0;reg.apply=[&](const ControlsProfile& p){assert(p.mouse.radians_per_count==.002);++applied;};
    reg.validate=[](const ControlsProfile& p){if(p.mouse.radians_per_count>1)throw std::invalid_argument("unsupported");};
    mag_controls_settings_register(reg); assert(mag_controls_settings_available());
    auto draft=mag_controls_settings_load(); assert(draft.bindings.size()==3);
    assert(mag_controls_settings_edit(draft,{{InputSource::Keyboard,26},2},0)==1);
    assert(draft.bindings[0].action==1);
    assert(mag_controls_settings_edit(draft,{{InputSource::Keyboard,26},2},0,false,true)==0);
    assert(draft.bindings.size()==2);
    mag_controls_settings_reset(draft,false);assert(draft.bindings.size()==3);
    assert(mag_controls_settings_edit(draft,{{InputSource::Controller,0},2},0,true)==0 && draft.controller_remap);
    assert(draft.bindings.size()==2);
    bool rejected=false;try{mag_controls_settings_edit(draft,{{InputSource::Controller,0},1},0);}catch(...){rejected=true;}assert(rejected);
    draft.mouse.radians_per_count=.002;mag_controls_settings_save(draft);assert(applied==1);
    assert(mag_controls_settings_load().bindings.size()==2); // Explicit unbound must survive reload.
    auto invalid=draft;invalid.mouse.radians_per_count=2;
    rejected=false;try{mag_controls_settings_save(invalid);}catch(...){rejected=true;}assert(rejected&&applied==1);
    assert(mag_controls_settings_load().mouse.radians_per_count==.002);
    for(uint32_t code:{4u,6u,11u,12u,13u,14u}) {
        auto buttons=mag_controls_settings_load();
        assert(mag_controls_settings_edit(buttons,{{InputSource::Controller,code},2},0)==0);
        assert(buttons.controller_remap);
        assert(std::any_of(buttons.bindings.begin(),buttons.bindings.end(),[&](auto b){return b.action==2&&b.control==PhysicalControl{InputSource::Controller,code};}));
    }
    assert(mag_controls_settings_control_name({InputSource::Controller,4})=="Select / Back");
    assert(mag_controls_settings_control_name({InputSource::Controller,6})=="Start / Menu");
    mag_controls_settings_unregister();assert(!mag_controls_settings_available());
    reg.actions.push_back({3,"Start",17,7,"Native menu / command buttons"});
    reg.defaults.catalog_revision=1;
    reg.defaults.bindings.push_back({{InputSource::Keyboard,41},3,17});
    reg.defaults.bindings.push_back({{InputSource::Controller,6},3,17});
    reg.upgrade=[](ControlsProfile& p){if(p.catalog_revision==0){p.bindings.push_back({{InputSource::Keyboard,41},3,17});p.bindings.push_back({{InputSource::Controller,6},3,17});p.catalog_revision=1;}};
    mag_controls_settings_register(reg);auto migrated=mag_controls_settings_load();
    assert(migrated.catalog_revision==1);
    assert(mag_controls_settings_edit(migrated,{{InputSource::Keyboard,0},3,17},0,true)==0);
    assert(mag_controls_settings_edit(migrated,{{InputSource::Controller,0},3,17},0,true)==0);
    mag_controls_settings_save(migrated);auto unbound=mag_controls_settings_load();
    assert(unbound.catalog_revision==1 && std::none_of(unbound.bindings.begin(),unbound.bindings.end(),[](auto b){return b.action==3;}));
    mag_controls_settings_unregister();
}
