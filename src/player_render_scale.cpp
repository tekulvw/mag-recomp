#include "player_render_scale.h"
#include "libs/video/rsx_draw_engine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
// Guest memory and the draw engine retain their original dimensions. Only the
// backend's GPU images and raster coordinates use this physical sample grid.
struct Image { u32 w,h,pw,ph,bytes; };
std::unordered_map<u32,Image> images;
rsx_draw_backend original,scaled;
u32 percent=100;
constexpr u32 maxDimension=16384;
constexpr size_t maxTransfer=512ull*1024*1024;

[[noreturn]] void fail(const char *why) {
    std::fprintf(stderr,"[MAG rendering resolution] %s\n",why);
    std::fflush(stderr);std::_Exit(86);
}
u32 dimension(u32 n) {
    const u64 value=((u64)n*percent+50)/100;
    if(!n||!value||value>maxDimension)fail("render target dimensions exceed the supported GPU limit");
    return (u32)value;
}
u32 bytesPerPixel(rsx_be_format fmt) {
    switch(fmt){
    case RSX_BE_FMT_R8:return 1;
    case RSX_BE_FMT_R8G8:case RSX_BE_FMT_R16:return 2;
    case RSX_BE_FMT_R8G8B8A8:case RSX_BE_FMT_R16G16:case RSX_BE_FMT_R16G16F:case RSX_BE_FMT_R32F:return 4;
    case RSX_BE_FMT_R16G16B16A16F:return 8;
    case RSX_BE_FMT_R32G32B32A32F:return 16;
    default:fail("unsupported render target storage format");
    }
}
std::vector<u8> buffer(u32 w,u32 h,u32 bytes) {
    const u64 length=(u64)w*h*bytes;
    if(!w||!h||!bytes||length>maxTransfer)fail("render target transfer exceeds its bounded allocation");
    try{return std::vector<u8>((size_t)length);}
    catch(...){fail("render target transfer allocation failed");}
}
std::vector<u8> expand(const void *data,u32 pitch,u32 w,u32 h,u32 pw,u32 ph,u32 bytes) {
    if((u64)w*bytes>pitch)fail("invalid render target seed stride");
    auto out=buffer(pw,ph,bytes);
    for(u32 y=0;y<ph;y++)for(u32 x=0;x<pw;x++){
        const u32 sx=std::min(w-1,(u32)(((u64)x*2+1)*w/(2ull*pw)));
        const u32 sy=std::min(h-1,(u32)(((u64)y*2+1)*h/(2ull*ph)));
        std::memcpy(out.data()+((size_t)y*pw+x)*bytes,(const u8*)data+(size_t)sy*pitch+(size_t)sx*bytes,bytes);
    }
    return out;
}
Image &image(u32 handle) {
    auto it=images.find(handle);if(it==images.end())fail("unknown render target handle");return it->second;
}
u32 remember(u32 handle,const Image &im){if(handle)images[handle]=im;return handle;}
u32 targetCreated(u32 handle,const Image &im,const char *kind) {
    static unsigned reports;
    if(handle&&(im.w>=960||im.h>=540)&&reports<12){
        ++reports;
        std::fprintf(stderr,"[MAG rendering resolution] %s target %ux%u -> GPU %ux%u\n",kind,im.w,im.h,im.pw,im.ph);
    }
    return remember(handle,im);
}
u32 colorCreate(void *user,rsx_be_format fmt,u32 w,u32 h,const void *seed,u32 pitch) {
    const Image im={w,h,dimension(w),dimension(h),bytesPerPixel(fmt)};
    std::vector<u8> rows;if(seed)rows=expand(seed,pitch,w,h,im.pw,im.ph,im.bytes);
    return targetCreated(original.color_target_create(user,fmt,im.pw,im.ph,seed?rows.data():nullptr,
                    seed?im.pw*im.bytes:0),im,"color");
}
u32 depthCreate(void *user,u32 w,u32 h) {
    const Image im={w,h,dimension(w),dimension(h),4};
    return targetCreated(original.depth_target_create(user,im.pw,im.ph),im,"depth");
}
void colorRelease(void*u,u32 h){images.erase(h);original.color_target_release(u,h);}
void depthRelease(void*u,u32 h){images.erase(h);original.depth_target_release(u,h);}
void textureRelease(void*u,u32 h){images.erase(h);original.texture_release(u,h);}
u32 surfaceView(void*u,u32 h,u32 remap,u32 format) {
    const Image im=image(h);return remember(original.surface_view(u,h,remap,format),im);
}
u32 colorSnapshot(void*u,u32 h){const Image im=image(h);return remember(original.color_snapshot(u,h),im);}
u32 depthSnapshot(void*u,u32 h,u32 w,u32 ht) {
    const Image im={w,ht,dimension(w),dimension(ht),4};
    return remember(original.depth_snapshot(u,h,im.pw,im.ph),im);
}
template<class Backend> u32 depthAsColor(void*u,u32 h,u32 w,u32 ht,u32 format) {
    const Image im={w,ht,dimension(w),dimension(ht),format==0x92?2u:4u};
    return remember(static_cast<Backend&>(original).depth_as_color(u,h,im.pw,im.ph,format),im);
}
u32 edge(u32 n){const u64 v=((u64)n*percent+50)/100;if(v>maxDimension)fail("invalid render target rectangle");return (u32)v;}
void rectangle(u32 &x,u32 &y,u32 &w,u32 &h) {
    if((u64)x+w>maxDimension||(u64)y+h>maxDimension)fail("invalid render target rectangle");
    const u32 right=edge(x+w),bottom=edge(y+h);x=edge(x);y=edge(y);w=right-x;h=bottom-y;
}
void viewport(void*u,float x,float y,float w,float h) {
    const float s=percent/100.0f;original.set_viewport(u,x*s,y*s,w*s,h*s);
}
void scissor(void*u,u32 x,u32 y,u32 w,u32 h){rectangle(x,y,w,h);original.set_scissor(u,x,y,w,h);}
template<class Backend> void colorRect(void*u,u32 surface,const float *rgba,u32 x,u32 y,u32 w,u32 h) {
    rectangle(x,y,w,h);static_cast<Backend&>(original).clear_color_rect(u,surface,rgba,x,y,w,h);
}
template<class Backend> void depthRect(void*u,u32 depth,u32 flags,float value,u8 stencil,u32 x,u32 y,u32 w,u32 h) {
    rectangle(x,y,w,h);static_cast<Backend&>(original).clear_depth_stencil_rect(u,depth,flags,value,stencil,x,y,w,h);
}
void colorWrite(void*u,u32 h,u32 x,u32 y,u32 w,u32 ht,const void *rows,u32 pitch) {
    const Image im=image(h);u32 px=x,py=y,pw=w,ph=ht;rectangle(px,py,pw,ph);
    if(x>im.w||y>im.h||w>im.w-x||ht>im.h-y)fail("render target write is outside its guest extent");
    auto physical=expand(rows,pitch,w,ht,pw,ph,im.bytes);
    original.color_target_write(u,h,px,py,pw,ph,physical.data(),pw*im.bytes);
}
// Fragment coordinates remain in guest pixels. This runs before the existing
// RSX AA-grid and window-origin transforms, preserving both those conventions
// and the fractional locations of the additional host samples.
u32 pipeline(void*u,const char *vs,const char *ps,const rsx_be_render_state *state,
             const rsx_vertex_layout_plan *layout,u32 stride,rsx_be_format fmt,u32 count) {
    std::string text=ps;
    if(text.find("input.position")!=std::string::npos){
        const size_t main=text.find("main(PSInput input)");
        const size_t body=main==std::string::npos?std::string::npos:text.find('{',main);
        if(body==std::string::npos)fail("fragment coordinates use an unsupported shader entry point");
        char line[96];std::snprintf(line,sizeof line,"\n    input.position.xy /= %.2f;\n",percent/100.0);
        text.insert(body+1,line);
    }
    return original.pipeline_create(u,vs,text.c_str(),state,layout,stride,fmt,count);
}
struct Region { u32 x,y,w,h; };
Region readRegion(const Image &im,u32 x,u32 y,u32 w,u32 h,u32 pitch) {
    if(!w||!h||x>=im.w||y>=im.h||w>im.w-x||h>im.h-y||(u64)w*im.bytes>pitch)
        fail("invalid guest render target readback");
    const u32 left=(u32)((u64)x*percent/100),top=(u32)((u64)y*percent/100);
    const u32 right=std::min(im.pw,(u32)(((u64)(x+w)*percent+99)/100));
    const u32 bottom=std::min(im.ph,(u32)(((u64)(y+h)*percent+99)/100));
    return {left,top,right-left,bottom-top};
}
void reduce(const Image &im,const Region &r,const u8 *physical,u32 x,u32 y,u32 w,u32 h,void*out,u32 pitch) {
    for(u32 j=0;j<h;j++)for(u32 i=0;i<w;i++){
        const u32 px=std::min(im.pw-1,(u32)(((u64)(x+i)*2+1)*percent/200));
        const u32 py=std::min(im.ph-1,(u32)(((u64)(y+j)*2+1)*percent/200));
        std::memcpy((u8*)out+(size_t)j*pitch+(size_t)i*im.bytes,
                    physical+((size_t)(py-r.y)*r.w+px-r.x)*im.bytes,im.bytes);
    }
}
void readback(void*u,u32 h,u32 x,u32 y,u32 w,u32 ht,void*out,u32 pitch) {
    const Image im=image(h);const Region r=readRegion(im,x,y,w,ht,pitch);auto pixels=buffer(r.w,r.h,im.bytes);
    original.readback(u,h,r.x,r.y,r.w,r.h,pixels.data(),r.w*im.bytes);
    reduce(im,r,pixels.data(),x,y,w,ht,out,pitch);
}
int depthReadback(void*u,u32 h,u32 x,u32 y,u32 w,u32 ht,float*out,u32 pitch) {
    const Image im=image(h);const Region r=readRegion(im,x,y,w,ht,pitch);auto pixels=buffer(r.w,r.h,4);
    const int status=original.depth_readback(u,h,r.x,r.y,r.w,r.h,(float*)pixels.data(),r.w*4);
    if(!status)reduce(im,r,pixels.data(),x,y,w,ht,out,pitch);
    return status;
}
// Newer SDKs supply these optional paths. Keep the pinned public runtime
// compatible while preserving exact rectangle/depth-view behavior when present.
template<class Backend> void optionalCallbacks(Backend &target,const Backend &source) {
    if constexpr(requires { source.depth_as_color; })
        if(source.depth_as_color)target.depth_as_color=depthAsColor<Backend>;
    if constexpr(requires { source.clear_color_rect; })
        if(source.clear_color_rect)target.clear_color_rect=colorRect<Backend>;
    if constexpr(requires { source.clear_depth_stencil_rect; })
        if(source.clear_depth_stencil_rect)target.clear_depth_stencil_rect=depthRect<Backend>;
}
void shutdown(void*u){if(original.shutdown)original.shutdown(u);images.clear();}
}

