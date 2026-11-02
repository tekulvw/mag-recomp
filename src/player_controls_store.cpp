#include "player_controls_store.h"
#include "player_preferences_file.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>
namespace mag {
namespace {
[[noreturn]] void invalid() { throw std::runtime_error("Controls preferences contain damaged or unsupported data."); }
struct Json {
    enum Kind { Object, Array, String, Number, Boolean } kind=Object;
    std::map<std::string,Json> object;
    std::vector<Json> array;
    std::string text;
    double number=0;
    bool boolean=false;
};
Json object(std::initializer_list<std::pair<const std::string,Json>> fields) { Json j; j.object=fields; return j; }
Json number(double n) { Json j; j.kind=Json::Number; j.number=n; return j; }
Json text(std::string s) { Json j; j.kind=Json::String; j.text=std::move(s); return j; }
Json boolean(bool b) { Json j; j.kind=Json::Boolean; j.boolean=b; return j; }
struct Parser {
    std::string_view input; size_t at=0, tokens=0;
    void space() { while(at<input.size() && (input[at]==' ' || input[at]=='\r' || input[at]=='\n' || input[at]=='\t')) ++at; }
    bool take(char c) { space(); if(at<input.size() && input[at]==c) { ++at; return true; } return false; }
    void need(char c) { if(!take(c)) invalid(); }
    std::string string() {
        need('"'); std::string result;
        while(at<input.size()) {
            unsigned char c=input[at++];
            if(c=='"') return result;
            if(c<32 || c>=128) invalid();
            if(c=='\\') {
                if(at==input.size()) invalid(); c=input[at++];
                if(c=='u') {
                    unsigned code=0;
                    for(unsigned i=0;i<4;++i) {
                        if(at==input.size()) invalid(); const char h=input[at++];
                        const unsigned n=h>='0' && h<='9' ? h-'0' : h>='a' && h<='f' ? h-'a'+10 : h>='A' && h<='F' ? h-'A'+10 : 16;
                        if(n==16) invalid(); code=code*16+n;
                    }
                    if(code<32 || code>=128) invalid(); c=static_cast<unsigned char>(code);
                } else if(c!='"' && c!='\\' && c!='/') invalid();
            }
            result+=static_cast<char>(c);
        }
        invalid();
    }
    Json parse(unsigned depth=0) {
        if(depth>8 || ++tokens>16384) invalid();
        space(); if(at==input.size()) invalid();
        if(take('{')) {
            Json j;
            if(take('}')) return j;
            do { auto key=string(); need(':'); auto value=parse(depth+1); if(!j.object.emplace(key,std::move(value)).second) invalid(); } while(take(','));
            need('}'); return j;
        }
        if(take('[')) {
            Json j; j.kind=Json::Array;
            if(take(']')) return j;
            do { j.array.push_back(parse(depth+1)); } while(take(','));
            need(']'); return j;
        }
        if(input[at]=='"') return text(string());
        if(input.substr(at,4)=="true") { at+=4; return boolean(true); }
        if(input.substr(at,5)=="false") { at+=5; return boolean(false); }
        const size_t begin=at;
        if(input[at]=='-') ++at;
        auto digits=[&] { const size_t start=at; while(at<input.size() && input[at]>='0' && input[at]<='9') ++at; if(at==start) invalid(); };
        if(at<input.size() && input[at]=='0') ++at; else digits();
        if(at<input.size() && input[at]=='.') { ++at; digits(); }
        if(at<input.size() && (input[at]=='e' || input[at]=='E')) { ++at; if(at<input.size() && (input[at]=='+' || input[at]=='-')) ++at; digits(); }
        std::istringstream stream(std::string(input.substr(begin,at-begin))); stream.imbue(std::locale::classic());
        double value; stream>>value;
        if(!stream || stream.peek()!=std::char_traits<char>::eof() || !std::isfinite(value)) invalid();
        auto result=number(value); result.text=std::string(input.substr(begin,at-begin)); return result;
    }
};
void fields(const Json& j,std::initializer_list<const char*> names) {
    if(j.kind!=Json::Object || j.object.size()!=names.size()) invalid();
    for(auto name:names) if(!j.object.contains(name)) invalid();
}
const Json& get(const Json& j,const char* name) { auto it=j.object.find(name); if(j.kind!=Json::Object || it==j.object.end()) invalid(); return it->second; }
double num(const Json& j) { if(j.kind!=Json::Number) invalid(); return j.number; }
uint32_t integer(const Json& j) {
    // IDs, codes, masks and the schema use canonical unsigned decimal tokens,
    // as the existing preferences do. Never round a fractional token into an ID.
    if(j.kind!=Json::Number || j.text.empty() || !std::all_of(j.text.begin(),j.text.end(),[](char c){return c>='0' && c<='9';})) invalid();
    uint32_t value=0; const auto parsed=std::from_chars(j.text.data(),j.text.data()+j.text.size(),value);
    if(parsed.ec!=std::errc() || parsed.ptr!=j.text.data()+j.text.size()) invalid();
    return value;
}
bool flag(const Json& j) { if(j.kind!=Json::Boolean) invalid(); return j.boolean; }
const std::string& str(const Json& j) { if(j.kind!=Json::String) invalid(); return j.text; }
void validate(const ControlsProfile& profile,std::span<const ActionId> catalog) {
    mag_controls_validate_profile(profile);
    for(const auto& binding:profile.bindings)
        if(std::find(catalog.begin(),catalog.end(),binding.action)==catalog.end()) invalid();
}
std::string quote(const std::string& s) { std::string out="\""; for(char c:s) { if(c=='"' || c=='\\') out+='\\'; out+=c; } return out+'"'; }
void encode(std::ostream& out,const Json& j) {
    switch(j.kind) {
    case Json::Object: { out<<'{'; bool first=true; for(const auto& [key,value]:j.object) { if(!first) out<<','; first=false; out<<quote(key)<<':'; encode(out,value); } out<<'}'; break; }
    case Json::Array: { out<<'['; bool first=true; for(const auto& value:j.array) { if(!first) out<<','; first=false; encode(out,value); } out<<']'; break; }
    case Json::String: out<<quote(j.text); break;
    case Json::Number: out<<std::setprecision(std::numeric_limits<double>::max_digits10)<<j.number; break;
    case Json::Boolean: out<<(j.boolean ? "true" : "false"); break;
    }
}
Json payload(const ControlsProfile& p) {
    const auto& m=p.mouse; const auto& c=p.controller;
    Json bindings; bindings.kind=Json::Array;
    for(const auto& b:p.bindings) bindings.array.push_back(object({{"source",text(b.control.source==InputSource::Keyboard ? "keyboard" : b.control.source==InputSource::Mouse ? "mouse" : "controller")},
        {"code",number(b.control.code)},{"action",number(b.action)},{"contexts",number(b.contexts)},{"behavior",text(b.behavior==BindingBehavior::Hold ? "hold" : "toggle")}}));
    return object({{"schema_version",number(3)},{"catalog_revision",number(p.catalog_revision)},{"controller_remap",boolean(p.controller_remap)},{"mode",text(p.mode==InputMode::Auto ? "auto" : p.mode==InputMode::KBM ? "kbm" : "controller")},
        {"mouse",object({{"radians_per_count",number(m.radians_per_count)},{"x_gain",number(m.x_gain)},{"y_gain",number(m.y_gain)},
                        {"hip_gain",number(m.hip_gain)},{"ads_gain",number(m.ads_gain)},{"scoped_gain",number(m.scoped_gain)},{"invert_y",boolean(m.invert_y)}})},
        {"controller",object({{"response",text(c.response==ControllerResponse::Native ? "native" : "linear")},
            {"inner_x",number(c.inner_x)},{"inner_y",number(c.inner_y)},{"outer_x",number(c.outer_x)},{"outer_y",number(c.outer_y)},
            {"x_gain",number(c.x_gain)},{"y_gain",number(c.y_gain)},{"hip_gain",number(c.hip_gain)},{"ads_gain",number(c.ads_gain)},{"scoped_gain",number(c.scoped_gain)},
            {"invert_x",boolean(c.invert_x)},{"invert_y",boolean(c.invert_y)}})}, {"bindings",std::move(bindings)}});
}
ControlsProfile decode(const std::string& bytes) {
    Parser parser{bytes}; const auto root=parser.parse(); parser.space(); if(parser.at!=bytes.size()) invalid();
    const auto schema=integer(get(root,"schema_version"));
    if(schema==1) fields(root,{"schema_version","mode","mouse","controller","bindings"});
    else if(schema==2) fields(root,{"schema_version","mode","mouse","controller","bindings","controller_remap"});
    else if(schema==3) fields(root,{"schema_version","mode","mouse","controller","bindings","controller_remap","catalog_revision"});
    else invalid();
    ControlsProfile p; if(schema>=2)p.controller_remap=flag(get(root,"controller_remap"));
    if(schema==3)p.catalog_revision=integer(get(root,"catalog_revision"));
    const auto& mode=str(get(root,"mode"));
    if(mode=="auto") p.mode=InputMode::Auto; else if(mode=="controller") p.mode=InputMode::Controller; else if(mode=="kbm") p.mode=InputMode::KBM; else invalid();
    const auto& m=get(root,"mouse"); fields(m,{"radians_per_count","x_gain","y_gain","hip_gain","ads_gain","scoped_gain","invert_y"});
    p.mouse={num(get(m,"radians_per_count")),num(get(m,"x_gain")),num(get(m,"y_gain")),num(get(m,"hip_gain")),num(get(m,"ads_gain")),num(get(m,"scoped_gain")),flag(get(m,"invert_y"))};
    const auto& c=get(root,"controller"); fields(c,{"response","inner_x","inner_y","outer_x","outer_y","x_gain","y_gain","hip_gain","ads_gain","scoped_gain","invert_x","invert_y"});
    const auto& response=str(get(c,"response")); if(response=="native") p.controller.response=ControllerResponse::Native; else if(response=="linear") p.controller.response=ControllerResponse::Linear; else invalid();
    p.controller.inner_x=num(get(c,"inner_x")); p.controller.inner_y=num(get(c,"inner_y")); p.controller.outer_x=num(get(c,"outer_x")); p.controller.outer_y=num(get(c,"outer_y"));
    p.controller.x_gain=num(get(c,"x_gain")); p.controller.y_gain=num(get(c,"y_gain")); p.controller.hip_gain=num(get(c,"hip_gain")); p.controller.ads_gain=num(get(c,"ads_gain")); p.controller.scoped_gain=num(get(c,"scoped_gain"));
    p.controller.invert_x=flag(get(c,"invert_x")); p.controller.invert_y=flag(get(c,"invert_y"));
    const auto& bindings=get(root,"bindings"); if(bindings.kind!=Json::Array || bindings.array.size()>1024) invalid();
    for(const auto& item:bindings.array) {
        fields(item,{"source","code","action","contexts","behavior"}); ControlBinding b;
        const auto& source=str(get(item,"source")); if(source=="keyboard") b.control.source=InputSource::Keyboard; else if(source=="mouse") b.control.source=InputSource::Mouse; else if(source=="controller") b.control.source=InputSource::Controller; else invalid();
        b.control.code=integer(get(item,"code")); b.action=integer(get(item,"action")); b.contexts=integer(get(item,"contexts"));
        const auto& behavior=str(get(item,"behavior")); if(behavior=="hold") b.behavior=BindingBehavior::Hold; else if(behavior=="toggle") b.behavior=BindingBehavior::Toggle; else invalid();
        p.bindings.push_back(b);
    }
    return p;
}
}
ControlsProfile mag_controls_load_profile(const std::filesystem::path& path,std::span<const ActionId> catalog) {
    const auto bytes=read_player_preferences_file(path,262144);
    if(!bytes) return {};
    auto profile=decode(*bytes); validate(profile,catalog); return profile;
}
bool mag_controls_save_profile(const std::filesystem::path& path,const ControlsProfile& profile,std::span<const ActionId> catalog) {
    try { validate(profile,catalog); std::ostringstream out; out.imbue(std::locale::classic()); encode(out,payload(profile)); out<<'\n'; write_player_preferences_file(path,out.str()); return true; }
    catch(const std::exception&) { return false; }
}
}
