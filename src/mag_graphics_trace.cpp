#include "mag_jobio_selector.h"
#include "mag_graphics_trace.h"
#include "diagnostics/critical_trace.h"
#include "diagnostics/critical_write_trace.h"
#include <atomic>
#include <exception>
#include <errno.h>
#include <unistd.h>
static std::atomic<uint64_t> sequence{0};
static thread_local uint64_t current_span;
struct PredState {uint64_t capture,ctx,object,array,target,result,markers0,markers1;};
static thread_local PredState last[2];
MagGraphicsTraceScope::MagGraphicsTraceScope(ppu_context *c,uint32_t f):ctx(c),function(f),start(0),token(0),capture(0),parent(0),predicate(f==0xA047D8||f==0xA05438),exceptions(0),marker_mask(0),wait_identity_valid(false){
 if(auto enter=mag_extension()->trace_enter){const int ext_errno=errno;for(int i=0;i<3;i++)ext_args[i]=c->gpr[3+i];ext_exceptions=std::uncaught_exceptions();enter(c,f,ext_args,ext_scratch);errno=ext_errno;}
 if(!ps3_critical_enabled())return;
 int saved=errno;capture=ps3_critical_capture_id();for(int i=0;i<3;i++)args[i]=c->gpr[3+i];
 if(!predicate){start=ps3_critical_now_ns();token=++sequence;parent=current_span;current_span=token;exceptions=std::uncaught_exceptions();}
 if(f==0x23D7D0){const uint32_t job=(uint32_t)args[0],module=vm_read32(job+0x288);if(module==0x1f6b930||module==0x1f6ba00||module==0x1f6c3c0||module==0x1f6c490){const uint32_t container=vm_read32(job+0x2a4),ticket=vm_read16(job+0x2a0);uint64_t identity[8]={module,job,vm_read64(job+0x280),container,vm_read32(container),ticket,vm_read32(container+0x2c),ticket?vm_read32(container+0x20)+8ull*(ticket-1):0};for(unsigned i=0;i<8;i++)wait_identity[i]=identity[i];wait_identity_valid=true;if(module==0x1f6c3c0||module==0x1f6c490)ps3_critical_emit_capture(capture,105,(uint64_t)ctx,token,identity,8);}}
 errno=saved;
}
MagGraphicsTraceScope::~MagGraphicsTraceScope(){
 const int caller_errno=errno;
 if(auto leave=mag_extension()->trace_exit)leave(ctx,function,ext_args,std::uncaught_exceptions()==ext_exceptions,ext_scratch);
 errno=caller_errno;
 if(!capture)return;int saved=errno;
 if(!predicate)current_span=parent;
 if(!ps3_critical_enabled()||capture!=ps3_critical_capture_id()){errno=saved;return;}
 if(predicate){if(function==0xA047D8)ps3_critical_watch_bind(capture,(uint64_t)ctx,(uint32_t)args[0],(uint32_t)args[1],(uint32_t)ctx->gpr[4]);auto &p=last[function==0xA05438];uint64_t result=ctx->gpr[3];
  if(p.capture!=capture||p.ctx!=(uint64_t)ctx||p.object!=args[0]||p.array!=args[1]||p.target!=args[2]||p.result!=result||(marker_mask==127&&(p.markers0!=((uint64_t)marker_values[0]<<48|((uint64_t)marker_values[1]<<32)|((uint64_t)marker_values[2]<<16)|marker_values[3])||p.markers1!=((uint64_t)marker_values[4]<<32|((uint64_t)marker_values[5]<<16)|marker_values[6])))){
   uint64_t m0=0,m1=0;if(marker_mask==127){m0=(uint64_t)marker_values[0]<<48|((uint64_t)marker_values[1]<<32)|((uint64_t)marker_values[2]<<16)|marker_values[3];m1=(uint64_t)marker_values[4]<<32|((uint64_t)marker_values[5]<<16)|marker_values[6];}
   p={capture,(uint64_t)ctx,args[0],args[1],args[2],result,m0,m1};
   if(function==0xA047D8){uint64_t markers[8]={args[0],args[1],args[2],result,m0,m1,marker_mask,current_span};ps3_critical_emit_capture(capture,39,(uint64_t)ctx,0,markers,8);}uint64_t w[8]={function,args[0],args[1],args[2],result,ctx->gpr[0],current_span,ctx->gpr[4]};ps3_critical_emit_capture(capture,PS3_CRITICAL_PREDICATE,(uint64_t)ctx,0,w,8);
  }
 }else{
  if(wait_identity_valid){const uint32_t job=(uint32_t)wait_identity[1];if(vm_read32(job+0x288)!=wait_identity[0]||vm_read32(job+0x2a4)!=wait_identity[3]||vm_read16(job+0x2a0)!=wait_identity[5]||vm_read64(job+0x280)!=wait_identity[2])wait_identity[6]|=1ull<<63;if(wait_identity[0]==0x1f6c3c0||wait_identity[0]==0x1f6c490){if(std::uncaught_exceptions()==exceptions)ps3_critical_emit_capture(capture,106,(uint64_t)ctx,token,wait_identity,8);}else ps3_critical_emit_capture(capture,38,(uint64_t)ctx,token,wait_identity,8);}
  if(function==0xDFBCAC){
   const uint32_t job=(uint32_t)args[function==0xDFBCAC?1:0],module=function==0xDFBCAC?(uint32_t)args[0]:vm_read32(job+0x288);
   if((module==0x1f6c3c0||module==0x1f6c490)&&std::uncaught_exceptions()==exceptions){const uint32_t container=vm_read32(job+0x2a4),ticket=vm_read16(job+0x2a0);uint64_t identity[8]={module,job,vm_read64(job+0x280),container,vm_read32(container),ticket,vm_read32(job+0x28c),ctx->thread_id};ps3_critical_emit_capture(capture,104,(uint64_t)ctx,token,identity,8);}
   if(module==0x1f6b930||module==0x1f6ba00){const uint32_t container=vm_read32(job+0x2a4),ticket=vm_read16(job+0x2a0);uint64_t identity[8]={module,job,vm_read64(job+0x280),container,vm_read32(container),ticket,vm_read32(container+0x2c),ticket?vm_read32(container+0x20)+8ull*(ticket-1):0};ps3_critical_emit_capture(capture,function==0xDFBCAC?37:38,(uint64_t)ctx,token,identity,8);}
  }
  uint64_t w[8]={start,function,args[0],args[1],args[2],ctx->gpr[3],parent,(uint64_t)(std::uncaught_exceptions()==exceptions)};ps3_critical_emit_capture(capture,PS3_CRITICAL_PPU,(uint64_t)ctx,token,w,8);}
 errno=saved;
}
struct ControlStart {ControlStart(){ps3_critical_control_start();}};static ControlStart trace_control;

