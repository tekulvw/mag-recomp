/* Compiler-only fixture: real DXC, authored generated HLSL, no device/window. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../native/mag_render_dxc.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

static uint32_t little32(const unsigned char *p) {
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
/* Official DXIL container header/part offsets and program version layout:
 * microsoft/DirectXShaderCompiler include/dxc/DxilContainer/DxilContainer.h. */
static bool has_dxil(const mag_render_dxc_blob &b,uint32_t expected_kind) {
    if (b.length < 32 || std::memcmp(b.bytes, "DXBC", 4)) return false;
    const auto *s=static_cast<const unsigned char *>(b.bytes);
    const uint32_t parts=little32(s+28);
    if(little32(s+24)!=b.length || parts>(b.length-32)/4)return false;
    for(uint32_t i=0;i<parts;++i){
        const uint32_t offset=little32(s+32+4*i);
        if(offset<32+4*uint64_t(parts) || uint64_t(offset)+12>b.length)return false;
        if(!std::memcmp(s+offset,"DXIL",4)){
            const uint32_t size=little32(s+offset+4),version=little32(s+offset+8);
            return size>=24 && uint64_t(offset)+8+size<=b.length &&
                (version>>16)==expected_kind && (version&0xff)==0x60;
        }
    }
    return false;
}
int main(int argc, char **argv) {
    if (argc!=4 && argc!=6) {std::fprintf(stderr,"usage: test_dxc ABSOLUTE_DXC_LIBRARY TRANSFORM_HLSL COMPACT_HLSL [VERTEX_HLSL FRAGMENT_HLSL]\n");return 2;}
    char diagnostic[4096]={}; mag_render_dxc_blob object={};
    const char shader[]="[numthreads(1,1,1)] void main() {}";
    assert(mag_render_dxc_compile("relative-dxcompiler",shader,sizeof(shader)-1,"main",4,&object,diagnostic,sizeof(diagnostic))==MAG_RENDER_DXC_INVALID);
    assert(!object.bytes && !object.length);
    assert(mag_render_dxc_compile(argv[1],shader,sizeof(shader)-1,"main -T ps_6_0",14,&object,diagnostic,sizeof(diagnostic))==MAG_RENDER_DXC_INVALID);
    assert(mag_render_dxc_compile_stage(argv[1],UINT32_MAX,shader,sizeof(shader)-1,"main",4,&object,diagnostic,sizeof(diagnostic))==MAG_RENDER_DXC_INVALID);
    for (int i=2;i<argc;++i) {
        std::ifstream input(argv[i],std::ios::binary); assert(input);
        const std::string source((std::istreambuf_iterator<char>(input)),{});
        const char *entry=i==2?"copy_transform":i==3?"compact":i==4?"scene_vertex":"scene_fragment";
        const uint32_t stage=i<4?MAG_RENDER_DXC_COMPUTE:i==4?MAG_RENDER_DXC_VERTEX:MAG_RENDER_DXC_FRAGMENT;
        const int status=mag_render_dxc_compile_stage(argv[1],stage,source.data(),source.size(),entry,static_cast<uint32_t>(std::strlen(entry)),&object,diagnostic,sizeof(diagnostic));
        if(status!=MAG_RENDER_DXC_OK){std::fprintf(stderr,"DXC %s status=%d: %s\n",entry,status,diagnostic);return 3;}
        assert(has_dxil(object,i<4?5:i==4?1:0));
        std::printf("DXC %s %s: validated typed DXIL %llu bytes\n",i<4?"cs_6_0":i==4?"vs_6_0":"ps_6_0",entry,static_cast<unsigned long long>(object.length));
        mag_render_dxc_free(&object); assert(!object.bytes && !object.length);
    }
    const char invalid[]="[numthreads(1,1,1)] void main() { undefined_identifier(); }";
    assert(mag_render_dxc_compile(argv[1],invalid,sizeof(invalid)-1,"main",4,&object,diagnostic,sizeof(diagnostic))==MAG_RENDER_DXC_INVALID);
    assert(!object.bytes && !object.length && diagnostic[0]);
    std::puts("Invalid HLSL rejected by actual compiler; no DXIL returned");
    return 0;
}