extern "C" const rsx_draw_backend *mag_player_render_backend(const rsx_draw_backend *backend) {
    if(!backend){images.clear();return nullptr;}
    const char *value=std::getenv("MAG_PLAYER_RENDER_SCALE");
    if(!value||!std::strcmp(value,"100"))return backend;
    if(std::strcmp(value,"150")&&std::strcmp(value,"200")&&std::strcmp(value,"300"))
        fail("invalid rendering resolution setting");
    percent=(u32)std::atoi(value);original=*backend;scaled=*backend;images.clear();
    if(!original.color_target_create||!original.depth_target_create||!original.pipeline_create||
       !original.set_viewport||!original.set_scissor||!original.readback||!original.depth_readback||
       !original.color_target_release||!original.depth_target_release||!original.texture_release)
        fail("Selected backend does not support rendering resolution scaling");
    scaled.shutdown=shutdown;scaled.color_target_create=colorCreate;scaled.depth_target_create=depthCreate;
    scaled.color_target_release=colorRelease;scaled.depth_target_release=depthRelease;scaled.texture_release=textureRelease;
    if(original.color_target_write)scaled.color_target_write=colorWrite;
    if(original.surface_view)scaled.surface_view=surfaceView;
    if(original.color_snapshot)scaled.color_snapshot=colorSnapshot;
    if(original.depth_snapshot)scaled.depth_snapshot=depthSnapshot;
    optionalCallbacks(scaled,original);
    scaled.pipeline_create=pipeline;scaled.set_viewport=viewport;scaled.set_scissor=scissor;
    scaled.readback=readback;scaled.depth_readback=depthReadback;
    // The packed-depth shortcut operates directly in guest coordinates. Use
    // the existing float readback + guest conversion fallback at higher scale.
    scaled.depth_store_z24=nullptr;scaled.depth_z24_texture=nullptr;
    std::fprintf(stderr,"[MAG rendering resolution] GPU render targets at %u%%; output size is independent\n",percent);
    return &scaled;
}
extern "C" void mag_player_set_render_backend(const rsx_draw_backend *backend) {
    rsx_draw_engine_set_backend(mag_player_render_backend(backend));
}
