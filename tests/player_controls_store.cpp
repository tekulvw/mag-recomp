#include "player_controls_store.h"
#include "player_settings_portable.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
using namespace mag;
static void check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
static std::string read(const std::filesystem::path& path) { std::ifstream f(path); return {std::istreambuf_iterator<char>(f),{}}; }
static void write(const std::filesystem::path& path,const std::string& text) { std::ofstream f(path); f<<text; }
int main(int argc,char** argv) {
 try {
    if(argc!=2) return 2;
    const std::filesystem::path root=argv[1], path=root/"controls.json", old=root/"settings.json";
    const ActionId catalog[]={1,2};
    PlayerSettings settings; settings.server_address="example.test"; settings.render_scale=200;
    save_player_settings(old,settings); const auto original=read(old);
    const auto defaults=mag_controls_load_profile(path,catalog);
    check(defaults.mode==InputMode::Auto && defaults.bindings.empty(),"missing controls return defaults without changing old settings");
    ControlsProfile profile; profile.mode=InputMode::KBM; profile.mouse.ads_gain=.4; profile.mouse.scoped_gain=.2;
    profile.mouse.invert_y=true; profile.controller.response=ControllerResponse::Linear; profile.controller.inner_x=.15; profile.controller.outer_x=.1;
    profile.bindings={{{InputSource::Keyboard,26},1,1,BindingBehavior::Toggle},{{InputSource::Controller,0},2,1}};
    check(mag_controls_save_profile(path,profile,catalog),"profile save succeeds");
    const auto loaded=mag_controls_load_profile(path,catalog); const auto good=read(path);
    check(loaded.mode==profile.mode && loaded.mouse.ads_gain==.4 && loaded.mouse.invert_y && loaded.controller.inner_x==.15 &&
          loaded.bindings.size()==2 && loaded.bindings[0].behavior==BindingBehavior::Toggle && loaded.bindings[1].control.source==InputSource::Controller,"independent controller and KBM preferences round trip");
    check(read(old)==original,"controls save preserves existing settings bytes");
    auto invalid=profile; invalid.bindings[0].action=99;
    check(!mag_controls_save_profile(path,invalid,catalog) && read(path)==good,"unknown action rejects before replacing previous profile");
    invalid=profile; invalid.mouse.radians_per_count=std::numeric_limits<double>::quiet_NaN();
    check(!mag_controls_save_profile(path,invalid,catalog) && read(path)==good,"invalid sensitivity preserves previous profile");
    invalid=profile; invalid.mouse.scoped_gain=1e300;
    check(!mag_controls_save_profile(path,invalid,catalog) && read(path)==good,"unrepresentable scoped mouse gain rejected before runtime");
    invalid=profile; invalid.controller.ads_gain=1e300;
    check(!mag_controls_save_profile(path,invalid,catalog) && read(path)==good,"unrepresentable controller ADS rate rejected before runtime");
    invalid=profile; invalid.controller.inner_x=.9; invalid.controller.outer_x=.2;
    check(!mag_controls_save_profile(path,invalid,catalog) && read(path)==good,"invalid deadzones preserve previous profile");
    for(const std::string& bad:std::vector<std::string>{"{broken", "{}", good+"garbage"}) {
        write(path,bad); bool rejected=false;
        try { (void)mag_controls_load_profile(path,catalog); } catch(const std::exception&) { rejected=true; }
        check(rejected && read(path)==bad,"corrupt controls remain untouched for explicit recovery");
    }
    const auto changed=[&](const std::string& before,const std::string& after) {
        auto data=good; const auto found=data.find(before); check(found!=std::string::npos,"test payload key exists");
        data.replace(found,before.size(),after); return data;
    };
    const std::vector<std::string> malformed={
        changed("\"schema_version\":1","\"schema_version\":2"),
        changed("\"schema_version\":1","\"schema_version\":01"),
        changed("\"schema_version\":1","\"schema_version\":1e999"),
        changed("\"mode\":\"kbm\"","\"mode\":true"),
        changed("\"mode\":\"kbm\"","\"mode\":\"kbm\",\"mode\":\"auto\""),
        changed("\"contexts\":1","\"contexts\":1.5"),
        changed("\"response\":\"linear\"","\"response\":\"quadratic\""),
        std::string(262145,' '), "[[[[[[[[[[0]]]]]]]]]]"
    };
    for(const auto& data:malformed) {
        write(path,data); bool bad=false;
        try { (void)mag_controls_load_profile(path,catalog); } catch(const std::exception&) { bad=true; }
        check(bad && read(path)==data,"unsupported schema, duplicate/type/number/size/depth errors stay visible without overwriting stored data");
    }
    write(path,good);
    auto unknown=good; const auto at=unknown.find("\"action\":1"); check(at!=std::string::npos,"saved action field present");
    unknown.replace(at,10,"\"action\":99"); write(path,unknown);
    bool rejected=false; try { (void)mag_controls_load_profile(path,catalog); } catch(const std::exception&) { rejected=true; }
    check(rejected,"unknown stored action rejected against current catalog");
    write(path,good);
    const auto blocked=root/"blocked"; write(blocked,"parent is a file");
    check(!mag_controls_save_profile(blocked/"controls.json",profile,catalog),"atomic save failure reported");
    check(read(path)==good && read(blocked)=="parent is a file","failed save preserves existing files");
    for(const auto& item:std::filesystem::directory_iterator(root))
        check(item.path().filename().string().find(".next.")==std::string::npos,"temporary save files cleaned");
    bool exists=false; const auto old_loaded=load_player_settings(old,exists);
    check(exists && old_loaded.server_address=="example.test" && old_loaded.render_scale==200,"legacy settings still load unchanged");
    return 0;
 } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