#include "mag_graphics_payload_trace.h"
#include "mag_payload_clock_observation.h"
#include <pthread.h>
#include <time.h>
#include "diagnostics/critical_pc_trace.h"
#include "diagnostics/job_io_recorder.h"
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "spu_context.h"
static uint32_t payload_ls_word(const spu_context *c,uint32_t a){const uint8_t *p=c->ls+(a&SPU_LS_MASK);return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
/* Guest continuations can survive abandoned C frames and host-thread migration.
 * A fixed context registry retains the invocation until an observed arrival.
 * Replacement is explicitly unmatched, never inferred to be a completion. */
static std::mutex payload_lock;
struct PendingPayload {MagGraphicsPayloadTrace token;uint64_t continuation_hash,capture_seen;uint32_t queue;MagPayloadClockObservation clock{};bool selected{};};
static PendingPayload payload_pending[64];
/* A cached opt-in observation selector; no guest input, scheduling or completion changes. */
static const bool payload_clock_selected=[](){const char*e=std::getenv("MAG_GRAPHICS_PAYLOAD_CLOCK");return e&&!std::strcmp(e,"1");}();
static const uint32_t payload_clock_entry=mag_payload_clock_entry(std::getenv("MAG_GRAPHICS_PAYLOAD_CLOCK_ENTRY"));
static MagPayloadClockObservation payload_clock(){
 MagPayloadClockObservation out{};out.wall=ps3_critical_now_ns();struct timespec value{};
 out.cpu_valid=clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value)==0;
 if(out.cpu_valid)out.cpu=uint64_t(value.tv_sec)*1000000000ull+uint64_t(value.tv_nsec);
 out.host_valid=pthread_threadid_np(nullptr,&out.host)==0;return out;
}
static const char *snapshot_prefix=std::getenv("MAG_GRAPHICS_PAYLOAD_SNAPSHOT");
static std::atomic<bool> snapshot_claimed{false};
static bool snapshot_write(const char *path,const void *data,size_t size){FILE *f=std::fopen(path,"wb");if(!f)return false;bool ok=std::fwrite(data,1,size,f)==size;return std::fclose(f)==0&&ok;}
/* A separate post-measurement diagnostic. The controller arms one capture,
 * waits for its finalized receipt and this private snapshot, then stops. */
