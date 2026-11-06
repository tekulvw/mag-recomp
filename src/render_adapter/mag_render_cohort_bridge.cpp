/* Retained cohort observers (see the header). Original rendering always
 * continues: these scopes read guest memory, repeat the original's own workload
 * notification while waiting for retained jobs, and copy sources inside the
 * synchronous adapter calls. They never write guest memory. Every entry returns
 * at once while the adapter's master switch is off. */
#include "mag_render_cohort_bridge.h"
#include "mag_render_frontend_bridge.h"
#include "mag_render_adapter_switch.h"
#include "mag_render_texture_bridge.h"
#include "mag_render_update_loop.h"
#include "ppu_recomp.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <cstdio>
#include <cstring>
#include <limits>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>
extern "C" { extern uint32_t ppu_vm_size; extern uint8_t* vm_base;
uint16_t vm_read16(uint64_t);uint64_t vm_read64(uint64_t); }
struct CellSpurs;
extern "C" int32_t cellSpursReadyCountStore(CellSpurs*,uint32_t,uint32_t);
namespace {
static_assert(sizeof(MagRenderDepthSourceInput)==80,"Depth source ABI");
static_assert(sizeof(MagRenderCohortResourceInput)==40&&sizeof(MagRenderCohortResourceResult)==32,"resource ABI");
static_assert(sizeof(MagRenderCohortResourceInputV3)==56,"located resource ABI");
static_assert(sizeof(MagRenderCohortResourceInputV4)==64,"Main setup source ABI");
static_assert(sizeof(MagRenderCohortResourceInputV5)==80,"pre-generator source ABI");
static_assert(sizeof(MagRenderProducerSourceInput)==72,"producer observation ABI");
static_assert(sizeof(MagRenderCohortBegin)==312&&sizeof(MagRenderCohortItem)==176,"cohort ABI");
std::mutex mutex;
std::shared_mutex source_gate;
MagRenderCohortStats stats{};
uint64_t serial{};
uint64_t next(){if(serial==UINT64_MAX){stats.lost=true;return 0;}return ++serial;}
void count(uint64_t&v){if(v==UINT64_MAX)stats.lost=true;else ++v;}
bool range(uint64_t p,uint64_t n){const uint64_t end=ppu_vm_size?ppu_vm_size:(1ull<<32);return vm_base&&p>=0x10000&&p<end&&n<=end-p;}
uint64_t queue_key(uint32_t container,uint32_t array){return uint64_t(container)<<32|array;}
struct Allocation {uint64_t id{},vm{};uint32_t module{};};
std::unordered_map<uint32_t,Allocation> allocations;
std::unordered_map<uint64_t,uint64_t> resets;
struct Ticket {uint32_t container{},base{},value{},slot{},marker{},bucket{};uint64_t reset{};std::array<uint32_t,4>lists{};bool sorted{},completed{};uint32_t job{};uint64_t allocation{};};
struct Phase {uint64_t vm{},id{},previous{},owner{},producer_window{};uint32_t producer_bank{},producer_toc{};bool producer_seen{},producer_bad{};uint32_t renderer{},bucket{};ppu_context*context{};std::thread::id host;bool valid{},open{},sealed{};uint32_t joins{};std::array<unsigned,4>sorts{};std::vector<Ticket>tickets;std::array<unsigned,4>depth_sorts{};std::vector<Ticket>depth_tickets;bool depth_copy_active{};uint32_t depth_captured{};bool helper_reads{};};
std::shared_ptr<Phase> active_regular,active_main;
thread_local std::shared_ptr<Phase> regular,main_phase;
struct Minimap {uint64_t vm{},id{};uint32_t renderer{};ppu_context*context{};uint64_t owner{};std::thread::id host;Ticket ticket{};bool valid{};};
thread_local Minimap minimap;
struct Origin {ppu_context*context{};uint64_t vm{},owner{},sp{},member{};uint32_t renderer{},marker{};bool valid{};};
thread_local Origin root,minimap_origin,depth_root;
struct Sort {ppu_context*context{};uint32_t bucket{},module{};bool valid{};uint32_t family{};};
thread_local Sort sort_source;
thread_local std::unordered_map<uint32_t,uint64_t> returning;
bool ticket_current(const Ticket&t){auto r=resets.find(queue_key(t.container,3));return r!=resets.end()&&r->second==t.reset&&range(t.container,4)&&vm_read32(t.container)==t.base&&range(uint64_t(t.base)+0x70,16);}
bool ready(Ticket&t){
 if(!ticket_current(t))return false;
 uint16_t minimum=vm_read16(uint64_t(t.base)+0x72);
 for(unsigned lane=2;lane<8;++lane)minimum=std::min(minimum,vm_read16(uint64_t(t.base)+0x70+2*lane));
 std::atomic_thread_fence(std::memory_order_acquire);
 t.completed=minimum>=t.value;return t.completed;
}
bool make_ticket(uint32_t container,uint32_t value,Ticket&t){
 auto r=resets.find(queue_key(container,3));
 if(!value||r==resets.end()||!range(container,0x30)||vm_read32(container+0x28)!=container||vm_read32(container+0x2c)!=3)return false;
 t.container=container;t.base=vm_read32(container);t.value=value;t.reset=r->second;return ticket_current(t);
}
bool owner(const std::shared_ptr<Phase>&p,ppu_context*c){return p&&p->valid&&p->vm==stats.vm&&p->context==c&&p->owner==c->thread_id&&p->host==std::this_thread::get_id();}
void fail(){stats.lost=true;if(active_main)active_main->valid=false;if(active_regular)active_regular->valid=false;}
void invalidate_pool(uint32_t job){
 for(auto*p:{active_regular.get(),active_main.get()})if(p)for(auto*list:{&p->tickets,&p->depth_tickets})for(auto&t:*list)
  if(t.sorted&&t.job==job&&!t.completed&&!ready(t))p->valid=false;
}
struct CopyOwner {ppu_context*context;uint64_t vm,owner;std::thread::id host;std::shared_ptr<Phase>phase;};
// Default-off PS3_RENDER_CAPTURE_HELPER_READS=1: while the source thread is
// parked inside the synchronous Main asset copy call, a helper thread of that
// call may copy too. Every other check is the source thread's; the window is
// closed before the call returns, and the adapter joins its helpers first.
bool helper_reads_enabled()noexcept{static const bool enabled=[](){const char*v=std::getenv("PS3_RENDER_CAPTURE_HELPER_READS");return v&&v[0]=='1'&&!v[1];}();return enabled;}
int32_t copy_asset(void*opaque,uint32_t address,uint32_t bytes,uint8_t*out) noexcept {
 try {
  const auto&o=*static_cast<CopyOwner*>(opaque);
  const bool here=o.host==std::this_thread::get_id();
  {std::lock_guard lock(mutex);
   const auto&p=o.phase;
   const bool phase_owned=here?owner(p,o.context)
     :p&&p->valid&&p->vm==stats.vm&&p->context==o.context&&p->owner==o.context->thread_id&&p->host==o.host&&p->helper_reads;
   if(!out||!range(address,bytes)||stats.vm!=o.vm||stats.lost||!phase_owned
     ||o.phase!=active_main||!o.phase->sealed||o.context->thread_id!=o.owner)return -1;}
  // The caller still holds the actual phase reuse pin. This copies one raw
  // source range, and does not invent asset immutability or cache generations.
  std::memcpy(out,vm_base+address,bytes);return 0;
 }catch(...){return -1;}
}
// Default-off PS3_RENDER_CAPTURE_VIEW_VALIDATION=1: a read-only view with
// exactly copy_asset's checks, so the adapter can compare guest bytes in place
// instead of copying them first. Same window, same moment, same bytes.
bool view_validation_enabled()noexcept{static const bool enabled=[](){const char*v=std::getenv("PS3_RENDER_CAPTURE_VIEW_VALIDATION");return v&&v[0]=='1'&&!v[1];}();return enabled;}
const uint8_t* view_asset(void*opaque,uint32_t address,uint32_t bytes) noexcept {
 try {
  const auto&o=*static_cast<CopyOwner*>(opaque);
  const bool here=o.host==std::this_thread::get_id();
  {std::lock_guard lock(mutex);
   const auto&p=o.phase;
   const bool phase_owned=here?owner(p,o.context)
     :p&&p->valid&&p->vm==stats.vm&&p->context==o.context&&p->owner==o.context->thread_id&&p->host==o.host&&p->helper_reads;
   if(!bytes||!range(address,bytes)||stats.vm!=o.vm||stats.lost||!phase_owned
     ||o.phase!=active_main||!o.phase->sealed||o.context->thread_id!=o.owner)return nullptr;}
  return vm_base+address;
 }catch(...){return nullptr;}
}
// Depth sources are a further opt-in (PS3_RENDER_DEPTH_SOURCES=1) under the master switch.
bool depth_sources_enabled(){static const bool enabled=[](){const char*v=std::getenv("PS3_RENDER_DEPTH_SOURCES");return v&&v[0]=='1'&&!v[1];}();return enabled&&mag_render_hooks_enabled();}
bool main_sources_enabled()noexcept{return mag_render_hooks_enabled();}
int32_t copy_depth_asset(void*opaque,uint32_t address,uint32_t bytes,uint8_t*out) noexcept {
 try {const auto&o=*static_cast<CopyOwner*>(opaque);
  {std::lock_guard lock(mutex);
   if(!out||!range(address,bytes)||stats.vm!=o.vm||stats.lost||!owner(o.phase,o.context)
      ||o.phase!=active_regular||!o.phase->open||!o.phase->depth_copy_active
      ||!depth_root.valid||depth_root.context!=o.context||depth_root.owner!=o.owner
      ||o.host!=std::this_thread::get_id())return -1;}
  std::memcpy(out,vm_base+address,bytes);return 0;
 }catch(...){return -1;}
}
// A distinct pre-generator copy window. No guest callback or pointer survives
// this call; the immutable Rust record is joined to its exact Main occurrence.
int32_t copy_producer_asset(void*opaque,uint32_t address,uint32_t bytes,uint8_t*out) noexcept {
 try {
  const auto&o=*static_cast<CopyOwner*>(opaque);
  {std::lock_guard lock(mutex);
   if(!out||!range(address,bytes)||stats.vm!=o.vm||stats.lost||!owner(o.phase,o.context)
      ||o.phase!=active_regular||!o.phase->open||o.context->thread_id!=o.owner||o.host!=std::this_thread::get_id())return -1;}
  std::memcpy(out,vm_base+address,bytes);return 0;
 }catch(...){return -1;}
}
void capture_producer(ppu_context*c){
 if(!main_sources_enabled())return;
 std::shared_lock pin(source_gate);std::shared_ptr<Phase>phase;MagRenderProducerSourceInput input{};
 {
  std::lock_guard lock(mutex);phase=regular;
  if(!owner(phase,c)||phase!=active_regular||!phase->open||stats.lost||c->lr!=0xd0d1c0
     ||c->gpr[3]!=uint64_t(phase->renderer)+0x31d0||c->gpr[2]>UINT32_MAX||c->gpr[2]<0x243c
     ||!range(c->gpr[3],0xbc))return;
  if(phase->producer_seen){phase->producer_bad=true;phase->producer_window=0;return;}
  phase->producer_seen=true;
  const auto bank=vm_read32(c->gpr[3]+0xb8);const auto update=mag_render_update_current(c);
  if(bank>1||!update){phase->producer_bad=true;return;}
  input.abi_version=1;input.struct_bytes=sizeof input;input.vm=stats.vm;input.source_window=next();input.ppu_owner=c->thread_id;input.execution_update=update;
  input.source_toc_address=uint32_t(c->gpr[2]);input.renderer_address=phase->renderer;input.bank_arena=uint32_t(c->gpr[3]);input.bank_index=bank;
  input.group_flags=uint32_t(c->gpr[6]);input.group_limit=int32_t(uint32_t(c->gpr[7]));
  const float minimum=float(c->fpr[1]),maximum=float(c->fpr[2]);std::memcpy(&input.clip_min,&minimum,4);std::memcpy(&input.clip_max,&maximum,4);
 }
 CopyOwner copy_owner{c,input.vm,c->thread_id,std::this_thread::get_id(),phase};
 const auto status=mag_render_adapter_producer_sources(input,copy_producer_asset,&copy_owner);
 std::lock_guard lock(mutex);
 if(status||!owner(phase,c)||phase!=active_regular||!phase->open){phase->producer_bad=true;phase->producer_window=0;return;}
 phase->producer_window=input.source_window;phase->producer_bank=input.bank_index;phase->producer_toc=input.source_toc_address;
}
void decline_locked(){(void)mag_render_adapter_cohort_decline(stats.vm);count(stats.declined);}
void capture(ppu_context*c){
 if(!main_sources_enabled())return;
 std::shared_lock pin(source_gate);
 std::shared_ptr<Phase> phase;
 uint64_t vm{},invocation{};
 std::vector<std::array<uint32_t,2>> wakes;
 uint32_t marker{},pass{},bucket{},source_user{},captured_job{};uint64_t captured_allocation{};
 {
  std::lock_guard lock(mutex);count(stats.arrivals);phase=main_phase;
  if(!owner(phase,c)||phase!=active_main||!phase->open||phase->sealed||phase->joins!=3||!root.valid
     ||root.vm!=stats.vm||root.context!=c||root.owner!=c->thread_id||c->gpr[1]!=root.sp-0x4d0
     ||c->gpr[14]!=0||c->gpr[25]!=root.marker||c->gpr[17]!=phase->bucket||stats.lost){decline_locked();return;}
  for(auto n:phase->sorts)if(n!=1){decline_locked();return;}
  const auto job=c->gpr[4];auto a=job<=UINT32_MAX?allocations.find(uint32_t(job)):allocations.end();
  if(a==allocations.end()||a->second.vm!=stats.vm||a->second.module!=0x1f6b930||!range(job,0x2a8)||vm_read32(job+0x288)!=0x1f6b930){decline_locked();return;}
  const auto header=vm_read64(job+0x280);const uint64_t command=uint32_t(header),bytes=(header>>48)&0xfff,user=vm_read32(job+0x28c);
  if(((header>>32)&0xffff)!=1||bytes!=240||user!=command+80||!range(command,bytes)||!range(user,160)
     ||c->gpr[19]>UINT32_MAX||!range(c->gpr[19],688)||!range(phase->bucket,0x68)||uint64_t(root.marker)+0x2bb0>UINT32_MAX){decline_locked();return;}
  marker=root.marker+0x2bb0;pass=uint32_t(c->gpr[19]);bucket=phase->bucket;source_user=uint32_t(user);
  captured_job=uint32_t(job);captured_allocation=a->second.id;
  if(!range(marker,320)){decline_locked();return;}
  for(auto&t:phase->tickets){
   if(!ticket_current(t)){decline_locked();return;}
   if(t.sorted){if(t.marker!=marker+80*t.slot){decline_locked();return;}
    for(unsigned i=0;i<4;++i)if(t.lists[i]!=vm_read32(uint64_t(bucket)+0x28+i*4)){decline_locked();return;}}
   std::array<uint32_t,2>op{vm_read32(t.container+4),vm_read32(t.container+8)};
   if((op[0]&127)||!range(op[0],128)||op[1]>=32){decline_locked();return;}
   if(std::find(wakes.begin(),wakes.end(),op)==wakes.end())wakes.push_back(op);
  }
  vm=stats.vm;invocation=next();phase->sealed=true;
 }
 auto decline=[](){std::lock_guard lock(mutex);decline_locked();};
 // Repeat the original real workload notification, never a fake completion.
 for(const auto&op:wakes)if(cellSpursReadyCountStore(reinterpret_cast<CellSpurs*>(uintptr_t(op[0])),op[1],5)){decline();return;}
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 for(;;){bool complete=true;{
  std::lock_guard lock(mutex);
  if(!owner(phase,c)||phase!=active_main||stats.vm!=vm||stats.lost){decline_locked();return;}
  for(auto&t:phase->tickets){if(!ticket_current(t)){decline_locked();return;}if(!ready(t))complete=false;}
 }if(complete)break;if(std::chrono::steady_clock::now()>=deadline){decline();return;}std::this_thread::yield();}
 // All fields below are source-owned retained visibility outputs, not child
 // geometry/material descriptors or an invented world/camera epoch.
 const uint32_t mbi=vm_read32(uint64_t(pass)+0xe4),transforms=vm_read32(uint64_t(pass)+0xe0);
 const uint32_t camera=vm_read16(uint64_t(pass)+0x19a);
 if(camera>3||mbi!=vm_read32(uint64_t(bucket)+4)||transforms!=vm_read32(uint64_t(bucket)+12)){decline();return;}
 MagRenderCohortBegin begin{};begin.abi_version=1;begin.struct_bytes=sizeof begin;begin.vm=vm;begin.invocation=invocation;begin.owner=c->thread_id;begin.pass_occurrence=phase->id;begin.execution_update=mag_render_update_current(c);begin.camera=camera;
 std::array<uint32_t,4>populations{},lists{};
 for(unsigned shard=0;shard<4;++shard){uint64_t total=0;
  for(unsigned group=0;group<16;++group){const auto n=vm_read32(uint64_t(marker)+80*shard+4*group);begin.group_counts[shard][group]=n;total+=n;}
  if(total>65536||total!=vm_read32(uint64_t(marker)+80*shard+64)){decline();return;}
  populations[shard]=uint32_t(total);lists[shard]=vm_read32(uint64_t(bucket)+0x28+shard*4);
  if(total&&!range(lists[shard],total*2)){decline();return;}
 }
 if(mag_render_adapter_cohort_begin(begin)){decline();return;}
 bool valid=true;uint64_t item_count=0;
 for(unsigned shard=0;shard<4&&valid;++shard)for(uint32_t i=0;i<populations[shard]&&valid;++i){
  MagRenderCohortItem item{};item.vm=vm;item.invocation=invocation;item.shard=shard;item.source_index=vm_read16(uint64_t(lists[shard])+2*i);
  const uint64_t source=uint64_t(mbi)+32*item.source_index;
  if(!range(source,32)){valid=false;break;}
  for(unsigned word=0;word<8;++word)item.mbi[word]=vm_read32(source+4*word);
  const uint32_t bit=1u<<(3-camera);
  if(item.mbi[5]&bit){
   const uint32_t prior=item.mbi[4]&15&(bit-1);unsigned count_prior=0;for(uint32_t n=prior;n;n>>=1)count_prior+=n&1;
   const uint32_t slot=(vm_read16(source+28)&0x7fff)+count_prior;
   const uint64_t transform=uint64_t(transforms)+112*slot;
   if(!range(transform,112)){valid=false;break;}item.has_transform=1;
   for(unsigned word=0;word<28;++word)item.transform[word]=vm_read32(transform+4*word);
  }
  if(mag_render_adapter_cohort_item(item)){valid=false;break;}++item_count;
 }
 // Finishing an incomplete builder revokes it; no stale successful publication.
 const auto result=mag_render_adapter_cohort_finish(vm,invocation);
 {std::lock_guard lock(mutex);
  if(!valid||result){decline_locked();return;}
  count(stats.published);if(item_count>UINT64_MAX-stats.items)stats.lost=true;else stats.items+=item_count;
 }
 MagRenderCohortResourceInput resources{};resources.abi_version=4;resources.struct_bytes=sizeof(MagRenderCohortResourceInputV4);
 resources.vm=vm;resources.invocation=invocation;resources.source_pass_address=pass;resources.render_mode=vm_read32(uint64_t(pass)+0xdc);
 resources.technique_override_mask=vm_read32(uint64_t(pass)+0x194);resources.geometry_resource_base=vm_read32(uint64_t(pass)+0x284);
 CopyOwner copy_owner{c,vm,c->thread_id,std::this_thread::get_id(),phase};
 MagRenderTextureCopyContext located{copy_asset,&copy_owner,nullptr};
 if(view_validation_enabled()){
  static const bool registered=mag_render_adapter_set_located_view(mag_render_texture_view_located)==0;
  static const bool guest=mag_render_adapter_set_guest_view(view_asset)==0;(void)guest;
  if(registered)located.view=view_asset;
 }
 const MagRenderCohortResourceInputV4 input{{resources,mag_render_texture_copy_located,&located},source_user,0};
 MagRenderCohortResourceResult copied{};
 if(phase->producer_seen&&!phase->producer_bad&&phase->producer_window){
  // Join only the same already-qualified Regular -> Main ownership chain.
  const uint64_t arena=uint64_t(phase->renderer)+0x31d0;
  if(c->gpr[2]!=phase->producer_toc||!range(arena,0xbc)||vm_read32(arena+0xb8)!=phase->producer_bank){decline();return;}
  auto v4=input;v4.base.base.abi_version=5;v4.base.base.struct_bytes=sizeof(MagRenderCohortResourceInputV5);
  const MagRenderCohortResourceInputV5 v5{v4,phase->producer_toc,0,phase->producer_window};
  struct Window{const std::shared_ptr<Phase>&p;bool open;
   Window(const std::shared_ptr<Phase>&q):p(q),open(helper_reads_enabled()){if(open){std::lock_guard lock(mutex);p->helper_reads=true;}}
   ~Window(){if(open){std::lock_guard lock(mutex);p->helper_reads=false;}}} window{phase};
  copied=mag_render_adapter_cohort_assets_v5(&v5,copy_asset,&copy_owner);
 }else{
  // Legacy observation remains explicitly without producer inputs. Whole-scene
  // admission requires the V5 exact pre-source record; this cannot be a draw.
  copied=mag_render_adapter_cohort_assets_v4(&input,copy_asset,&copy_owner);
 }
 // The Main client and UI handoff consume these identities once they are published.
 (void)captured_job;(void)captured_allocation;
 std::lock_guard lock(mutex);
 if(copied.status){if(copied.status!=2)(void)mag_render_adapter_cohort_decline(stats.vm);count(stats.asset_declined);return;}
 count(stats.asset_copies);if(copied.bytes>UINT64_MAX-stats.asset_bytes)stats.lost=true;else stats.asset_bytes+=copied.bytes;
}
void capture_depth(ppu_context*c){
 if(!depth_sources_enabled())return;
 std::shared_lock pin(source_gate);std::shared_ptr<Phase>phase;MagRenderDepthSourceInput input{};std::vector<Ticket>dependencies;
 uint64_t observed_vm{};{std::lock_guard lock(mutex);observed_vm=stats.vm;count(stats.depth_arrivals);}
 struct Attempt {uint64_t vm;bool success{};~Attempt(){std::lock_guard lock(mutex);if(stats.vm==vm&&!success)count(stats.depth_declined);}} attempt{observed_vm};
 {
  std::lock_guard lock(mutex);phase=regular;
  if(!owner(phase,c)||phase!=active_regular||!phase->open||phase->depth_copy_active||stats.lost
     ||!depth_root.valid||depth_root.vm!=stats.vm||depth_root.context!=c||depth_root.owner!=c->thread_id
     ||c->gpr[1]!=depth_root.sp-0x4d0||c->gpr[14]>3||c->gpr[25]!=depth_root.marker||c->gpr[17]>UINT32_MAX
     ||phase->tickets.size()!=1)return;
  const uint32_t camera=uint32_t(c->gpr[14]);if(phase->depth_captured&(1u<<camera))return;
  for(auto n:phase->depth_sorts)if(n!=1)return;
  if(!minimap.valid||minimap.vm!=stats.vm||minimap.host!=std::this_thread::get_id()||minimap.context!=c
     ||minimap.owner!=c->thread_id||minimap.renderer!=phase->renderer||minimap.id<=phase->previous)return;
  const auto job=c->gpr[4];auto a=job<=UINT32_MAX?allocations.find(uint32_t(job)):allocations.end();
  if(a==allocations.end()||a->second.vm!=stats.vm||a->second.module!=0x1f6ba00||!range(job,0x2a8)||vm_read32(job+0x288)!=0x1f6ba00
     ||!range(c->gpr[19],688)||!range(c->gpr[17],0x68))return;
  const auto header=vm_read64(job+0x280);const uint64_t command=uint32_t(header),bytes=(header>>48)&0xfff,user=vm_read32(job+0x28c);
  const uint64_t marker=uint64_t(depth_root.marker)+0x2bb0+uint64_t(camera)*0x140;
  if(((header>>32)&0xffff)!=1||bytes!=240||user!=command+80||!range(command,bytes)||!range(user,160)||!range(marker,320))return;
  dependencies=phase->tickets;dependencies.push_back(minimap.ticket);
  for(const auto&t:phase->depth_tickets){if(t.marker!=marker+80*t.slot||t.bucket!=c->gpr[17])return;
   for(unsigned i=0;i<4;++i)if(t.lists[i]!=vm_read32(uint64_t(t.bucket)+0x28+3*16+4*i))return;dependencies.push_back(t);}
  if(dependencies.size()!=6)return;
  input={1,sizeof input,stats.vm,next(),c->thread_id,next(),mag_render_update_current(c),camera,
    uint32_t(c->gpr[17]),uint32_t(marker),uint32_t(c->gpr[19]),uint32_t(user),uint32_t(job),a->second.id};
  if(!input.invocation||!input.occurrence||!input.execution_update)return;
  phase->depth_copy_active=true;phase->depth_captured|=1u<<camera;
 }
 struct End {std::shared_ptr<Phase>phase;~End(){std::lock_guard lock(mutex);phase->depth_copy_active=false;}} end{phase};
 std::vector<std::array<uint32_t,2>>wakes;
 for(const auto&t:dependencies){std::array<uint32_t,2>op{};{std::lock_guard lock(mutex);if(!ticket_current(t))return;
  op={vm_read32(t.container+4),vm_read32(t.container+8)};if((op[0]&127)||!range(op[0],128)||op[1]>=32)return;}
  if(std::find(wakes.begin(),wakes.end(),op)==wakes.end())wakes.push_back(op);}
 for(const auto&op:wakes)if(cellSpursReadyCountStore(reinterpret_cast<CellSpurs*>(uintptr_t(op[0])),op[1],5))return;
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 for(;;){bool complete=true;{std::lock_guard lock(mutex);if(!owner(phase,c)||phase!=active_regular||!phase->open||stats.lost)return;
   for(auto&t:dependencies){if(!ticket_current(t))return;if(t.sorted)for(unsigned i=0;i<4;++i)if(t.lists[i]!=vm_read32(uint64_t(t.bucket)+0x28+3*16+4*i))return;
    if(!ready(t))complete=false;}}
  if(complete)break;if(std::chrono::steady_clock::now()>=deadline)return;std::this_thread::yield();}
 CopyOwner copy_owner{c,input.vm,c->thread_id,std::this_thread::get_id(),phase};
 const auto result=mag_render_adapter_depth_sources(input,copy_depth_asset,&copy_owner);
 {std::lock_guard lock(mutex);if(!result.status&&stats.vm==input.vm){attempt.success=true;count(stats.depth_published);
  if(result.items>UINT64_MAX-stats.depth_items||result.bytes>UINT64_MAX-stats.depth_bytes)stats.lost=true;else{stats.depth_items+=result.items;stats.depth_bytes+=result.bytes;}}}
 static unsigned reports{};{std::lock_guard lock(mutex);if(reports<4){++reports;std::fprintf(stderr,
  "[rust-depth-source] vm=%llu invocation=%llu occurrence=%llu update=%llu camera=%u status=%u items=%llu reads=%llu bytes=%llu original_depth=retained\n",
  (unsigned long long)input.vm,(unsigned long long)input.invocation,(unsigned long long)input.occurrence,(unsigned long long)input.execution_update,input.camera,
  result.status,(unsigned long long)result.items,(unsigned long long)result.reads,(unsigned long long)result.bytes);}}
}

}
struct MagRenderCohortScope::Impl {
 ppu_context*c{};MagRenderCohortRole role;int exceptions{};uint64_t vm{},owner_id{},caller{};uint32_t entry_bucket{};std::array<uint64_t,8>args{};std::thread::id host;
 std::unique_lock<std::shared_mutex> reset_pin;
 explicit Impl(ppu_context*p,MagRenderCohortRole r):c(p),role(r),exceptions(std::uncaught_exceptions()),host(std::this_thread::get_id()){
  if(!c)return;if(!mag_render_hooks_enabled()){c=nullptr;return;}caller=c->lr;owner_id=c->thread_id;entry_bucket=uint32_t(c->gpr[24]);for(unsigned i=0;i<8;++i)args[i]=c->gpr[i];
  if(role==MagRenderCohortRole::Reset||role==MagRenderCohortRole::ResetAll)reset_pin=std::unique_lock<std::shared_mutex>(source_gate);
  std::unique_lock<std::shared_mutex> phase_pin;
  if(role==MagRenderCohortRole::Regular||role==MagRenderCohortRole::Main)phase_pin=std::unique_lock<std::shared_mutex>(source_gate);
  {std::lock_guard lock(mutex);vm=stats.vm;if(!vm)return;
   switch(role){
   case MagRenderCohortRole::Regular:{
    auto p=std::make_shared<Phase>();p->vm=vm;p->id=next();p->previous=regular?regular->id:0;p->owner=owner_id;p->renderer=uint32_t(args[3]);p->context=c;p->host=host;p->open=true;
    p->valid=args[3]<=UINT32_MAX&&range(args[3],0x3300)&&(!regular||!regular->open)&&((caller==0x14200&&args[2]==0x1b25b30)||(caller==0xd11834&&args[2]==0x1b94d70));
    if(active_regular)active_regular->valid=false;active_regular=regular=p;break;}
   case MagRenderCohortRole::Minimap:
    minimap_origin={c,vm,owner_id,0,0,uint32_t(args[3]),0,caller==0xded2c&&args[3]<=UINT32_MAX&&range(args[3],0x3300)};break;
   case MagRenderCohortRole::Main:{
    auto p=std::make_shared<Phase>();p->vm=vm;p->id=next();p->owner=owner_id;p->renderer=uint32_t(args[3]);p->context=c;p->host=host;p->open=true;
    p->valid=!main_phase&&caller==0xd11714&&args[3]<=UINT32_MAX&&owner(regular,c)&&!regular->open&&regular==active_regular&&regular->renderer==p->renderer;
    if(p->valid){p->producer_seen=regular->producer_seen;p->producer_bad=regular->producer_bad;p->producer_window=regular->producer_window;p->producer_bank=regular->producer_bank;p->producer_toc=regular->producer_toc;p->tickets=regular->tickets;if(minimap.valid&&minimap.vm==vm&&minimap.host==host&&minimap.context==c&&minimap.owner==owner_id&&minimap.renderer==p->renderer&&minimap.id>regular->previous)p->tickets.push_back(minimap.ticket);if(p->tickets.size()!=2)p->valid=false;}
    if(active_main)active_main->valid=false;active_main=main_phase=p;break;}
   case MagRenderCohortRole::DepthSort:
    if(depth_sources_enabled())sort_source={c,uint32_t(args[5]),0,owner(regular,c)&&regular==active_regular&&regular->open&&caller==0xd0d0b8&&args[3]==3&&args[4]==0&&args[6]==1&&args[5]<=UINT32_MAX&&range(args[5],0x68),3};break;
   case MagRenderCohortRole::DepthRoot:{
    if(!depth_sources_enabled())break;
    const uint32_t marker=args[2]>=0x2af8&&range(args[2]-0x2af8,4)?vm_read32(args[2]-0x2af8):0;
    depth_root={c,vm,owner_id,args[1],args[3],uint32_t(args[3])-0x31d0,marker,false};
    depth_root.valid=owner(regular,c)&&regular==active_regular&&regular->open&&caller==0xd0c3a8&&args[6]==0&&args[7]==3
      &&args[3]>=0x31d0&&args[3]<=UINT32_MAX&&args[1]>=0x4d0&&depth_root.renderer==regular->renderer;break;}
   case MagRenderCohortRole::Sort:
    sort_source={c,uint32_t(args[5]),0,owner(main_phase,c)&&main_phase->open&&caller==0xd11268&&args[3]==0&&args[4]==0&&args[6]==1&&args[5]<=UINT32_MAX&&range(args[5],0x68)};
    if(main_phase){if(main_phase->bucket&&main_phase->bucket!=sort_source.bucket)sort_source.valid=false;main_phase->bucket=sort_source.bucket;if(!sort_source.valid)main_phase->valid=false;}break;
   case MagRenderCohortRole::Root:{
    const uint32_t marker=args[2]>=0x2af8&&range(args[2]-0x2af8,4)?vm_read32(args[2]-0x2af8):0;
    root={c,vm,owner_id,args[1],args[3],uint32_t(args[3])-0x31d0,marker,false};
    root.valid=owner(main_phase,c)&&!root.valid&&caller==0xd11438&&args[6]==3&&args[7]==0&&args[3]>=0x31d0&&args[3]<=UINT32_MAX&&args[1]>=0x4d0&&root.renderer==main_phase->renderer;break;}
   case MagRenderCohortRole::Reset:case MagRenderCohortRole::ResetAll:{
    if(args[3]>UINT32_MAX||!range(args[3],4)||(role==MagRenderCohortRole::Reset&&args[4]>7)){fail();break;}
    const unsigned first=role==MagRenderCohortRole::ResetAll?0:unsigned(args[4]),last=role==MagRenderCohortRole::ResetAll?3:first;
    if(resets.size()>65536){fail();break;}for(unsigned i=first;i<=last;++i)resets[queue_key(uint32_t(args[3]),i)]=next();break;}
   default:break;
   }
  }
  if(role==MagRenderCohortRole::Producer)capture_producer(c);
  // Capture before the original submission's first instruction; never skip it.
  if(role==MagRenderCohortRole::Submit&&caller==0xcff5a4){if(args[3]==0x1f6b930)capture(c);else if(args[3]==0x1f6ba00)capture_depth(c);}
 }
 ~Impl(){try{
  if(!c||!vm)return;std::lock_guard lock(mutex);
  if(vm!=stats.vm)return;
  const bool normal=exceptions==std::uncaught_exceptions()&&owner_id==c->thread_id&&host==std::this_thread::get_id();
  switch(role){
  case MagRenderCohortRole::Regular:if(regular){regular->open=false;if(!normal)regular->valid=false;}break;
  case MagRenderCohortRole::Main:if(main_phase){main_phase->open=false;if(!normal)main_phase->valid=false;main_phase.reset();}break;
  case MagRenderCohortRole::Minimap:minimap_origin={};break;
  case MagRenderCohortRole::Sort:case MagRenderCohortRole::DepthSort:sort_source={};break;
  case MagRenderCohortRole::DepthRoot:depth_root={};break;
  case MagRenderCohortRole::Root:root={};break;
  case MagRenderCohortRole::Join:{
   const uint32_t bit=caller==0xd11358&&args[3]==0x1f6c740?1:caller==0xd11368&&args[3]==0x1f6c560?2:0;
   if(main_phase){if(!normal||!owner(main_phase,c)||!bit||args[4]!=0x1e82ef0||(main_phase->joins&bit))main_phase->valid=false;else main_phase->joins|=bit;}break;}
  case MagRenderCohortRole::Allocate:{
   const auto job=c->gpr[3];
   if(!normal||job>UINT32_MAX||args[3]>UINT32_MAX||!range(job,0x2a8)||vm_read32(job+0x288)!=args[3]||vm_read32(job+0x290)!=job)break;
   if(allocations.size()>1048576){fail();break;}allocations[uint32_t(job)]={next(),vm,uint32_t(args[3])};
   break;}
  case MagRenderCohortRole::Barrier:{
   if(!normal||args[3]>UINT32_MAX||args[4]>UINT32_MAX)break;
   const uint32_t bit=args[5]==0x1e0eb14&&caller==0xd0d044?1:args[5]==0x1e0eb24&&caller==0xd09a08?2:0;
   if(!bit)break;Ticket t;
   const auto result=args[3];const bool valid=range(result,8)&&vm_read32(result+4)==args[4]&&make_ticket(uint32_t(args[4]),vm_read16(result),t);
   if(bit==1){if(owner(regular,c)&&regular->open){if(!valid||!regular->tickets.empty())regular->valid=false;else regular->tickets.push_back(t);}}
   else{minimap={vm,next(),minimap_origin.renderer,c,owner_id,host,t,valid&&minimap_origin.valid&&minimap_origin.context==c&&minimap_origin.vm==vm};}break;}
  case MagRenderCohortRole::Submit:{
   if(!sort_source.valid)break;
   auto p=sort_source.family==3?regular:main_phase;if(!p)break;const auto job=args[4];const uint32_t bucket=entry_bucket;
   if(!normal||!owner(p,c)||p->sealed||sort_source.context!=c||args[3]!=sort_source.module||bucket!=sort_source.bucket||!range(job,0x2a8)
     ||!(caller==0xcfdbb0||caller==0xcfdcac||caller==0xcfdda8||caller==0xcfdea4)){p->valid=false;break;}
   auto a=job<=UINT32_MAX?allocations.find(uint32_t(job)):allocations.end();
   if(a==allocations.end()||a->second.vm!=vm||a->second.module!=args[3]||vm_read32(job+0x288)!=args[3]){p->valid=false;break;}
   const auto header=vm_read64(job+0x280);const uint64_t start=uint32_t(header),bytes=(header>>48)&0xfff,user=vm_read32(job+0x28c);
   if(((header>>32)&0xffff)!=1||!bytes||bytes>256||!range(start,bytes)||user<start||user+32>start+bytes||vm_read32(user+20)!=sort_source.family){p->valid=false;break;}
   Ticket t;t.sorted=true;t.slot=vm_read32(user+12);t.marker=vm_read32(user);t.bucket=bucket;t.job=uint32_t(job);t.allocation=a->second.id;
   if(t.slot>=4||!make_ticket(vm_read32(job+0x2a4),vm_read16(job+0x2a0),t)||++(sort_source.family==3?p->depth_sorts:p->sorts)[t.slot]!=1){p->valid=false;break;}
   for(unsigned i=0;i<4;++i)t.lists[i]=vm_read32(uint64_t(bucket)+0x28+sort_source.family*16+i*4);
   (sort_source.family==3?p->depth_tickets:p->tickets).push_back(t);break;}
  default:break;
  }
 }catch(...){std::lock_guard lock(mutex);fail();}
 }
};
MagRenderCohortScope::MagRenderCohortScope(ppu_context*c,MagRenderCohortRole r)noexcept{if(!mag_render_hooks_enabled())return;mag_render_frontend_vm_ensure();try{impl=std::make_unique<Impl>(c,r);}catch(...){std::lock_guard lock(mutex);fail();}}
MagRenderCohortScope::~MagRenderCohortScope()noexcept{try{impl.reset();}catch(...){std::lock_guard lock(mutex);fail();}}
// The fog camera now arrives with the producer sources, and the prepared fog
// rows are not acquired yet, so this seam has nothing to observe. The hook stays
// so the cohort stage keeps its pinned shape; it does no work.
void mag_render_cohort_fog_parameters(ppu_context*) noexcept {}
void mag_render_cohort_vm(uint64_t vm)noexcept{if(!mag_render_hooks_enabled())return;try{std::unique_lock pin(source_gate);
 std::lock_guard lock(mutex);if(active_main)active_main->valid=false;if(active_regular)active_regular->valid=false;active_main.reset();active_regular.reset();allocations.clear();resets.clear();stats={};stats.vm=vm;(void)mag_render_adapter_cohort_vm(vm);}catch(...){std::lock_guard lock(mutex);fail();}}
