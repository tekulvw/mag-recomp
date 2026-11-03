/* Actual SDK provider/boundary, injected CPU COM dispatch only. No D3D runtime,
 * real device, queue, window or GPU executes. Build against the explicit SDK.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "rsx_live_draw.c"
#include "rsx_picture_identity.inc"
#include <assert.h>
static unsigned refs_a = 1, refs_b = 1, refs_c = 1, barriers, executions,
                signals, resets;
static UINT64 completed;
static HRESULT removed = S_OK;
static ID3D12Resource resource_a, resource_b, resource_c;
static ULONG STDMETHODCALLTYPE add_resource(ID3D12Resource *r) {
  return r == &resource_a ? ++refs_a : r == &resource_b ? ++refs_b : ++refs_c;
}
static ULONG STDMETHODCALLTYPE release_resource(ID3D12Resource *r) {
  unsigned *n = r == &resource_a   ? &refs_a
                : r == &resource_b ? &refs_b
                                   : &refs_c;
  assert(*n);
  return --*n;
}
static HRESULT STDMETHODCALLTYPE device_reason(ID3D12Device *d) {
  assert(d == g.dev);
  return removed;
}
static UINT64 STDMETHODCALLTYPE fence_value(ID3D12Fence *f) {
  assert(f == g.fence && s_native_thread.original_depth &&
         s_native_thread.gate == &s_native_owner);
  return completed;
}
static HRESULT STDMETHODCALLTYPE close_list(ID3D12GraphicsCommandList *l) {
  assert(l == g.list);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE reset_list(ID3D12GraphicsCommandList *l,
                                            ID3D12CommandAllocator *a,
                                            ID3D12PipelineState *p) {
  assert(l == g.list && a == g.alloc && !p);
  ++resets;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE reset_allocator(ID3D12CommandAllocator *a) {
  assert(a == g.alloc);
  ++resets;
  return S_OK;
}
static void STDMETHODCALLTYPE execute_cpu(ID3D12CommandQueue *q, UINT n,
                                          ID3D12CommandList *const *lists) {
  assert(q == g.queue && n == 1 && lists && s_native_thread.original_depth);
  ++executions;
}
static HRESULT STDMETHODCALLTYPE signal_cpu(ID3D12CommandQueue *q,
                                            ID3D12Fence *f, UINT64 v) {
  assert(q == g.queue && f == g.fence && v);
  completed = v;
  ++signals;
  return S_OK;
}
static void STDMETHODCALLTYPE barrier_cpu(ID3D12GraphicsCommandList *l, UINT n,
                                          const D3D12_RESOURCE_BARRIER *b) {
  assert(l == g.list && n && b);
  ++barriers;
}
int main(int argc, char **argv) {
  (void)argv;
  assert(!_putenv_s("PS3RECOMP_RSX_NATIVE_OWNER", "1"));
  static ID3D12ResourceVtbl rv;
  rv.AddRef = add_resource;
  rv.Release = release_resource;
  resource_a.lpVtbl = &rv;
  resource_b.lpVtbl = &rv;
  resource_c.lpVtbl = &rv;
  static ID3D12DeviceVtbl dv;
  dv.GetDeviceRemovedReason = device_reason;
  static ID3D12Device device;
  device.lpVtbl = &dv;
  static ID3D12FenceVtbl fv;
  fv.GetCompletedValue = fence_value;
  static ID3D12Fence fence;
  fence.lpVtbl = &fv;
  static ID3D12GraphicsCommandListVtbl lv;
  lv.Close = close_list;
  lv.Reset = reset_list;
  lv.ResourceBarrier = barrier_cpu;
  static ID3D12GraphicsCommandList list;
  list.lpVtbl = &lv;
  static ID3D12CommandAllocatorVtbl av;
  av.Reset = reset_allocator;
  static ID3D12CommandAllocator allocator;
  allocator.lpVtbl = &av;
  static ID3D12CommandQueueVtbl qv;
  qv.ExecuteCommandLists = execute_cpu;
  qv.Signal = signal_cpu;
  static ID3D12CommandQueue queue;
  queue.lpVtbl = &qv;
  g.ready = 1;
  g.dev = &device;
  g.fence = &fence;
  g.list = &list;
  g.alloc = &allocator;
  g.queue = &queue;
  g.fence_event = (HANDLE)(uintptr_t)1;
  s_native_owner.generation = 17;
  g.n_surfaces = 1;
  surface_t *s = &g.surfaces[0];
  s->tex = &resource_a;
  s->w = s->h = 4;
  s->fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  s->location = 0;
  s->offset = 128;
  s->pitch = 16;
  s->native_source_format = 8;
  s->native_aa_grid = 0x101;
  s->native_generation = ld_native_new_generation();
  s->native_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  s->native_state_known = 1;
  rsx_native_image_key key = {
      1,     sizeof(key), RSX_NATIVE_COLOR_IMAGE, 0, 128, 16, 8, 4, 4,
      0x101, {0, 0}};
  rsx_native_image_match match = {.abi_version = 1,
                                  .struct_size = sizeof(match)};
  assert(rsx_live_draw_native_image(&key, &match) == 1 &&
         match.native_handle == 1 && !match.queued_clear_planes);
  ++key.pitch;
  assert(!rsx_live_draw_native_image(&key, &match));
  --key.pitch;
  rsx_native_image old = {.abi_version = 1, .struct_size = sizeof(old)};
  assert(!rsx_live_draw_borrow_image(17, 1, &old));
  assert(!old.retain(old.userdata, 17, 1, old.resource_generation));
  assert(refs_a == 2 && s_native_owner.retained == 1);
  uint64_t no_ticket = 99;
  assert(rsx_live_draw_native_boundary(&no_ticket) == -4 && !no_ticket &&
         !executions && !signals);
  assert(rsx_live_draw_flush_checked());
  assert(executions == 1 && signals == 1 && resets == 2 &&
         !s_native_owner.dirty && g.fence_value == 1);
  uint64_t ticket = 99;
  assert(!rsx_live_draw_native_boundary(&ticket) && ticket == 1);
  completed = 0;
  assert(rsx_live_draw_native_boundary(&ticket) == -4 && !ticket);
  completed = 1;
  assert(!ld_borrowed_begin(&s_native_owner, 17));
  assert(rsx_live_draw_validate_image_planes(old.userdata, 17, 1,
                                             old.resource_generation, 1) == -5);
  s->native_clear_planes = 1;
  assert(!rsx_live_draw_validate_image_planes(old.userdata, 17, 1,
                                              old.resource_generation, 1));
  ld_borrowed_end(&s_native_owner, 17);
  g.n_zdepths = 1;
  zdepth_t *z = &g.zdepths[0];
  z->tex = &resource_c;
  z->w = z->h = 4;
  z->native_generation = ld_native_new_generation();
  z->native_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  z->native_state_known = 1;
  z->cleared = 1;
  z->native_clear_planes = 2;
  rsx_native_image depth = {.abi_version = 1, .struct_size = sizeof(depth)};
  assert(!rsx_live_draw_borrow_image(17, MAX_SURFACES + 1, &depth));
  assert(!depth.retain(depth.userdata, 17, depth.resource_id,
                       depth.resource_generation));
  assert(!ld_borrowed_begin(&s_native_owner, 17));
  assert(!rsx_live_draw_validate_image_planes(
      depth.userdata, 17, depth.resource_id, depth.resource_generation, 2));
  assert(
      rsx_live_draw_validate_image_planes(depth.userdata, 17, depth.resource_id,
                                          depth.resource_generation, 4) == -5);
  ld_borrowed_end(&s_native_owner, 17);
  depth.release(depth.userdata, 17, depth.resource_id,
                depth.resource_generation);
  assert(refs_c == 1);
  assert(ld_owner_original_begin());
  D3D12_RESOURCE_BARRIER b = {0};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = &resource_a;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  ld_native_resource_barriers(1, &b);
  ld_owner_original_finish(1);
  rsx_native_image bad = {.abi_version = 1, .struct_size = sizeof(bad)};
  assert(rsx_live_draw_borrow_image(17, 1, &bad) == -2);
  assert(ld_owner_original_begin());
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  ld_native_resource_barriers(1, &b);
  ld_owner_original_finish(1);
  assert(barriers == 2 && s->native_state_known);
  assert(ld_owner_original_begin());
  assert(retire_texture(s->tex));
  s->tex = &resource_b;
  s->native_generation = ld_native_new_generation();
  s->native_clear_planes = 0;
  ld_native_image_gc();
  ld_owner_original_finish(1);
  assert(old.retain(old.userdata, 17, 1, old.resource_generation) == -3 &&
         refs_a == 2);
  assert(rsx_live_draw_flush_checked());
  assert(refs_a == 1 && !g.n_retired_textures);
  assert(!ld_borrowed_begin(&s_native_owner, 17));
  assert(old.validate(old.userdata, 17, 1, old.resource_generation) == -3);
  /* Last retired lease may be released by native rollback/destruction while
   * already holding this exact owner gate: no recursive SRW acquisition. */
  old.release(old.userdata, 17, 1, old.resource_generation);
  assert(refs_a == 0 && !s_native_owner.retained && s_native_thread.native);
  ld_borrowed_end(&s_native_owner, 17);
  rsx_native_image next = {.abi_version = 1, .struct_size = sizeof(next)};
  assert(!rsx_live_draw_borrow_image(17, 1, &next));
  assert(next.resource_generation > old.resource_generation);
  assert(!next.retain(next.userdata, 17, 1, next.resource_generation));
  assert(ld_owner_original_begin());
  b.Transition.pResource = &resource_b;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  ld_native_resource_barriers(1, &b);
  ld_owner_original_finish(1);
  assert(!s->native_state_known &&
         rsx_live_draw_borrow_image(17, 1, &bad) == -2);
  if (argc > 1) {
    assert(rsx_live_draw_flush_checked());
    removed = DXGI_ERROR_DEVICE_REMOVED;
    assert(rsx_live_draw_native_boundary(&ticket) == -6 && !ticket &&
           s_native_owner.failed && s_native_owner.dirty);
    removed = S_OK;
    assert(rsx_live_draw_native_boundary(&ticket) == -6 && !ticket);
  } else {
    s_native_owner.closing = 1;
    assert(rsx_live_draw_native_boundary(&ticket) == -3 && !ticket);
  }
  next.release(next.userdata, 17, 1, next.resource_generation);
  assert(refs_b == 1 && !s_native_owner.retained);
  ld_native_image_records_clear();
  assert(!s_native_images);
  assert(executions == (argc > 1 ? 3u : 2u) && signals == executions);
  puts("PASS actual SDK D3D12 image/boundary CPU dispatch: exact "
       "key/generation/COM pins/recorded states/actual checked-flush CPU fence dispatch; no "
       "runtime/device/queue/GPU");
  return 0;
}