static void payload_snapshot(spu_context *c){
 if(!snapshot_prefix||!*snapshot_prefix||snapshot_claimed.load(std::memory_order_relaxed))return;
 int saved=errno;const uint64_t finalized=ps3_critical_finalized_capture_id();
 if(!finalized||ps3_critical_enabled()||std::strlen(snapshot_prefix)>1800){errno=saved;return;}
 const uint32_t target=c->pc&SPU_LS_MASK,queue=payload_ls_word(c,0x13F0),marker=payload_ls_word(c,0x13A8);
 if(target!=0x17A30||marker!=211){errno=saved;return;}
 const uint32_t generation=ps3_critical_context_generation(c);bool bound=false;
 {std::lock_guard<std::mutex> guard(payload_lock);for(const auto &p:payload_pending)if(p.token.owner==(uint64_t)c&&p.capture_seen==finalized&&p.token.generation==generation&&p.queue==queue){bound=true;break;}}
 if(!bound||ps3_critical_enabled()||ps3_critical_capture_id()!=finalized||snapshot_claimed.exchange(true)){errno=saved;return;}
 const uint64_t started=ps3_critical_now_ns();char ls_path[2048],gpr_path[2048],receipt[2048],temporary[2048];
 std::snprintf(ls_path,sizeof ls_path,"%s.ls.bin",snapshot_prefix);std::snprintf(gpr_path,sizeof gpr_path,"%s.gpr.bin",snapshot_prefix);std::snprintf(receipt,sizeof receipt,"%s.json",snapshot_prefix);std::snprintf(temporary,sizeof temporary,"%s.json.tmp",snapshot_prefix);
 const bool ls_ok=snapshot_write(ls_path,c->ls,SPU_LS_SIZE),gpr_ok=snapshot_write(gpr_path,c->gpr,sizeof c->gpr);
 FILE *f=std::fopen(temporary,"wb");if(f){
  const bool ok=std::fprintf(f,"{\"schema\":1,\"status\":\"%s\",\"capture_id\":%llu,\"owner\":%llu,\"generation\":%u,\"queue\":%u,\"entry\":%u,\"marker\":%u,\"start_ns\":%llu,\"end_ns\":%llu,\"ls_bytes\":%u,\"gpr_bytes\":%zu,\"ls_written\":%s,\"gpr_written\":%s,\"diagnostics_inactive\":%s}\n",ls_ok&&gpr_ok?"complete":"io_error",(unsigned long long)finalized,(unsigned long long)(uintptr_t)c,generation,queue,target,marker,(unsigned long long)started,(unsigned long long)ps3_critical_now_ns(),SPU_LS_SIZE,sizeof c->gpr,ls_ok?"true":"false",gpr_ok?"true":"false",!ps3_critical_enabled()?"true":"false")>0;
  const int closed=std::fclose(f);if(ok&&!closed)std::rename(temporary,receipt);
 }
 errno=saved;
}

/* Default-off black-box recorder for the complete observable interface of
 * selected payload runs (see docs/native-graphics-reimplementation-method.md).
 * MAG_JOB_IO_DIR enables it; MAG_JOB_IO_ENTRY (hex, default 12D28) selects the
 * payload entry; MAG_JOB_IO_SKIP runs are ignored first so recording starts in
 * a steady scene; MAG_JOB_IO_COUNT bounds the number of runs. Optional
 * MAG_JOB_IO_ARM requires an existing controller arm file; unarmed entries do
 * not consume SKIP/COUNT. This observes real job transfers, not PPE snapshots. */