void mag_render_cohort_sort_selected(ppu_context*c)noexcept{if(!mag_render_hooks_enabled())return;try{std::lock_guard lock(mutex);auto phase=sort_source.family==3?regular:main_phase;if(!owner(phase,c))return;const auto toc=c->gpr[2];const uint32_t module=toc>=0x2ab4&&range(toc-0x2ab4,4)?vm_read32(toc-0x2ab4):0;
 if(!sort_source.valid||sort_source.context!=c||sort_source.module||c->gpr[30]!=sort_source.family||c->gpr[22]!=0||c->gpr[31]!=1||c->gpr[29]!=sort_source.bucket||!range(module,0xc8)){phase->valid=false;return;}sort_source.module=module;
}catch(...){std::lock_guard lock(mutex);fail();}}
void mag_render_cohort_pool_before(uint32_t job)noexcept{if(!mag_render_hooks_enabled())return;try{
 std::lock_guard lock(mutex);if(!stats.vm)return;invalidate_pool(job);auto a=allocations.find(job);if(a!=allocations.end())returning[job]=a->second.id;}catch(...){std::lock_guard lock(mutex);fail();}}
void mag_render_cohort_pool_returned(uint32_t job)noexcept{if(!mag_render_hooks_enabled())return;try{
 std::lock_guard lock(mutex);auto old=returning.find(job);if(old==returning.end())return;auto a=allocations.find(job);if(a!=allocations.end()&&a->second.id==old->second)allocations.erase(a);returning.erase(old);}catch(...){std::lock_guard lock(mutex);fail();}}
MagRenderCohortStats mag_render_cohort_stats()noexcept{try{std::lock_guard lock(mutex);return stats;}catch(...){MagRenderCohortStats s{};s.lost=true;return s;}}
