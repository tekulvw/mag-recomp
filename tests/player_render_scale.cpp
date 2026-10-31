// This backend records only the resource/coordinate boundary; the separate
// Metal regression checks real rasterization with synthetic geometry.
#include "libs/video/rsx_draw_engine.h"
#include "player_render_scale.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
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
static void release(void*,u32){}
static u32 pipeline(void*,const char*,const char*ps,const rsx_be_render_state*,const rsx_vertex_layout_plan*,u32,rsx_be_format,u32){fragment=ps;return 1;}
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
 assert(argc==2);setenv("MAG_PLAYER_RENDER_SCALE",!strcmp(argv[1],"oversize")?"300":argv[1],1);
 rsx_draw_backend base={};base.color_target_create=color;base.depth_target_create=depth;
 base.texture_release=release;base.color_target_release=release;base.depth_target_release=release;
 base.color_snapshot=snap;base.surface_view=view;base.depth_snapshot=dsnap;base.depth_as_color=dcolor;
 base.pipeline_create=pipeline;base.set_viewport=vp;base.set_scissor=sc;
 base.clear_color_rect=clear;base.clear_depth_stencil_rect=dclear;base.readback=read;base.depth_readback=dread;
 const auto*b=mag_player_render_backend(&base);
 if(!strcmp(argv[1],"100")){assert(b==&base);puts("native backend identity passed");return 0;}
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
 const char*ps="float4 main(PSInput input) : SV_TARGET {\nreturn _rsx_wpos(input.position); }";
 b->pipeline_create(nullptr,"vs",ps,nullptr,nullptr,16,RSX_BE_FMT_R8G8B8A8,1);
 assert(fragment.find("input.position.xy /= ")!=std::string::npos);
 assert(fragment.find("input.position.xy /= ")<fragment.find("return _rsx_wpos"));
 const char*grid="float4 main(PSInput input) : SV_TARGET {\ninput.position.xy = floor(input.position.xy / float2(2, 1)) + 0.5;\nreturn _rsx_wpos(input.position); }";
 b->pipeline_create(nullptr,"vs",grid,nullptr,nullptr,16,RSX_BE_FMT_R8G8B8A8,1);
 assert(fragment.find("input.position.xy /= ")<fragment.find("input.position.xy = floor("));
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
 assert(mag_player_render_backend(nullptr)==nullptr);
 puts("scaled resources, coordinates, WPOS and guest readbacks passed");
}