static const char *jobio_directory=std::getenv("MAG_JOB_IO_DIR");
static const char *jobio_arm_path=std::getenv("MAG_JOB_IO_ARM");
static uint32_t jobio_env(const char *name,uint32_t fallback,int base){const char *e=std::getenv(name);return e&&*e?(uint32_t)std::strtoul(e,nullptr,base):fallback;}
static const uint32_t jobio_entry=jobio_env("MAG_JOB_IO_ENTRY",0x12D28,16),jobio_skip=jobio_env("MAG_JOB_IO_SKIP",30,10),jobio_limit=jobio_env("MAG_JOB_IO_COUNT",8,10);
static std::atomic<uint32_t> jobio_seen{0},jobio_taken{0};
static void jobio_arm(spu_context *c){
 if(mag_jobio_selector_configured()){
  if(jobio_directory&&*jobio_directory&&jobio_taken.load()<jobio_limit&&mag_jobio_selector_begin(c,c->pc&SPU_LS_MASK,jobio_directory))jobio_taken.fetch_add(1);
  return;
 }
 if(!jobio_directory||!*jobio_directory||(c->pc&SPU_LS_MASK)!=jobio_entry||ps3_jobio_is(c)||jobio_taken.load()>=jobio_limit)return;
 if(jobio_arm_path&&*jobio_arm_path&&access(jobio_arm_path,F_OK)!=0)return;
 if(jobio_seen.fetch_add(1)<jobio_skip)return;
 int saved=errno;if(!ps3_jobio_begin(c,jobio_entry,jobio_directory))jobio_taken.fetch_add(1);errno=saved;
}
static uint64_t payload_hash(const spu_context *c,uint32_t pc){uint64_t h=14695981039346656037ULL;for(unsigned i=0;i<16;i++){h^=c->ls[(pc+i)&(SPU_LS_SIZE-1)];h*=1099511628211ULL;}return h;}
static void payload_arrival(void *opaque,uint32_t pc,uint32_t path){
 if(ps3_jobio_is(opaque)&&pc==0x3260){int saved=errno;if(!mag_jobio_selector_end(opaque))ps3_jobio_end((spu_context*)opaque);errno=saved;}
 if(!ps3_critical_enabled())return;int saved=errno;auto *c=(spu_context*)opaque;
 const auto capture=ps3_critical_capture_id();const auto generation=ps3_critical_context_generation(c);
 std::lock_guard<std::mutex> guard(payload_lock);
 for(auto &p:payload_pending){auto &t=p.token;if(t.owner!=(uint64_t)c||!t.capture)continue;
  if(t.capture!=capture||t.generation!=generation){t.capture=0;continue;}
  if(pc!=t.continuation||payload_ls_word(c,0x13F0)!=p.queue||payload_hash(c,pc)!=p.continuation_hash)continue;
  uint64_t end[8]={t.start,t.target,t.continuation,t.generation,t.control_12e0,pc,(uint64_t)c->image_id,((uint64_t)path<<32)|(uint32_t)c->resident_ovl};
  ps3_critical_emit_capture(t.capture,14,t.owner,t.span,end,8);
  if(p.selected){const auto stamp=payload_clock();uint64_t cpu{};const bool valid=mag_payload_clock_delta(p.clock,stamp,&cpu);uint64_t clock_end[8]={stamp.wall,stamp.cpu,stamp.host,t.target,t.generation,p.queue,cpu,uint64_t(stamp.cpu_valid)|(uint64_t(stamp.host_valid)<<1)|(uint64_t(valid)<<2)};ps3_critical_emit_capture(t.capture,118,t.owner,t.span,clock_end,8);}
  t.capture=0;break;
 }
 errno=saved;
}
struct PayloadArrivalStart {PayloadArrivalStart(){ps3_critical_arrival_callback=payload_arrival;__atomic_store_n(&ps3_critical_arrival_pc,0x3260,__ATOMIC_RELEASE);}};static PayloadArrivalStart payload_arrival_start;
extern "C" void mag_graphics_payload_begin(MagGraphicsPayloadTrace *t,void *opaque){
 t->capture=0;jobio_arm((spu_context*)opaque);if(!ps3_critical_enabled()){if(snapshot_prefix)payload_snapshot((spu_context*)opaque);return;}int saved=errno;auto *c=(spu_context*)opaque;
 uint64_t range=__atomic_load_n(&ps3_critical_watch_range,__ATOMIC_ACQUIRE);
 uint32_t queue=payload_ls_word(c,0x13F0);
 const bool selected=payload_clock_selected&&payload_clock_entry&&((c->pc&SPU_LS_MASK)==payload_clock_entry);
 const auto *ext=mag_extension();const bool extension_selected=ext->payload_select&&ext->payload_select(queue,ps3_critical_capture_id());
 if(!selected&&!extension_selected&&(!range || queue!=((uint32_t)range&~127u))){errno=saved;return;}
 t->capture=ps3_critical_capture_id();t->start=ps3_critical_now_ns();t->span=++sequence;t->owner=(uint64_t)c;t->target=c->pc&SPU_LS_MASK;t->continuation=c->gpr[0]._u32[0];t->generation=ps3_critical_context_generation(c);t->control_12e0=payload_ls_word(c,0x12E0);
 {std::lock_guard<std::mutex> guard(payload_lock);PendingPayload *slot=nullptr;
  for(auto &p:payload_pending)if(p.token.owner==t->owner){slot=&p;break;}
  if(!slot)for(auto &p:payload_pending)if(!p.token.capture||p.token.capture!=t->capture){slot=&p;break;}
  if(slot){if(slot->token.capture==t->capture){uint64_t unmatched[3]={slot->token.start,slot->token.target,t->span};ps3_critical_emit_capture(t->capture,17,t->owner,slot->token.span,unmatched,3);}*slot={*t,payload_hash(c,t->continuation),t->capture,queue,{},selected};
   if(selected){slot->clock=payload_clock();const auto &clock=slot->clock;uint64_t clock_begin[8]={clock.wall,clock.cpu,clock.host,t->target,t->generation,queue,0,uint64_t(clock.cpu_valid)|(uint64_t(clock.host_valid)<<1)};ps3_critical_emit_capture(t->capture,117,t->owner,t->span,clock_begin,8);}}
  else{uint64_t missing[1]={1};ps3_critical_emit_capture(t->capture,17,t->owner,t->span,missing,1);}
 }
 if(ext->payload_begin)ext->payload_begin(c,t->span,t->target);
 uint64_t fingerprint=payload_hash(c,t->target);
 uint64_t begin[8]={t->start,t->target,t->continuation,queue,t->generation,t->control_12e0,payload_ls_word(c,0x1320),fingerprint};ps3_critical_emit_capture(t->capture,13,t->owner,t->span,begin,8);
 uint64_t descriptor[8];for(unsigned i=0;i<4;i++){descriptor[i]=payload_ls_word(c,0x1320+i*4);descriptor[4+i]=payload_ls_word(c,0x13A0+i*4);}ps3_critical_emit_capture(t->capture,15,t->owner,t->span,descriptor,8);
 for(unsigned i=0;i<4;i++)if(c->resident_code[i].size){uint64_t mapping[5]={i,c->resident_code[i].lsa,c->resident_code[i].size,c->resident_code[i].source_ea,(uint64_t)c->resident_code[i].image_id};ps3_critical_emit_capture(t->capture,16,t->owner,t->span,mapping,5);}
 errno=saved;
}
extern "C" void mag_graphics_payload_end(const MagGraphicsPayloadTrace *t,void *opaque){
 if(t->capture)payload_arrival(opaque,((spu_context*)opaque)->pc&SPU_LS_MASK,1);
}

