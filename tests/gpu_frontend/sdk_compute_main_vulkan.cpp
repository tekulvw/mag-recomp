/* Same Rust ownership/coherence probe as Metal, using authored SPIR-V. */
#include "mag_render_backend_bridge.h"
#include "frontend_shader_fixture.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
extern "C" void *mag_render_test_owner_open(void);
extern "C" const char *mag_render_test_owner_name(void);
extern "C" int mag_render_test_owner_hardware(uint32_t,uint32_t);
extern "C" int mag_render_test_owner_gate(void);
extern "C" void mag_render_test_owner_signal(void *);
extern "C" void mag_render_test_owner_close(int);
static bool identifier(const char *text,uint32_t &out) {
    char *end=nullptr;errno=0;unsigned long value=std::strtoul(text,&end,0);
    if (errno || !*text || *end || !value || value>std::numeric_limits<uint32_t>::max()) return false;
    out=uint32_t(value);return true;
}
static std::string json_string(const std::string &value) {
    const char hex[]="0123456789abcdef";std::string out="\"";
    for (unsigned char c:value) {
        if (c=='"' || c=='\\') {out+='\\';out+=char(c);}
        else if (c<32) {out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else out+=char(c);
    }
    return out+'"';
}
int main(int argc,char **argv) {
    uint32_t vendor=0,device_id=0;
    if (argc!=3 || !identifier(argv[1],vendor) || !identifier(argv[2],device_id)) {
        std::fprintf(stderr,"provide exact nonzero hardware vendor/device IDs\n");return 2;
    }
    auto *device=static_cast<rsx_frontend_device *>(mag_render_test_owner_open());
    if (!device) return 3;
    if (!mag_render_test_owner_hardware(vendor,device_id)) {mag_render_test_owner_close(1);return 3;}
    std::string name=mag_render_test_owner_name();
    MagRenderSdkApi api={};api.abi_version=1;api.struct_size=sizeof(api);
    if (mag_render_sdk_api(device,&api)!=0 || mag_render_test_owner_gate()!=0) {mag_render_test_owner_close(1);return 3;}
    const auto &shader=copy_transform_fixture;
    rsx_frontend_pipeline_desc pipeline={};pipeline.struct_size=sizeof(pipeline);pipeline.abi_version=1;
    pipeline.source_bytes=shader.source;pipeline.source_length=shader.length;
    pipeline.entry_point=shader.entry;pipeline.entry_point_length=uint32_t(std::strlen(shader.entry));
    pipeline.binding_count=shader.count;pipeline.bindings=shader.bindings;
    std::memcpy(pipeline.threads_per_group,shader.threads,sizeof(pipeline.threads_per_group));
    pipeline.shader_language=RSX_FRONTEND_SPIRV;
    MagRenderSdkProbeReport report={};report.abi_version=1;report.struct_size=sizeof(report);
    int status=mag_render_sdk_compute_probe(&api,&pipeline,mag_render_test_owner_signal,nullptr,&report);
    // Always signal the fixture-only host timeline semaphore, even on an error.
    mag_render_test_owner_signal(nullptr);mag_render_test_owner_close(report.pending==0);
    std::cout<<"{\"device\":"<<json_string(name)<<",\"vendor_id\":"<<vendor<<",\"device_id\":"<<device_id
        <<",\"status\":"<<status<<",\"submitted\":"<<report.submitted<<",\"completed\":"<<report.completed
        <<",\"pending\":"<<report.pending<<",\"retained_before_terminal\":"<<report.retained_before_terminal
        <<",\"retained_after_terminal\":"<<report.retained_after_terminal<<",\"upload_bytes\":"<<report.upload_bytes<<"}\n";
    return status==0 && report.submitted==3 && report.completed==3 && report.pending==0 &&
        report.retained_before_terminal==352 && report.retained_after_terminal==0 ? 0:1;
}
