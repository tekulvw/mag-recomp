#pragma once
/* Bridges from the cohort observer hooks (tools/render_hooks/cohort.py) to the
 * adapter's cohort, source-copy and depth-source C entry points
 * (crates/mag-render-adapter-scene). Observers only: original calls, drains,
 * guest writes and return values are unchanged. */
#include <cstdint>
#include <memory>
struct ppu_context;
extern "C" {
struct MagRenderCohortBegin {
 uint32_t abi_version,struct_bytes,camera,reserved;
 uint64_t vm,invocation,owner,pass_occurrence,execution_update;
 uint32_t group_counts[4][16];
};
struct MagRenderCohortItem {
 uint64_t vm,invocation;uint32_t shard,source_index,has_transform,reserved;
 uint32_t mbi[8],transform[28];
};
struct MagRenderDepthSourceInput {
 uint32_t abi_version,struct_bytes;uint64_t vm,invocation,owner,occurrence,execution_update;
 uint32_t camera,bucket,marker,pass,user,job;uint64_t allocation;
};
struct MagRenderCohortResourceInput {
 uint32_t abi_version,struct_bytes;uint64_t vm,invocation;
 uint32_t render_mode,technique_override_mask,geometry_resource_base,source_pass_address;
};
struct MagRenderCohortResourceResult {uint32_t status,reserved;uint64_t items,reads,bytes;};
using MagRenderSourceCopy=int32_t(*)(void*,uint32_t,uint32_t,uint8_t*);
MagRenderCohortResourceResult mag_render_adapter_depth_sources(MagRenderDepthSourceInput,MagRenderSourceCopy,void*);
using MagRenderLocatedSourceCopy=int32_t(*)(void*,uint32_t,uint32_t,uint32_t,uint8_t*);
struct MagRenderCohortResourceInputV3 {
 MagRenderCohortResourceInput base;
 MagRenderLocatedSourceCopy located_copy;void*located_opaque;
};
struct MagRenderCohortResourceInputV4 {
 MagRenderCohortResourceInputV3 base;uint32_t source_user_address,reserved;
};
struct MagRenderCohortResourceInputV5 {
 MagRenderCohortResourceInputV4 base;uint32_t source_toc_address,reserved;uint64_t producer_source_window;
};
struct MagRenderProducerSourceInput {
 uint32_t abi_version,struct_bytes;uint64_t vm,source_window,ppu_owner,execution_update;
 uint32_t source_toc_address,renderer_address,bank_arena,bank_index,group_flags;int32_t group_limit;uint32_t clip_min,clip_max;
};
uint32_t mag_render_adapter_producer_sources(MagRenderProducerSourceInput,MagRenderSourceCopy,void*);
MagRenderCohortResourceResult mag_render_adapter_cohort_assets_v5(const MagRenderCohortResourceInputV5*,MagRenderSourceCopy,void*);
MagRenderCohortResourceResult mag_render_adapter_cohort_assets_v4(const MagRenderCohortResourceInputV4*,MagRenderSourceCopy,void*);
uint32_t mag_render_adapter_cohort_vm(uint64_t);
uint32_t mag_render_adapter_set_located_view(const uint8_t*(*)(void*,uint32_t,uint32_t,uint32_t));
uint32_t mag_render_adapter_set_guest_view(const uint8_t*(*)(void*,uint32_t,uint32_t));
uint32_t mag_render_adapter_cohort_decline(uint64_t);
uint32_t mag_render_adapter_cohort_begin(MagRenderCohortBegin);
uint32_t mag_render_adapter_cohort_item(MagRenderCohortItem);
uint32_t mag_render_adapter_cohort_finish(uint64_t,uint64_t);
}
void mag_render_cohort_vm(uint64_t) noexcept;
// Source-qualified complete-call+drain scopes. Integers select hook roles;
// none is a caller readiness certificate, asset generation or fake epoch.
enum class MagRenderCohortRole:uint32_t {Regular,Minimap,Main,Sort,Root,Join,Allocate,Submit,Reset,ResetAll,Barrier,Producer,DepthRoot,DepthSort};
class MagRenderCohortScope {
 struct Impl;std::unique_ptr<Impl> impl;
public:
 MagRenderCohortScope(ppu_context*,MagRenderCohortRole) noexcept;
 ~MagRenderCohortScope() noexcept;
 MagRenderCohortScope(const MagRenderCohortScope&)=delete;
 MagRenderCohortScope&operator=(const MagRenderCohortScope&)=delete;
};
void mag_render_cohort_pool_before(uint32_t) noexcept;
void mag_render_cohort_pool_returned(uint32_t) noexcept;
void mag_render_cohort_sort_selected(ppu_context*) noexcept;
// Seam after the fog parameter call; currently does no work (see the source).
void mag_render_cohort_fog_parameters(ppu_context*) noexcept;
struct MagRenderCohortStats {uint64_t vm,arrivals,published,declined,items,asset_copies,asset_declined,asset_bytes,depth_arrivals,depth_published,depth_declined,depth_items,depth_bytes;bool lost;};
MagRenderCohortStats mag_render_cohort_stats() noexcept;