extern "C" void mag_graphics_renderer_enter(void *opaque){
 if(!ps3_critical_enabled())return;int saved=errno;auto *c=(spu_context*)opaque;const auto capture=ps3_critical_capture_id();
 std::lock_guard<std::mutex> guard(payload_lock);
 for(const auto &p:payload_pending){const auto &t=p.token;if(t.owner==(uint64_t)c&&t.capture==capture&&t.target==0x17A30){uint64_t w[5]={107,0x17A30,t.generation,p.queue,payload_ls_word(c,0x13A8)};ps3_critical_emit_capture(capture,18,t.owner,t.span,w,5);break;}}
 errno=saved;
}

/* Observation-only second capture, separate from the efficacy timing window.
 * Record the actual stored effective value; do not reread guest memory. */
static const bool occlusion_param_observe=[](){const char *e=std::getenv("MAG_OCCLUSION_PARAM_OBSERVE");return e&&!std::strcmp(e,"1");}();
extern "C" void mag_graphics_occlusion_store(ppu_context *ctx,uint32_t pc,uint32_t params,uint32_t renderer,uint32_t value,uint32_t partition){
 if(!occlusion_param_observe||!ps3_critical_enabled()||ps3_critical_capture_id()!=2)return;
 int saved=errno;uint64_t w[8]={pc,params,renderer,value,partition,ctx->lr,ctx->gpr[30],ctx->gpr[22]};
 ps3_critical_emit_capture(2,21,(uint64_t)ctx,0,w,8);errno=saved;
}
