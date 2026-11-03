/* Injected CPU COM interfaces only. No D3D runtime, real device, queue or GPU.
 * Unused interface methods fail loudly; the exercised methods model only
 * identity/metadata/reference counts and genuine fence-value observations. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../native/mag_render_d3d12.cpp"
#include <cassert>
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
extern "C" mag_native_device *mag_native_wrap(const mag_native_ops *, void *) {
  assert(false);
  return nullptr;
}
extern "C" uint64_t mag_native_next_generation(void) { return 1; }
struct Cpu3D12Device final : ID3D12Device {
  ULONG refs = 1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override {
    *out = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    assert(refs);
    return --refs;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT,
                                           const void *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,
                                                    const IUnknown *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }

  UINT STDMETHODCALLTYPE GetNodeCount() override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid,
                     void **command_queue) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid,
                         void **command_allocator) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE
  CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                              REFIID riid, void **pipeline_state) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE
  CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                             REFIID riid, void **pipeline_state) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandList(UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
                    ID3D12CommandAllocator *command_allocator,
                    ID3D12PipelineState *initial_pipeline_state, REFIID riid,
                    void **command_list) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(D3D12_FEATURE feature, void *feature_data,
                      UINT feature_data_size) override {
    assert(feature == D3D12_FEATURE_FORMAT_SUPPORT &&
           feature_data_size == sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT));
    static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT *>(feature_data)->Support1 =
        static_cast<D3D12_FORMAT_SUPPORT1>(~0u);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid,
                       void **descriptor_heap) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE CreateRootSignature(
      UINT node_mask, const void *bytecode, SIZE_T bytecode_length, REFIID riid,
      void **root_signature) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  void STDMETHODCALLTYPE
  CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
                           D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    assert(!"unexpected CPU COM call");
  }

  void STDMETHODCALLTYPE CreateShaderResourceView(
      ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    assert(!"unexpected CPU COM call");
  }

  void STDMETHODCALLTYPE CreateUnorderedAccessView(
      ID3D12Resource *resource, ID3D12Resource *counter_resource,
      const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    assert(!"unexpected CPU COM call");
  }

  void STDMETHODCALLTYPE CreateRenderTargetView(
      ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    assert(!"unexpected CPU COM call");
  }

  void STDMETHODCALLTYPE CreateDepthStencilView(
      ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    assert(!"unexpected CPU COM call");
  }

  void STDMETHODCALLTYPE
  CreateSampler(const D3D12_SAMPLER_DESC *desc,
                D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    assert(!"unexpected CPU COM call");
  }

  void STDMETHODCALLTYPE CopyDescriptors(
      UINT dst_descriptor_range_count,
      const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets,
      const UINT *dst_descriptor_range_sizes, UINT src_descriptor_range_count,
      const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets,
      const UINT *src_descriptor_range_sizes,
      D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
    assert(!"unexpected CPU COM call");
  }

  void STDMETHODCALLTYPE CopyDescriptorsSimple(
      UINT descriptor_count,
      const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset,
      const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset,
      D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
    assert(!"unexpected CPU COM call");
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE GetResourceAllocationInfo(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
      UINT reource_desc_count,
      const D3D12_RESOURCE_DESC *resource_descs) override {
    assert(!"unexpected CPU COM call");
    return {};
  }
  D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE
  GetResourceAllocationInfo(UINT visible_mask, UINT reource_desc_count,
                            const D3D12_RESOURCE_DESC *resource_descs) {
    D3D12_RESOURCE_ALLOCATION_INFO __ret;
    return *GetResourceAllocationInfo(&__ret, visible_mask, reource_desc_count,
                                      resource_descs);
  }
#else
  D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(
      UINT visible_mask, UINT reource_desc_count,
      const D3D12_RESOURCE_DESC *resource_descs) override {
    assert(!"unexpected CPU COM call");
    return {};
  }
#endif

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE
  GetCustomHeapProperties(D3D12_HEAP_PROPERTIES *__ret, UINT node_mask,
                          D3D12_HEAP_TYPE heap_type) override {
    assert(!"unexpected CPU COM call");
    return {};
  }
  D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE
  GetCustomHeapProperties(UINT node_mask, D3D12_HEAP_TYPE heap_type) {
    D3D12_HEAP_PROPERTIES __ret;
    return *GetCustomHeapProperties(&__ret, node_mask, heap_type);
  }
#else
  D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE
  GetCustomHeapProperties(UINT node_mask, D3D12_HEAP_TYPE heap_type) override {
    assert(!"unexpected CPU COM call");
    return {};
  }
#endif

  HRESULT STDMETHODCALLTYPE CreateCommittedResource(
      const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
      const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
      const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid,
      void **resource) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC *desc, REFIID riid,
                                       void **heap) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE CreatePlacedResource(
      ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC *desc,
      D3D12_RESOURCE_STATES initial_state,
      const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid,
      void **resource) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE CreateReservedResource(
      const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
      const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid,
      void **resource) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE CreateSharedHandle(
      ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes,
      DWORD access, const WCHAR *name, HANDLE *handle) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE handle, REFIID riid,
                                             void **object) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(const WCHAR *name,
                                                   DWORD access,
                                                   HANDLE *handle) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE
  MakeResident(UINT object_count, ID3D12Pageable *const *objects) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE Evict(UINT object_count,
                                  ID3D12Pageable *const *objects) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initial_value,
                                        D3D12_FENCE_FLAGS flags, REFIID riid,
                                        void **fence) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return S_OK; }

  void STDMETHODCALLTYPE GetCopyableFootprints(
      const D3D12_RESOURCE_DESC *desc, UINT first_sub_resource,
      UINT sub_resource_count, UINT64 base_offset,
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *row_count,
      UINT64 *row_size, UINT64 *total_bytes) override {
    assert(!"unexpected CPU COM call");
  }

  HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *desc,
                                            REFIID riid, void **heap) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL enable) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC *desc,
                         ID3D12RootSignature *root_signature, REFIID riid,
                         void **command_signature) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  void STDMETHODCALLTYPE GetResourceTiling(
      ID3D12Resource *resource, UINT *total_tile_count,
      D3D12_PACKED_MIP_INFO *packed_mip_info,
      D3D12_TILE_SHAPE *standard_tile_shape, UINT *sub_resource_tiling_count,
      UINT first_sub_resource_tiling,
      D3D12_SUBRESOURCE_TILING *sub_resource_tilings) override {
    assert(!"unexpected CPU COM call");
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  LUID *STDMETHODCALLTYPE GetAdapterLuid(LUID *__ret) override {
    assert(!"unexpected CPU COM call");
    return {};
  }
  LUID STDMETHODCALLTYPE GetAdapterLuid() {
    LUID __ret;
    return *GetAdapterLuid(&__ret);
  }
#else
  LUID STDMETHODCALLTYPE GetAdapterLuid() override {
    assert(!"unexpected CPU COM call");
    return {};
  }
#endif
};
struct Cpu3D12Resource final : ID3D12Resource {
  ULONG refs = 1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override {
    *out = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    assert(refs);
    return --refs;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT,
                                           const void *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,
                                                    const IUnknown *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **out) override {
    *out = device;
    device->AddRef();
    return S_OK;
  }
  ID3D12Device *device = nullptr;
  D3D12_RESOURCE_DESC desc = {};
  HRESULT STDMETHODCALLTYPE Map(UINT sub_resource,
                                const D3D12_RANGE *read_range,
                                void **data) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  void STDMETHODCALLTYPE Unmap(UINT sub_resource,
                               const D3D12_RANGE *written_range) override {
    assert(!"unexpected CPU COM call");
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_RESOURCE_DESC *
      STDMETHODCALLTYPE GetDesc(D3D12_RESOURCE_DESC *__ret) override {
    *__ret = desc;
    return __ret;
  }
  D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() {
    D3D12_RESOURCE_DESC __ret;
    return *GetDesc(&__ret);
  }
#else
  D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc; }
#endif

  D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT dst_sub_resource,
                                               const D3D12_BOX *dst_box,
                                               const void *src_data,
                                               UINT src_row_pitch,
                                               UINT src_slice_pitch) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE ReadFromSubresource(
      void *dst_data, UINT dst_row_pitch, UINT dst_slice_pitch,
      UINT src_sub_resource, const D3D12_BOX *src_box) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE
  GetHeapProperties(D3D12_HEAP_PROPERTIES *heap_properties,
                    D3D12_HEAP_FLAGS *flags) override {
    assert(!"unexpected CPU COM call");
    return {};
  }
};
struct Cpu3D12Fence final : ID3D12Fence {
  ULONG refs = 1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override {
    *out = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    assert(refs);
    return --refs;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT,
                                           const void *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,
                                                    const IUnknown *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **out) override {
    *out = device;
    device->AddRef();
    return S_OK;
  }
  ID3D12Device *device = nullptr;
  uint64_t completed = 0;
  UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return completed; }

  HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value,
                                                 HANDLE event) override {
    assert(!"unexpected CPU COM call");
    return {};
  }

  HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override {
    assert(!"unexpected CPU COM call");
    return {};
  }
};

struct CpuHost {
  unsigned pins = 0, releases = 0;
  uint64_t id = 3, generation = 9;
  bool retired = false;
  MagRenderD3D12Image payload = {};
};
static bool active;
static unsigned begins, ends, gated_releases;
static int retain_image(void *u, uint64_t life, uint64_t id, uint64_t gen) {
  auto &h = *static_cast<CpuHost *>(u);
  assert(!active);
  if (life != 17 || id != h.id || gen != h.generation || h.retired)
    return -3;
  ++h.pins;
  return 0;
}
static void release_image(void *u, uint64_t life, uint64_t id, uint64_t gen) {
  auto &h = *static_cast<CpuHost *>(u);
  assert(life == 17 && id == h.id && gen == h.generation && h.pins);
  if (active)
    ++gated_releases;
  --h.pins;
  ++h.releases;
}
static int validate_image(void *u, uint64_t, uint64_t, uint64_t) {
  assert(active);
  return static_cast<CpuHost *>(u)->retired ? -3 : 0;
}
static int validate_planes(void *u, uint64_t l, uint64_t i, uint64_t g,
                           uint32_t p) {
  assert(p == 1);
  return validate_image(u, l, i, g);
}
static int begin_owner(void *, uint64_t life) {
  assert(life == 17 && !active);
  active = true;
  ++begins;
  return 0;
}
static void end_owner(void *, uint64_t life) {
  assert(life == 17 && active);
  active = false;
  ++ends;
}
int main() {
  Cpu3D12Device device, foreign;
  Cpu3D12Resource resource;
  Cpu3D12Fence fence;
  resource.device = &device;
  fence.device = &device;
  resource.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  resource.desc.Width = resource.desc.Height = 4;
  resource.desc.DepthOrArraySize = resource.desc.MipLevels =
      resource.desc.SampleDesc.Count = 1;
  resource.desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  resource.desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  Context c;
  c.generation = 1;
  c.owner.lifetime = 17;
  c.owner.try_begin = begin_owner;
  c.owner.end = end_owner;
  c.device.p = &device;
  device.AddRef();
  c.fence.p = &fence;
  fence.AddRef();
  CpuHost host;
  host.payload = {1, sizeof(host.payload), &resource,
                  D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_RENDER_TARGET};
  MagRenderHostImage h = {};
  h.abi_version = 1;
  h.struct_size = sizeof(h);
  h.backend = MAG_RENDER_HOST_D3D12;
  h.format = MAG_NATIVE_SCENE_RGBA8;
  h.width = h.height = 4;
  h.owner_lifetime = 17;
  h.resource_id = 3;
  h.resource_generation = 9;
  h.image = &host.payload;
  h.userdata = &host;
  h.retain = retain_image;
  h.release = release_image;
  h.validate = validate_image;
  mag_native_image ref = {};
  mag_native_image_certificate cert = {h, validate_planes};
  host.retired = true;
  h.image = reinterpret_cast<void *>(uintptr_t(1));
  assert(image_import(&c, &h, &ref) == MAG_NATIVE_STALE && !host.pins);
  host.retired = false;
  h.image = &host.payload;
  uint32_t short_payload[] = {9, 8};
  h.image = short_payload;
  assert(image_import(&c, &h, &ref) == MAG_NATIVE_INVALID && !host.pins);
  h.image = &host.payload;
  resource.device = &foreign;
  assert(image_import(&c, &h, &ref) == MAG_NATIVE_STALE && !host.pins);
  resource.device = &device;
  resource.desc.SampleDesc.Count = 2;
  assert(image_import(&c, &h, &ref) == MAG_NATIVE_UNSUPPORTED && !host.pins);
  resource.desc.SampleDesc.Count = 1;
  host.payload.acquire_state = D3D12_RESOURCE_STATE_COMMON;
  assert(image_import(&c, &h, &ref) == MAG_NATIVE_UNSUPPORTED && !host.pins);
  host.payload.acquire_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  assert(!image_import_certified(&c, &cert, &ref) && ref.id && host.pins == 1 &&
         resource.refs == 2);
  mag_native_scene_image image = {MAG_NATIVE_SCENE_RGBA8, 4, 4, 0, {0, 0, 0}};
  mag_native_scene_pass pass = {};
  pass.color = {0, MAG_NATIVE_SCENE_LOAD, MAG_NATIVE_SCENE_STORE, 0, {0}};
  pass.depth.image = MAG_NATIVE_SCENE_NONE;
  mag_native_scene scene = {};
  scene.struct_size = sizeof(scene);
  scene.abi_version = 1;
  scene.images = &image;
  scene.image_count = 1;
  scene.passes = &pass;
  scene.pass_count = 1;
  scene.output_image = 0;
  mag_native_stencil_attachment stencil = {};
  stencil.image = MAG_NATIVE_SCENE_NONE;
  mag_native_scene_import imported = {0, 1, ref};
  mag_native_scene_v5 v5 = {};
  v5.struct_size = sizeof(v5);
  v5.abi_version = 5;
  v5.base.struct_size = sizeof(v5.base);
  v5.base.abi_version = 4;
  v5.base.base.struct_size = sizeof(v5.base.base);
  v5.base.base.abi_version = 3;
  v5.base.base.base = {sizeof(v5.base.base.base), 2, &scene, nullptr, 0, 0};
  v5.base.stencils = &stencil;
  v5.base.stencil_count = 1;
  v5.imports = &imported;
  v5.import_count = 1;
  mag_native_scene_image alias_images[2] = {image, image};
  mag_native_scene_pass alias_passes[2] = {pass, pass};
  alias_passes[1].color.image = 1;
  mag_native_scene_import alias_imports[2] = {{0, 1, ref}, {1, 1, ref}};
  mag_native_stencil_attachment alias_stencils[2] = {stencil, stencil};
  mag_native_scene alias_scene = scene;
  alias_scene.images = alias_images;
  alias_scene.passes = alias_passes;
  alias_scene.image_count = 2;
  alias_scene.pass_count = 2;
  alias_scene.output_image = 1;
  auto alias = v5;
  alias.base.base.base.scene = &alias_scene;
  alias.base.stencils = alias_stencils;
  alias.base.stencil_count = 2;
  alias.imports = alias_imports;
  alias.import_count = 2;
  mag_native_ticket rejected = {};
  assert(submit_scene_v5(&c, &alias, nullptr, nullptr, &rejected) ==
             MAG_NATIVE_INVALID &&
         !rejected.submission_id && begins == 1 && ends == 1);
  c.slots[0].occupied = c.slots[1].occupied = true;
  mag_native_ticket ticket = {};
  assert(submit_scene_v5(&c, &v5, nullptr, nullptr, &ticket) ==
             MAG_NATIVE_BUSY &&
         !ticket.submission_id && begins == 2 && ends == 2 && !active);
  for (unsigned i = 0; i < 2; ++i) {
    auto &slot = c.slots[i];
    slot.scene_images.resize(1);
    slot.scene_images[0].borrowed = imported_image(&c, ref);
    slot.scene_images[0].gpu = imported_image(&c, ref)->gpu;
    slot.fence = 5 + i;
    slot.ticket = {1, 5 + i};
  }
  host.retired = true;
  assert(!image_release(&c, ref) && host.pins == 1);
  assert(image_release(&c, ref) == MAG_NATIVE_STALE);
  mag_native_event events[2];
  uint32_t count = 0;
  fence.completed = 4;
  assert(!poll(&c, events, 2, &count) && !count && host.pins == 1);
  fence.completed = 5;
  assert(!poll(&c, events, 2, &count) && count == 1 && host.pins == 1);
  count = 0;
  fence.completed = 6;
  assert(!poll(&c, events, 2, &count) && count == 1 && !host.pins &&
         resource.refs == 1);
  assert(!gated_releases);
  Cpu3D12Resource fresh_resource;
  fresh_resource.device = &device;
  fresh_resource.desc = resource.desc;
  CpuHost fresh;
  fresh.generation = 10;
  fresh.payload = host.payload;
  fresh.payload.resource = &fresh_resource;
  h.userdata = &fresh;
  h.image = &fresh.payload;
  h.resource_generation = 10;
  mag_native_image fresh_ref = {};
  assert(!image_import(&c, &h, &fresh_ref) && fresh.pins == 1);
  assert(!begin_owner(nullptr, 17));
  assert(!image_release(&c, fresh_ref));
  assert(!fresh.pins && fresh_resource.refs == 1 && gated_releases == 1);
  end_owner(nullptr, 17);
  assert(operations.image_import == image_import &&
         operations.submit_scene_v5 == submit_scene_v5);
  c.fence = {};
  c.device = {};
  assert(device.refs == 1 && foreign.refs == 1 && fence.refs == 1);
  puts("PASS D3D12 CPU COM import: retain-before-read, same-device/metadata, "
       "one V5 lease, two-fence slot retirement; no runtime/device/queue/GPU");
}
