// This backend records only the resource/coordinate boundary; the separate
// Metal regression checks real rasterization with synthetic geometry.
#include "libs/video/rsx_draw_engine.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
extern "C" const rsx_draw_backend *mag_player_render_backend(const rsx_draw_backend*) __attribute__((weak_import));
extern "C" void rsx_draw_engine_set_backend(const rsx_draw_backend*) {}
static u32 next_handle=1,last_w,last_h;
static std::vector<unsigned char> seed_copy;
static std::string fragment;
static float viewport[4]; static u32 rect[4];
static u32 color(void*,rsx_be_format,u32 w,u32 h,const void*seed,u32 pitch){
 last_w=w;last_h=h;seed_copy.clear();if(seed)for(u32 y=0;y<h;y++){
  const auto*p=(const unsigned char*)seed+y*pitch;seed_copy.insert(seed_copy.end(),p,p+w*4);}
 return next_handle++;
}
static u32 depth(void*,u32 w,u32 h){last_w=w;last_h=h;return next_handle++;}
static u32 snap(void*,u32){return next_handle++;}
static u32 dsnap(void*p,u32,u32 w,u32 h){return depth(p,w,h);}
static u32 dcolor(void*p,u32,u32 w,u32 h,u32){return depth(p,w,h);}
static u32 view(void*,u32,u32,u32){return next_handle++;}
static u32 zview(void*p,u32,u32 w,u32 h,u32,u32){return depth(p,w,h);}
static std::string host_fragment;
static u32 host_pipeline(void*,const char*,const char*ps,const rsx_be_render_state*,const rsx_vertex_layout_plan*,const u8*,rsx_be_format,u32){host_fragment=ps;return 1;}
static void release(void*,u32){}
static u32 pipeline(void*,const char*,const char*ps,const rsx_be_render_state*,const rsx_vertex_layout_plan*,u32,rsx_be_format,u32){fragment=ps;return 1;}
static u32 bound_target,bound_depth;
static void bind(void*,const u32*surfaces,u32 count,u32 depth){bound_target=count?surfaces[0]:0;bound_depth=depth;}
static void vp(void*,float x,float y,float w,float h){viewport[0]=x;viewport[1]=y;viewport[2]=w;viewport[3]=h;}
static void sc(void*,u32 x,u32 y,u32 w,u32 h){rect[0]=x;rect[1]=y;rect[2]=w;rect[3]=h;}
static void clear(void*p,u32,const float*,u32 x,u32 y,u32 w,u32 h){sc(p,x,y,w,h);}
static void dclear(void*p,u32,u32,float,u8,u32 x,u32 y,u32 w,u32 h){sc(p,x,y,w,h);}
static void read(void*,u32,u32 x,u32 y,u32 w,u32 h,void*out,u32 pitch){
 for(u32 j=0;j<h;j++)for(u32 i=0;i<w;i++){
  auto*p=(unsigned char*)out+j*pitch+i*4;p[0]=x+i;p[1]=y+j;p[2]=0;p[3]=255;}
}
static int dread(void*,u32,u32 x,u32 y,u32 w,u32 h,float*out,u32 pitch){
 for(u32 j=0;j<h;j++)for(u32 i=0;i<w;i++)*(float*)((u8*)out+j*pitch+i*4)=float(x+i+10*(y+j));return 0;
}
int main(int argc,char**argv){
 assert(mag_player_render_backend && "rendering resolution adapter is missing");
 assert(argc==2);setenv("MAG_PLAYER_RENDER_SCALE",!strcmp(argv[1],"oversize")?"300":argv[1],1);
 rsx_draw_backend base={};base.color_target_create=color;base.depth_target_create=depth;
 base.texture_release=release;base.color_target_release=release;base.depth_target_release=release;
 base.color_snapshot=snap;base.surface_view=view;base.depth_snapshot=dsnap;base.depth_as_color=dcolor;
 base.depth_z24_view=zview;base.pipeline_create_host=host_pipeline;
 base.pipeline_create=pipeline;base.bind_targets=bind;base.set_viewport=vp;base.set_scissor=sc;
 base.clear_color_rect=clear;base.clear_depth_stencil_rect=dclear;base.readback=read;base.depth_readback=dread;
 const auto*b=mag_player_render_backend(&base);
 if(!strcmp(argv[1],"100")){assert(b==&base);puts("native backend identity passed");return 0;}
 // Every image the adapter hands out for a guest-sized target region must
 // be on the same physical grid as the target itself, or a texture that
 // samples it in normalized coordinates reads the wrong part of the scene.
 assert(b->depth_z24_view&&b->depth_z24_view!=base.depth_z24_view);
 assert(b->pipeline_create_host&&b->pipeline_create_host!=base.pipeline_create_host);
 if(!strcmp(argv[1],"oversize")){b->depth_target_create(nullptr,16000,16000);assert(false);}
 const int percent=atoi(argv[1]);assert(percent==150||percent==200||percent==300);
 const u32 expected=percent==150?6:percent==200?8:12;
 unsigned char seeds[4*4*4];for(int i=0;i<16;i++)for(int c=0;c<4;c++)seeds[i*4+c]=i;
 const auto c=b->color_target_create(nullptr,RSX_BE_FMT_R8G8B8A8,4,4,seeds,16);
 assert(last_w==expected&&last_h==expected&&seed_copy.size()==expected*expected*4);
 assert(seed_copy.front()==0&&seed_copy.back()==15);
 const auto d=b->depth_target_create(nullptr,4,4);assert(last_w==expected&&last_h==expected);
 b->set_viewport(nullptr,0,0,4,4);assert(viewport[2]==expected&&viewport[3]==expected);
 b->set_scissor(nullptr,0,0,4,4);assert(rect[2]==expected&&rect[3]==expected);
 if(percent==150){
  b->set_scissor(nullptr,0,0,1,4);assert(rect[0]==0&&rect[2]==2);
  b->set_scissor(nullptr,1,0,3,4);assert(rect[0]==2&&rect[2]==4);
 }
 float rgba[4]={};b->clear_color_rect(nullptr,c,rgba,0,0,4,4);assert(rect[2]==expected);
 b->clear_depth_stencil_rect(nullptr,d,3,1,0,0,0,4,4);assert(rect[3]==expected);
 b->depth_snapshot(nullptr,d,4,4);assert(last_w==expected);
 b->depth_as_color(nullptr,d,4,4,0x85);assert(last_h==expected);
 // A depth target read as A8R8G8B8 words (a shadow map sampled as colour):
 // the view of its top-left guest rectangle covers the physical rectangle.
 b->depth_z24_view(nullptr,d,4,4,0xE4,0x85);assert(last_w==expected&&last_h==expected);
 b->depth_z24_view(nullptr,d,2,3,0xE4,0x85);
 assert(last_w==(2u*percent+50)/100&&last_h==(3u*percent+50)/100);
 // A wide shadow atlas and the scene target that samples it scale alike.
 const auto atlas=b->depth_target_create(nullptr,2048,1024);const u32 aw=last_w,ah=last_h;
 assert(aw==2048u*percent/100&&ah==1024u*percent/100);
 b->depth_z24_view(nullptr,atlas,2048,1024,0xE4,0x85);assert(last_w==aw&&last_h==ah);
 b->depth_snapshot(nullptr,atlas,2048,1024);assert(last_w==aw&&last_h==ah);
 const char*ps="float4 main(PSInput input) : SV_TARGET {\nreturn _rsx_wpos(input.position); }";
 b->pipeline_create(nullptr,"vs",ps,nullptr,nullptr,16,RSX_BE_FMT_R8G8B8A8,1);
 assert(fragment.find("input.position.xy /= ")!=std::string::npos);
 assert(fragment.find("input.position.xy /= ")<fragment.find("return _rsx_wpos"));
 const char*grid="float4 main(PSInput input) : SV_TARGET {\ninput.position.xy = floor(input.position.xy / float2(2, 1)) + 0.5;\nreturn _rsx_wpos(input.position); }";
 b->pipeline_create(nullptr,"vs",grid,nullptr,nullptr,16,RSX_BE_FMT_R8G8B8A8,1);
 assert(fragment.find("input.position.xy /= ")<fragment.find("input.position.xy = floor("));
 b->pipeline_create_host(nullptr,"vs",ps,nullptr,nullptr,nullptr,RSX_BE_FMT_R8G8B8A8,1);
 assert(host_fragment.find("input.position.xy /= ")<host_fragment.find("return _rsx_wpos"));
 unsigned char pixels[2*12];memset(pixels,0xCC,sizeof pixels);
 b->readback(nullptr,c,1,1,2,2,pixels,12);
 const int first=percent==150?2:percent==200?3:4;
 const int second=percent==150?3:percent==200?5:7;
 assert(pixels[0]==first&&pixels[1]==first&&pixels[4]==second&&pixels[12+1]==second);
 assert(pixels[8]==0xCC&&pixels[20]==0xCC);
 float depths[6]={};assert(!b->depth_readback(nullptr,d,1,1,2,2,depths,12));
 assert(depths[0]==float(first*11)&&depths[1]==float(second+10*first));
 assert(depths[3]==float(first+10*second));
 // Optional GPU packed-depth shortcut is disabled; the engine's existing
 // float readback/conversion path must see guest-sized depth instead.
 assert(!b->depth_store_z24&&!b->depth_z24_texture);
 // A guest size the scale does not divide (a 1x1 exposure target at 150%)
 // rounds to whole physical pixels. Everything drawn into, cleared in or
 // read from it follows that image's own ratio, so the whole physical image
 // is covered and a normalized sample of it finds the pixels drawn.
 const auto one=b->color_target_create(nullptr,RSX_BE_FMT_R8G8B8A8,1,1,nullptr,0);
 const u32 side=last_w;assert(side==u32(percent+50)/100&&last_h==side);
 const u32 handles[1]={one};b->bind_targets(nullptr,handles,1,0);assert(bound_target==one);
 b->set_viewport(nullptr,0,0,1,1);assert(viewport[0]==0&&viewport[2]==float(side)&&viewport[3]==float(side));
 b->set_scissor(nullptr,0,0,1,1);assert(rect[0]==0&&rect[1]==0&&rect[2]==side&&rect[3]==side);
 b->clear_color_rect(nullptr,one,rgba,0,0,1,1);assert(rect[2]==side&&rect[3]==side);
 unsigned char texel[4]={};b->readback(nullptr,one,0,0,1,1,texel,4);
 assert(texel[0]==side/2&&texel[1]==side/2);
 // Exactly scaled targets keep the nominal mapping when bound, depth-only
 // passes take their depth target's grid, and both clamp nothing.
 const u32 four[1]={c};b->bind_targets(nullptr,four,1,d);
 b->set_viewport(nullptr,0,0,4,4);assert(viewport[2]==expected&&viewport[3]==expected);
 b->bind_targets(nullptr,nullptr,0,atlas);assert(bound_depth==atlas);
 b->set_viewport(nullptr,0,0,2048,1024);assert(viewport[2]==float(aw)&&viewport[3]==float(ah));
 b->set_scissor(nullptr,1024,0,1024,1024);assert(rect[0]==aw/2&&rect[2]==aw-aw/2&&rect[3]==ah);
 assert(mag_player_render_backend(nullptr)==nullptr);
 puts("scaled resources, coordinates, WPOS and guest readbacks passed");
}
