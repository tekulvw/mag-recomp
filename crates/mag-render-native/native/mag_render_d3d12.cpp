/* Frontend-owned D3D12 backend. Borrows one real host device/direct queue;
 * never creates a renderer device, window, swapchain or replacement queue. */
extern "C" {
#include "mag_render_native_internal.h"
}
#include "mag_render_d3d12_limits.h"
#include "mag_render_dxc.h"
#define WIN32_LEAN_AND_MEAN
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d12.h>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <objbase.h>
#include <string>
#include <utility>
#include <windows.h>

namespace {
template <class T> struct Ref {
  T *p = nullptr;
  Ref() = default;
  Ref(const Ref &other) : p(other.p) {
    if (p)
      p->AddRef();
  }
  Ref(Ref &&other) noexcept : p(other.p) { other.p = nullptr; }
  Ref &operator=(Ref other) noexcept {
    std::swap(p, other.p);
    return *this;
  }
  ~Ref() {
    if (p)
      p->Release();
  }
  T **put() {
    if (p)
      p->Release();
    p = nullptr;
    return &p;
  }
  T *operator->() const { return p; }
  explicit operator bool() const noexcept { return p != nullptr; }
};
using Resource = Ref<ID3D12Resource>;
struct Family {
  uint64_t id = 0, last = 0;
  uint32_t public_versions = 0;
};
struct Upload {
  Resource gpu;
};
struct ImageReadback {
  Resource gpu;
  uint64_t pitch = 0, length = 0;
  uint32_t width = 0, height = 0;
};
struct Buffer {
  std::shared_ptr<Family> family;
  uint64_t version = 0, length = 0, allocation = 0;
  Resource gpu;
  std::shared_ptr<Upload> upload;
  std::shared_ptr<ImageReadback> image;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
  bool gpu_dirty = false, seeded = false;
};
struct Pipeline {
  uint64_t id = 0;
  bool graphics = false;
  Ref<ID3D12RootSignature> root;
  Ref<ID3D12PipelineState> gpu;
  uint32_t count = 0;
  std::array<mag_native_binding_layout, 16> bindings = {};
};
constexpr uint32_t MaxResources = MAG_NATIVE_BATCH_MAX_RESOURCES;
constexpr uint32_t MaxCommands = MAG_NATIVE_BATCH_MAX_COMMANDS;
constexpr uint32_t HeapDescriptors = MaxCommands * MAG_NATIVE_MAX_BINDINGS;
struct Attachment {
  Resource color, depth;
  std::shared_ptr<ImageReadback> readback;
};
struct Slot {
  Ref<ID3D12CommandAllocator> allocator;
  Ref<ID3D12GraphicsCommandList> list;
  Ref<ID3D12DescriptorHeap> descriptors, rtv, dsv;
  bool occupied = false;
  uint64_t fence = 0;
  mag_native_ticket ticket = {};
  uint32_t resources = 0, pipelines = 0, attachments = 0;
  std::array<std::shared_ptr<Buffer>, MaxResources> buffers = {};
  std::array<std::shared_ptr<Upload>, MaxResources> uploads = {};
  std::array<bool, MaxResources> writes = {};
  std::array<std::shared_ptr<Pipeline>, MaxCommands> programs = {};
  std::array<Attachment, MaxCommands> targets = {};
};
struct Read {
  bool occupied = false;
  uint64_t fence = 0, offset = 0, length = 0;
  HANDLE event = nullptr;
  std::shared_ptr<Buffer> buffer;
  std::shared_ptr<Upload> upload;
  Resource staging;
  Ref<ID3D12CommandAllocator> allocator;
  Ref<ID3D12GraphicsCommandList> list;
};
struct Context {
  MagRenderHostOwner owner = {};
  bool pinned = false, failed = false;
  uint64_t generation = 0, next_buffer = 0, next_pipeline = 0, next_ticket = 0,
           next_fence = 0;
  Ref<ID3D12Device> device;
  Ref<ID3D12CommandQueue> queue;
  Ref<ID3D12Fence> fence;
  HMODULE runtime = nullptr;
  PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize = nullptr;
  std::string compiler;
  std::map<std::pair<uint64_t, uint64_t>, std::shared_ptr<Buffer>> buffers;
  std::map<uint64_t, std::shared_ptr<Family>> families;
  std::map<uint64_t, std::shared_ptr<Pipeline>> pipelines;
  std::array<Slot, 2> slots;
  Read read;
  UINT descriptor_step = 0, rtv_step = 0, dsv_step = 0;
};
struct Bound {
  std::shared_ptr<Buffer> buffer;
  uint64_t offset = 0, length = 0;
  uint32_t layout = 0;
};
struct Command {
  bool draw = false;
  std::shared_ptr<Pipeline> pipeline;
  uint32_t count = 0;
  std::array<Bound, 16> bindings = {};
  std::array<uint32_t, 3> groups = {};
  std::shared_ptr<Buffer> vertices, indices, readback;
  mag_native_indexed_draw draw_desc = {};
};
struct Plan {
  uint32_t resources = 0, commands = 0, draws = 0;
  bool typed = false;
  std::array<std::shared_ptr<Buffer>, MaxResources> buffers = {};
  std::array<uint32_t, MaxResources> roles = {};
  std::array<bool, MaxResources> written = {};
  std::array<Command, MaxCommands> nodes = {};
};
struct Lease {
  Context *context = nullptr;
  ~Lease() {
    if (context)
      context->owner.end(context->owner.userdata, context->owner.lifetime);
  }
};
static bool same(mag_native_buffer a, mag_native_buffer b) {
  return a.device_generation == b.device_generation && a.id == b.id &&
         a.version == b.version;
}
static bool zero(mag_native_buffer b) {
  return !b.device_generation && !b.id && !b.version;
}
static uint64_t padded(uint64_t bytes) {
  return (bytes + 255u) & ~uint64_t(255);
}
static std::shared_ptr<Buffer> buffer(Context *c, mag_native_buffer ref) {
  if (ref.device_generation != c->generation || !ref.id || !ref.version)
    return {};
  auto found = c->buffers.find({ref.id, ref.version});
  return found == c->buffers.end() ? nullptr : found->second;
}
static std::shared_ptr<Pipeline> pipeline(Context *c, mag_native_pipeline ref) {
  if (ref.device_generation != c->generation || !ref.id)
    return {};
  auto found = c->pipelines.find(ref.id);
  return found == c->pipelines.end() ? nullptr : found->second;
}
static int failure(Context *c, HRESULT result) {
  if (c->device && FAILED(c->device->GetDeviceRemovedReason()))
    c->failed = true;
  return result == E_OUTOFMEMORY ? MAG_NATIVE_ALLOCATION_FAILED
                                 : MAG_NATIVE_DEVICE_FAILED;
}
static HRESULT
allocation(Context *c, D3D12_HEAP_TYPE type, uint64_t length,
           D3D12_RESOURCE_STATES state, Resource &out,
           D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = type;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = length;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  return c->device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
      __uuidof(ID3D12Resource), reinterpret_cast<void **>(out.put()));
}
static void clear_slot(Slot &slot) {
  for (auto &b : slot.buffers)
    b.reset();
  for (auto &u : slot.uploads)
    u.reset();
  for (auto &p : slot.programs)
    p.reset();
  for (auto &t : slot.targets)
    t = Attachment{};
  slot.writes = {};
  slot.resources = slot.pipelines = slot.attachments = 0;
  slot.occupied = false;
  slot.fence = 0;
  slot.ticket = {};
}
static bool uses(Context *c, const std::shared_ptr<Buffer> &b) {
  for (const auto &s : c->slots)
    if (s.occupied)
      for (uint32_t i = 0; i < s.resources; ++i)
        if (s.buffers[i] == b)
          return true;
  return c->read.occupied && c->read.buffer == b;
}
static void destroy(Context *c) {
  if (c->read.event)
    CloseHandle(c->read.event);
  c->read = Read{};
  for (auto &s : c->slots)
    s = Slot{};
  c->buffers.clear();
  c->families.clear();
  c->pipelines.clear();
  c->fence = {};
  c->queue = {};
  c->device = {};
  if (c->runtime)
    FreeLibrary(c->runtime);
  if (c->pinned)
    c->owner.release(c->owner.userdata, c->owner.lifetime);
  delete c;
}
static int caps(void *opaque, mag_native_caps_desc *out) {
  auto *c = static_cast<Context *>(opaque);
  *out = {sizeof(*out), 1, c->generation, 1, 1, 2, 16, UINT32_MAX, 0};
  return MAG_NATIVE_OK;
}
static int buffer_create(void *opaque, mag_native_buffer previous,
                         const mag_native_buffer_desc *desc,
                         mag_native_buffer *out) {
  auto *c = static_cast<Context *>(opaque);
  if (c->failed)
    return MAG_NATIVE_DEVICE_FAILED;
  if (!desc->byte_length || desc->byte_length % 4)
    return MAG_NATIVE_INVALID;
  if (desc->byte_length > UINT32_MAX - 255u)
    return MAG_NATIVE_UNSUPPORTED;
  try {
    std::shared_ptr<Family> family;
    if (previous.id) {
      auto old = buffer(c, previous);
      if (!old)
        return MAG_NATIVE_STALE;
      family = old->family;
      if (family->last == UINT64_MAX)
        return MAG_NATIVE_ALLOCATION_FAILED;
    } else {
      if (c->next_buffer == UINT64_MAX)
        return MAG_NATIVE_ALLOCATION_FAILED;
      family = std::make_shared<Family>();
      family->id = ++c->next_buffer;
    }
    auto b = std::make_shared<Buffer>();
    b->family = family;
    b->version = family->last + 1;
    b->length = desc->byte_length;
    b->allocation = padded(b->length);
    b->seeded = desc->initial_bytes != nullptr;
    HRESULT result = allocation(c, D3D12_HEAP_TYPE_DEFAULT, b->allocation,
                                D3D12_RESOURCE_STATE_COPY_DEST, b->gpu,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(result))
      return failure(c, result);
    b->upload = std::make_shared<Upload>();
    result = allocation(c, D3D12_HEAP_TYPE_UPLOAD, b->allocation,
                        D3D12_RESOURCE_STATE_GENERIC_READ, b->upload->gpu);
    if (FAILED(result))
      return failure(c, result);
    void *mapped = nullptr;
    D3D12_RANGE none = {0, 0};
    result = b->upload->gpu->Map(0, &none, &mapped);
    if (FAILED(result) || !mapped)
      return failure(c, FAILED(result) ? result : E_FAIL);
    std::memset(mapped, 0, static_cast<size_t>(b->allocation));
    if (desc->initial_bytes)
      std::memcpy(mapped, desc->initial_bytes, static_cast<size_t>(b->length));
    b->upload->gpu->Unmap(0, nullptr);
    auto key = std::make_pair(family->id, b->version);
    c->buffers.emplace(key, b);
    try {
      c->families.emplace(family->id, family);
    } catch (...) {
      c->buffers.erase(key);
      throw;
    }
    family->last = b->version;
    ++family->public_versions;
    *out = {c->generation, family->id, b->version};
    return MAG_NATIVE_OK;
  } catch (const std::bad_alloc &) {
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
}
static int buffer_release(void *opaque, mag_native_buffer ref) {
  auto *c = static_cast<Context *>(opaque);
  auto b = buffer(c, ref);
  if (!b)
    return MAG_NATIVE_STALE;
  c->buffers.erase({ref.id, ref.version});
  if (!--b->family->public_versions)
    c->families.erase(ref.id);
  return MAG_NATIVE_OK;
}
static int compile(Context *c, const mag_native_shader_stage &stage,
                   uint32_t kind, mag_render_dxc_blob &out) {
  if (stage.language != MAG_NATIVE_HLSL)
    return MAG_NATIVE_UNSUPPORTED;
  char diagnostic[4096] = {};
  int status = mag_render_dxc_compile_stage(
      c->compiler.c_str(), kind, stage.source, stage.source_length, stage.entry,
      stage.entry_length, &out, diagnostic, sizeof(diagnostic));
  if (status && diagnostic[0])
    std::fprintf(stderr, "[mag native D3D12] %s\n", diagnostic);
  return status;
}
static HRESULT root_signature(Context *c, const D3D12_ROOT_SIGNATURE_DESC &desc,
                              Ref<ID3D12RootSignature> &root) {
  Ref<ID3DBlob> bytes, errors;
  HRESULT result = c->serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                bytes.put(), errors.put());
  if (FAILED(result))
    return result;
  return c->device->CreateRootSignature(
      0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
      __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(root.put()));
}
static int pipeline_create(void *opaque, const mag_native_pipeline_desc *desc,
                           mag_native_pipeline *out) {
  auto *c = static_cast<Context *>(opaque);
  if (c->failed)
    return MAG_NATIVE_DEVICE_FAILED;
  if (desc->shader_language != MAG_NATIVE_HLSL)
    return MAG_NATIVE_UNSUPPORTED;
  if (!desc->bindings || !desc->binding_count || desc->binding_count > 16)
    return MAG_NATIVE_INVALID;
  uint64_t threads = 1;
  for (uint32_t n : desc->threads_per_group) {
    if (!n || n > 256)
      return MAG_NATIVE_INVALID;
    threads *= n;
  }
  if (threads > 256)
    return MAG_NATIVE_INVALID;
  try {
    auto p = std::make_shared<Pipeline>();
    p->count = desc->binding_count;
    std::array<bool, 16> seen = {};
    std::array<D3D12_DESCRIPTOR_RANGE, 16> ranges = {};
    std::array<D3D12_ROOT_PARAMETER, 16> parameters = {};
    for (uint32_t i = 0; i < p->count; ++i) {
      const auto &b = desc->bindings[i];
      if (b.slot >= 16 || seen[b.slot] || !b.minimum_bytes ||
          b.minimum_bytes > UINT32_MAX || !b.alignment_bytes ||
          (b.alignment_bytes & (b.alignment_bytes - 1)) ||
          (b.access != MAG_NATIVE_READ && b.access != MAG_NATIVE_READ_WRITE) ||
          b.binding_kind > MAG_NATIVE_UNIFORM ||
          (b.binding_kind == MAG_NATIVE_UNIFORM && b.access != MAG_NATIVE_READ))
        return MAG_NATIVE_INVALID;
      if (b.runtime_array_stride && b.minimum_bytes != b.runtime_array_stride)
        return MAG_NATIVE_UNSUPPORTED;
      if ((b.runtime_array_stride && b.runtime_size_index >= 16) ||
          (!b.runtime_array_stride &&
           b.runtime_size_index != MAG_NATIVE_NO_SIZE_INDEX))
        return MAG_NATIVE_INVALID;
      seen[b.slot] = true;
      p->bindings[i] = b;
      auto &r = ranges[i];
      r.RangeType =
          b.binding_kind == MAG_NATIVE_UNIFORM ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
          : b.access == MAG_NATIVE_READ_WRITE ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                                              : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      r.NumDescriptors = 1;
      r.BaseShaderRegister = b.slot;
      r.OffsetInDescriptorsFromTableStart =
          D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
      parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameters[i].DescriptorTable.NumDescriptorRanges = 1;
      parameters[i].DescriptorTable.pDescriptorRanges = &r;
    }
    D3D12_ROOT_SIGNATURE_DESC signature = {};
    signature.NumParameters = p->count;
    signature.pParameters = parameters.data();
    HRESULT result = root_signature(c, signature, p->root);
    if (FAILED(result))
      return failure(c, result);
    mag_render_dxc_blob bytes = {};
    mag_native_shader_stage stage = {
        desc->source_bytes, desc->source_length, desc->entry_point,
        desc->entry_point_length, desc->shader_language};
    int status = compile(c, stage, MAG_RENDER_DXC_COMPUTE, bytes);
    if (status)
      return status;
    D3D12_COMPUTE_PIPELINE_STATE_DESC gpu = {};
    gpu.pRootSignature = p->root.p;
    gpu.CS = {bytes.bytes, static_cast<SIZE_T>(bytes.length)};
    result = c->device->CreateComputePipelineState(
        &gpu, __uuidof(ID3D12PipelineState),
        reinterpret_cast<void **>(p->gpu.put()));
    mag_render_dxc_free(&bytes);
    if (FAILED(result))
      return failure(c, result);
    if (c->next_pipeline == UINT64_MAX)
      return MAG_NATIVE_ALLOCATION_FAILED;
    p->id = ++c->next_pipeline;
    c->pipelines.emplace(p->id, p);
    *out = {c->generation, p->id};
    return MAG_NATIVE_OK;
  } catch (const std::bad_alloc &) {
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
}
static int graphics_create(void *opaque, const mag_native_graphics_desc *desc,
                           mag_native_pipeline *out) {
  auto *c = static_cast<Context *>(opaque);
  if (c->failed)
    return MAG_NATIVE_DEVICE_FAILED;
  if (desc->reserved || desc->vertex_stride != 16)
    return MAG_NATIVE_UNSUPPORTED;
  try {
    auto p = std::make_shared<Pipeline>();
    p->graphics = true;
    D3D12_ROOT_SIGNATURE_DESC root = {};
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    HRESULT result = root_signature(c, root, p->root);
    if (FAILED(result))
      return failure(c, result);
    mag_render_dxc_blob vertex = {}, fragment = {};
    int status = compile(c, desc->vertex, MAG_RENDER_DXC_VERTEX, vertex);
    if (status)
      return status;
    status = compile(c, desc->fragment, MAG_RENDER_DXC_FRAGMENT, fragment);
    if (status) {
      mag_render_dxc_free(&vertex);
      return status;
    }
    D3D12_INPUT_ELEMENT_DESC input = {
        "LOC", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
        0,     0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
        0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gpu = {};
    gpu.pRootSignature = p->root.p;
    gpu.VS = {vertex.bytes, static_cast<SIZE_T>(vertex.length)};
    gpu.PS = {fragment.bytes, static_cast<SIZE_T>(fragment.length)};
    gpu.InputLayout = {&input, 1};
    gpu.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gpu.NumRenderTargets = 1;
    gpu.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpu.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    gpu.SampleDesc.Count = 1;
    gpu.SampleMask = UINT_MAX;
    gpu.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gpu.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gpu.RasterizerState.DepthClipEnable = TRUE;
    for (auto &blend : gpu.BlendState.RenderTarget) {
      blend.SrcBlend = D3D12_BLEND_ONE;
      blend.DestBlend = D3D12_BLEND_ZERO;
      blend.BlendOp = D3D12_BLEND_OP_ADD;
      blend.SrcBlendAlpha = D3D12_BLEND_ONE;
      blend.DestBlendAlpha = D3D12_BLEND_ZERO;
      blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      blend.LogicOp = D3D12_LOGIC_OP_NOOP;
      blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    gpu.DepthStencilState.StencilReadMask =
        gpu.DepthStencilState.StencilWriteMask = 0xff;
    gpu.DepthStencilState.FrontFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_COMPARISON_FUNC_ALWAYS};
    gpu.DepthStencilState.BackFace = gpu.DepthStencilState.FrontFace;
    gpu.DepthStencilState.DepthEnable = TRUE;
    gpu.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    gpu.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    result = c->device->CreateGraphicsPipelineState(
        &gpu, __uuidof(ID3D12PipelineState),
        reinterpret_cast<void **>(p->gpu.put()));
    mag_render_dxc_free(&vertex);
    mag_render_dxc_free(&fragment);
    if (FAILED(result))
      return failure(c, result);
    if (c->next_pipeline == UINT64_MAX)
      return MAG_NATIVE_ALLOCATION_FAILED;
    p->id = ++c->next_pipeline;
    c->pipelines.emplace(p->id, p);
    *out = {c->generation, p->id};
    return MAG_NATIVE_OK;
  } catch (const std::bad_alloc &) {
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
}
static int pipeline_release(void *opaque, mag_native_pipeline ref) {
  auto *c = static_cast<Context *>(opaque);
  auto p = pipeline(c, ref);
  if (!p)
    return MAG_NATIVE_STALE;
  c->pipelines.erase(ref.id);
  return MAG_NATIVE_OK;
}
static int resource_index(const Plan &plan, const std::shared_ptr<Buffer> &b) {
  for (uint32_t i = 0; i < plan.resources; ++i)
    if (plan.buffers[i] == b)
      return static_cast<int>(i);
  return -1;
}
static bool range(uint64_t size, uint64_t offset, uint64_t length) {
  return length && offset <= size && length <= size - offset;
}
static int prepare_frame(Context *c, const mag_native_frame *frame,
                         Command &command) {
  if (!frame || frame->struct_size != sizeof(*frame) ||
      frame->abi_version != 1 || !frame->bindings)
    return MAG_NATIVE_INVALID;
  command.pipeline = pipeline(c, frame->pipeline);
  if (!command.pipeline)
    return MAG_NATIVE_STALE;
  if (command.pipeline->graphics ||
      frame->binding_count != command.pipeline->count)
    return MAG_NATIVE_INVALID;
  command.count = frame->binding_count;
  for (unsigned axis = 0; axis < 3; ++axis) {
    if (!frame->groups[axis] || frame->groups[axis] > 65535)
      return MAG_NATIVE_INVALID;
    command.groups[axis] = frame->groups[axis];
  }
  std::array<bool, 16> seen = {};
  for (uint32_t i = 0; i < command.count; ++i) {
    const auto &supplied = frame->bindings[i];
    if (supplied.slot >= 16 || seen[supplied.slot] || supplied.reserved)
      return MAG_NATIVE_INVALID;
    seen[supplied.slot] = true;
    uint32_t layout = 0;
    while (layout < command.pipeline->count &&
           command.pipeline->bindings[layout].slot != supplied.slot)
      ++layout;
    if (layout == command.pipeline->count)
      return MAG_NATIVE_INVALID;
    const auto &definition = command.pipeline->bindings[layout];
    auto b = buffer(c, supplied.buffer);
    if (!b)
      return MAG_NATIVE_STALE;
    if (b->image || !range(b->length, supplied.offset, supplied.byte_length) ||
        supplied.byte_length < definition.minimum_bytes ||
        supplied.offset % definition.alignment_bytes || supplied.offset % 4 ||
        supplied.byte_length % 4 ||
        (definition.runtime_array_stride &&
         supplied.byte_length % definition.runtime_array_stride))
      return MAG_NATIVE_INVALID;
    if (definition.binding_kind == MAG_NATIVE_UNIFORM &&
        (supplied.offset % 256 || padded(supplied.byte_length) > 65536 ||
         supplied.offset + padded(supplied.byte_length) > b->allocation))
      return MAG_NATIVE_UNSUPPORTED;
    if (definition.binding_kind == MAG_NATIVE_STORAGE &&
        !mag_d3d12_raw_offset(supplied.offset))
      return MAG_NATIVE_UNSUPPORTED;
    for (uint32_t j = 0; j < i; ++j)
      if (command.bindings[j].buffer == b &&
          (definition.access == MAG_NATIVE_READ_WRITE ||
           command.pipeline->bindings[command.bindings[j].layout].access ==
               MAG_NATIVE_READ_WRITE))
        return MAG_NATIVE_INVALID;
    command.bindings[i] = {b, supplied.offset, supplied.byte_length, layout};
  }
  return MAG_NATIVE_OK;
}
static int prepare_draw(Context *c, const mag_native_indexed_draw *draw,
                        Command &command) {
  if (!draw || draw->reserved || draw->reserved1 || !draw->width ||
      !draw->height || draw->width > 4096 || draw->height > 4096 ||
      !draw->index_count || draw->index_count % 3 ||
      !std::isfinite(draw->clear_depth) || draw->clear_depth < 0 ||
      draw->clear_depth > 1)
    return MAG_NATIVE_INVALID;
  for (float value : draw->clear_color)
    if (!std::isfinite(value) || value < 0 || value > 1)
      return MAG_NATIVE_INVALID;
  command.pipeline = pipeline(c, draw->pipeline);
  if (!command.pipeline)
    return MAG_NATIVE_STALE;
  if (!command.pipeline->graphics)
    return MAG_NATIVE_INVALID;
  command.vertices = buffer(c, draw->vertices);
  command.indices = buffer(c, draw->indices);
  if (!command.vertices || !command.indices)
    return MAG_NATIVE_STALE;
  if (command.vertices == command.indices || command.vertices->image ||
      command.indices->image || draw->vertex_offset % 16 ||
      draw->vertex_length % 16 || draw->index_offset % 4 ||
      draw->index_length % 4 ||
      !range(command.vertices->length, draw->vertex_offset,
             draw->vertex_length) ||
      !range(command.indices->length, draw->index_offset, draw->index_length) ||
      uint64_t(draw->index_count) * 4 > draw->index_length ||
      draw->vertex_length > UINT_MAX || draw->index_length > UINT_MAX)
    return MAG_NATIVE_INVALID;
  if (!zero(draw->color_readback)) {
    command.readback = buffer(c, draw->color_readback);
    if (!command.readback)
      return MAG_NATIVE_STALE;
    if (command.readback == command.vertices ||
        command.readback == command.indices ||
        uint64_t(draw->width) * draw->height * 4 != command.readback->length)
      return MAG_NATIVE_INVALID;
  }
  command.draw = true;
  command.draw_desc = *draw;
  return MAG_NATIVE_OK;
}
static int prepare_batch(Context *c, const mag_native_batch *batch,
                         Plan &plan) {
  if (!batch || batch->struct_size != sizeof(*batch) ||
      batch->abi_version != 1 || !batch->resources || !batch->commands ||
      !batch->resource_count || batch->resource_count > MaxResources ||
      !batch->command_count || batch->command_count > MaxCommands)
    return MAG_NATIVE_INVALID;
  plan.typed = true;
  plan.resources = batch->resource_count;
  plan.commands = batch->command_count;
  uint64_t bytes = 0;
  for (uint32_t i = 0; i < plan.resources; ++i) {
    const auto &resource = batch->resources[i];
    if (resource.reserved || resource.role > MAG_NATIVE_READBACK)
      return MAG_NATIVE_INVALID;
    auto b = buffer(c, resource.buffer);
    if (!b)
      return MAG_NATIVE_STALE;
    for (uint32_t j = 0; j < i; ++j)
      if (same(resource.buffer, batch->resources[j].buffer))
        return MAG_NATIVE_INVALID;
    if ((resource.role != MAG_NATIVE_UPLOAD && b->seeded) ||
        (resource.role == MAG_NATIVE_UPLOAD && b->gpu_dirty) ||
        (b->image && resource.role != MAG_NATIVE_READBACK))
      return MAG_NATIVE_INVALID;
    bytes += b->length;
    if (bytes > MAG_NATIVE_BATCH_MAX_BYTES)
      return MAG_NATIVE_INVALID;
    plan.buffers[i] = b;
    plan.roles[i] = resource.role;
  }
  for (uint32_t index = 0; index < plan.commands; ++index) {
    const auto &supplied = batch->commands[index];
    auto &node = plan.nodes[index];
    if (supplied.reserved)
      return MAG_NATIVE_INVALID;
    if (supplied.kind == MAG_NATIVE_DISPATCH) {
      if (!supplied.dispatch || supplied.draw)
        return MAG_NATIVE_INVALID;
      int status = prepare_frame(c, supplied.dispatch, node);
      if (status)
        return status;
      std::array<bool, MaxResources> writes = {};
      for (uint32_t b = 0; b < node.count; ++b) {
        const auto &binding = node.bindings[b];
        int resource = resource_index(plan, binding.buffer);
        if (resource < 0)
          return MAG_NATIVE_INVALID;
        const auto &layout = node.pipeline->bindings[binding.layout];
        if (layout.access == MAG_NATIVE_READ_WRITE) {
          if (plan.roles[resource] != MAG_NATIVE_PRODUCED ||
              plan.written[resource] || writes[resource])
            return MAG_NATIVE_INVALID;
          writes[resource] = true;
        } else if (plan.roles[resource] == MAG_NATIVE_READBACK ||
                   (plan.roles[resource] == MAG_NATIVE_PRODUCED &&
                    !plan.written[resource]))
          return MAG_NATIVE_INVALID;
      }
      for (uint32_t i = 0; i < plan.resources; ++i)
        if (writes[i])
          plan.written[i] = true;
    } else if (supplied.kind == MAG_NATIVE_INDEXED_DRAW) {
      if (supplied.dispatch || !supplied.draw)
        return MAG_NATIVE_INVALID;
      int status = prepare_draw(c, supplied.draw, node);
      if (status)
        return status;
      if (!mag_d3d12_attachment_budget(&bytes, supplied.draw->width,
                                       supplied.draw->height,
                                       bool(node.readback)))
        return MAG_NATIVE_ALLOCATION_FAILED;
      for (const auto &value : {node.vertices, node.indices}) {
        int resource = resource_index(plan, value);
        if (resource < 0 || plan.roles[resource] != MAG_NATIVE_PRODUCED ||
            !plan.written[resource])
          return MAG_NATIVE_INVALID;
      }
      if (node.readback) {
        int resource = resource_index(plan, node.readback);
        if (resource < 0 || plan.roles[resource] != MAG_NATIVE_READBACK ||
            plan.written[resource])
          return MAG_NATIVE_INVALID;
        plan.written[resource] = true;
      }
      ++plan.draws;
    } else
      return MAG_NATIVE_INVALID;
  }
  for (uint32_t i = 0; i < plan.resources; ++i)
    if (plan.roles[i] == MAG_NATIVE_PRODUCED && !plan.written[i])
      return MAG_NATIVE_INVALID;
  return MAG_NATIVE_OK;
}
static int heap(Context *c, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count,
                bool visible, Ref<ID3D12DescriptorHeap> &out) {
  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  desc.Type = type;
  desc.NumDescriptors = count;
  desc.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                       : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  HRESULT result =
      c->device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap),
                                      reinterpret_cast<void **>(out.put()));
  return FAILED(result) ? failure(c, result) : MAG_NATIVE_OK;
}
static int attachment(Context *c, const Command &command, Attachment &out) {
  const auto &draw = command.draw_desc;
  D3D12_HEAP_PROPERTIES properties = {};
  properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  properties.CreationNodeMask = properties.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = draw.width;
  desc.Height = draw.height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear = {};
  clear.Format = desc.Format;
  std::memcpy(clear.Color, draw.clear_color, sizeof(clear.Color));
  HRESULT result = c->device->CreateCommittedResource(
      &properties, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, __uuidof(ID3D12Resource),
      reinterpret_cast<void **>(out.color.put()));
  if (FAILED(result))
    return failure(c, result);
  if (command.readback) {
    out.readback = std::make_shared<ImageReadback>();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 length = 0;
    c->device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr,
                                     nullptr, &length);
    out.readback->pitch = footprint.Footprint.RowPitch;
    out.readback->width = draw.width;
    out.readback->height = draw.height;
    out.readback->length = length;
    result = allocation(c, D3D12_HEAP_TYPE_READBACK, length,
                        D3D12_RESOURCE_STATE_COPY_DEST, out.readback->gpu);
    if (FAILED(result))
      return failure(c, result);
  }
  desc.Format = DXGI_FORMAT_D32_FLOAT;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  clear = {};
  clear.Format = desc.Format;
  clear.DepthStencil.Depth = draw.clear_depth;
  result = c->device->CreateCommittedResource(
      &properties, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, __uuidof(ID3D12Resource),
      reinterpret_cast<void **>(out.depth.put()));
  return FAILED(result) ? failure(c, result) : MAG_NATIVE_OK;
}
static void transition(ID3D12GraphicsCommandList *list,
                       ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after) {
  if (before == after)
    return;
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  list->ResourceBarrier(1, &barrier);
}
static D3D12_CPU_DESCRIPTOR_HANDLE cpu(Context *c, Slot &slot,
                                       uint32_t offset) {
  auto result = slot.descriptors->GetCPUDescriptorHandleForHeapStart();
  result.ptr += SIZE_T(offset) * c->descriptor_step;
  return result;
}
static D3D12_GPU_DESCRIPTOR_HANDLE gpu(Context *c, Slot &slot,
                                       uint32_t offset) {
  auto result = slot.descriptors->GetGPUDescriptorHandleForHeapStart();
  result.ptr += UINT64(offset) * c->descriptor_step;
  return result;
}
static void descriptor(Context *c, Slot &slot, uint32_t offset,
                       const Bound &bound,
                       const mag_native_binding_layout &layout) {
  if (layout.binding_kind == MAG_NATIVE_UNIFORM) {
    D3D12_CONSTANT_BUFFER_VIEW_DESC view = {
        bound.buffer->gpu->GetGPUVirtualAddress() + bound.offset,
        static_cast<UINT>(padded(bound.length))};
    c->device->CreateConstantBufferView(&view, cpu(c, slot, offset));
  } else if (layout.access == MAG_NATIVE_READ_WRITE) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    view.Buffer.FirstElement = bound.offset / 4;
    view.Buffer.NumElements = static_cast<UINT>(bound.length / 4);
    view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    c->device->CreateUnorderedAccessView(bound.buffer->gpu.p, nullptr, &view,
                                         cpu(c, slot, offset));
  } else {
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.FirstElement = bound.offset / 4;
    view.Buffer.NumElements = static_cast<UINT>(bound.length / 4);
    view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    c->device->CreateShaderResourceView(bound.buffer->gpu.p, &view,
                                        cpu(c, slot, offset));
  }
}
static bool conflict(Context *c, const std::shared_ptr<Buffer> &b, bool write) {
  for (const auto &s : c->slots)
    if (s.occupied)
      for (uint32_t i = 0; i < s.resources; ++i)
        if (s.buffers[i] == b && (write || s.writes[i]))
          return true;
  return write && c->read.occupied && c->read.buffer == b;
}
static int execute(Context *c, Plan &plan, mag_native_ticket *out) {
  if (c->failed)
    return MAG_NATIVE_DEVICE_FAILED;
  Slot *slot = nullptr;
  for (auto &candidate : c->slots)
    if (!candidate.occupied) {
      slot = &candidate;
      break;
    }
  if (!slot)
    return MAG_NATIVE_BUSY;
  if (c->next_ticket == UINT64_MAX || c->next_fence >= UINT64_MAX - 1)
    return MAG_NATIVE_DEVICE_FAILED;
  std::array<bool, MaxResources> writes = {};
  for (uint32_t n = 0; n < plan.commands; ++n) {
    const auto &command = plan.nodes[n];
    if (command.draw) {
      if (command.readback)
        writes[resource_index(plan, command.readback)] = true;
    } else
      for (uint32_t i = 0; i < command.count; ++i)
        if (command.pipeline->bindings[command.bindings[i].layout].access ==
            MAG_NATIVE_READ_WRITE)
          writes[resource_index(plan, command.bindings[i].buffer)] = true;
  }
  for (uint32_t i = 0; i < plan.resources; ++i)
    if (plan.typed ? uses(c, plan.buffers[i])
                   : conflict(c, plan.buffers[i], writes[i]))
      return MAG_NATIVE_BUSY;
  try {
    clear_slot(*slot);
    uint32_t draw = 0;
    for (uint32_t n = 0; n < plan.commands; ++n)
      if (plan.nodes[n].draw) {
        int status = attachment(c, plan.nodes[n], slot->targets[draw]);
        if (status) {
          clear_slot(*slot);
          return status;
        }
        ++draw;
      }
    slot->attachments = draw;
    // No mutation of queue/resource state until the complete CPU validation and
    // allocation succeeds.
    Lease lease;
    int status = c->owner.try_begin(c->owner.userdata, c->owner.lifetime);
    if (status) {
      clear_slot(*slot);
      return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
    }
    lease.context = c;
    HRESULT result = slot->allocator->Reset();
    if (SUCCEEDED(result))
      result = slot->list->Reset(slot->allocator.p, nullptr);
    if (FAILED(result)) {
      clear_slot(*slot);
      return failure(c, result);
    }
    ID3D12GraphicsCommandList *list = slot->list.p;
    ID3D12DescriptorHeap *heaps[] = {slot->descriptors.p};
    list->SetDescriptorHeaps(1, heaps);
    std::array<D3D12_RESOURCE_STATES, MaxResources> states = {};
    for (uint32_t i = 0; i < plan.resources; ++i) {
      auto &b = plan.buffers[i];
      slot->buffers[i] = b;
      slot->uploads[i] = b->upload;
      slot->writes[i] = writes[i];
      states[i] = b->state;
      if (b->upload && (!plan.typed || plan.roles[i] != MAG_NATIVE_READBACK))
        list->CopyBufferRegion(b->gpu.p, 0, b->upload->gpu.p, 0, b->allocation);
    }
    slot->resources = plan.resources;
    slot->pipelines = plan.commands;
    uint32_t descriptors = 0, target = 0;
    for (uint32_t n = 0; n < plan.commands; ++n) {
      auto &command = plan.nodes[n];
      slot->programs[n] = command.pipeline;
      if (!command.draw) {
        std::array<D3D12_RESOURCE_STATES, MaxResources> desired = {};
        std::array<bool, MaxResources> used = {};
        for (uint32_t i = 0; i < command.count; ++i) {
          const auto &bound = command.bindings[i];
          int resource = resource_index(plan, bound.buffer);
          const auto &layout = command.pipeline->bindings[bound.layout];
          D3D12_RESOURCE_STATES state =
              layout.binding_kind == MAG_NATIVE_UNIFORM
                  ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
              : layout.access == MAG_NATIVE_READ_WRITE
                  ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                  : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
          desired[resource] =
              static_cast<D3D12_RESOURCE_STATES>(desired[resource] | state);
          used[resource] = true;
        }
        for (uint32_t i = 0; i < plan.resources; ++i)
          if (used[i]) {
            transition(list, plan.buffers[i]->gpu.p, states[i], desired[i]);
            states[i] = desired[i];
          }
        list->SetComputeRootSignature(command.pipeline->root.p);
        list->SetPipelineState(command.pipeline->gpu.p);
        for (uint32_t i = 0; i < command.count; ++i) {
          const auto &bound = command.bindings[i];
          descriptor(c, *slot, descriptors, bound,
                     command.pipeline->bindings[bound.layout]);
          list->SetComputeRootDescriptorTable(bound.layout,
                                              gpu(c, *slot, descriptors));
          ++descriptors;
        }
        list->Dispatch(command.groups[0], command.groups[1], command.groups[2]);
        // Establish visibility for a following compute node or vertex/index
        // consumption.
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->ResourceBarrier(1, &barrier);
      } else {
        const auto &d = command.draw_desc;
        Attachment &a = slot->targets[target];
        const int vertex = resource_index(plan, command.vertices),
                  index = resource_index(plan, command.indices);
        transition(list, command.vertices->gpu.p, states[vertex],
                   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        states[vertex] = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        transition(list, command.indices->gpu.p, states[index],
                   D3D12_RESOURCE_STATE_INDEX_BUFFER);
        states[index] = D3D12_RESOURCE_STATE_INDEX_BUFFER;
        auto rtv = slot->rtv->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(target) * c->rtv_step;
        auto dsv = slot->dsv->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += SIZE_T(target) * c->dsv_step;
        c->device->CreateRenderTargetView(a.color.p, nullptr, rtv);
        c->device->CreateDepthStencilView(a.depth.p, nullptr, dsv);
        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        list->ClearRenderTargetView(rtv, d.clear_color, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, d.clear_depth,
                                    0, 0, nullptr);
        D3D12_VIEWPORT viewport = {
            0, 0, static_cast<float>(d.width), static_cast<float>(d.height),
            0, 1};
        D3D12_RECT scissor = {0, 0, static_cast<LONG>(d.width),
                              static_cast<LONG>(d.height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRootSignature(command.pipeline->root.p);
        list->SetPipelineState(command.pipeline->gpu.p);
        D3D12_VERTEX_BUFFER_VIEW vertices = {
            command.vertices->gpu->GetGPUVirtualAddress() + d.vertex_offset,
            static_cast<UINT>(d.vertex_length), 16};
        D3D12_INDEX_BUFFER_VIEW indices = {
            command.indices->gpu->GetGPUVirtualAddress() + d.index_offset,
            static_cast<UINT>(d.index_length), DXGI_FORMAT_R32_UINT};
        list->IASetVertexBuffers(0, 1, &vertices);
        list->IASetIndexBuffer(&indices);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawIndexedInstanced(d.index_count, 1, 0, 0, 0);
        if (a.readback) {
          transition(list, a.color.p, D3D12_RESOURCE_STATE_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_COPY_SOURCE);
          D3D12_TEXTURE_COPY_LOCATION source = {};
          source.pResource = a.color.p;
          source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          D3D12_TEXTURE_COPY_LOCATION destination = {};
          destination.pResource = a.readback->gpu.p;
          destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          destination.PlacedFootprint.Footprint = {
              DXGI_FORMAT_R8G8B8A8_UNORM, d.width, d.height, 1,
              static_cast<UINT>(a.readback->pitch)};
          list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        ++target;
      }
    }
    result = list->Close();
    if (FAILED(result)) {
      clear_slot(*slot);
      return failure(c, result);
    }
    // From Execute onwards every failure is uncertain. Reserve all pins/ticket
    // before this void API.
    slot->ticket = {c->generation, ++c->next_ticket};
    slot->fence = ++c->next_fence;
    slot->occupied = true;
    ID3D12CommandList *commands[] = {list};
    c->queue->ExecuteCommandLists(1, commands);
    result = c->queue->Signal(c->fence.p, slot->fence);
    for (uint32_t i = 0; i < plan.resources; ++i) {
      auto &b = plan.buffers[i];
      b->state = states[i];
      if (!plan.typed || plan.roles[i] != MAG_NATIVE_READBACK)
        b->upload.reset();
      if (writes[i])
        b->gpu_dirty = true;
    }
    target = 0;
    for (uint32_t n = 0; n < plan.commands; ++n)
      if (plan.nodes[n].draw) {
        if (plan.nodes[n].readback) {
          plan.nodes[n].readback->image = slot->targets[target].readback;
          plan.nodes[n].readback->upload.reset();
        }
        ++target;
      }
    *out = slot->ticket;
    if (FAILED(result) || FAILED(c->device->GetDeviceRemovedReason())) {
      c->failed = true;
      return MAG_NATIVE_INDETERMINATE;
    }
    return MAG_NATIVE_OK;
  } catch (const std::bad_alloc &) {
    // Future changes must never turn an allocation exception after Execute
    // into a definitely-unsubmitted result or release live resource pins.
    if (slot->occupied) {
      c->failed = true;
      *out = slot->ticket;
      return MAG_NATIVE_INDETERMINATE;
    }
    clear_slot(*slot);
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
}
static int submit(void *opaque, const mag_native_frame *frame,
                  mag_native_ticket *out) {
  auto *c = static_cast<Context *>(opaque);
  try {
    Plan plan;
    plan.commands = 1;
    int status = prepare_frame(c, frame, plan.nodes[0]);
    if (status)
      return status;
    for (uint32_t i = 0; i < plan.nodes[0].count; ++i) {
      auto b = plan.nodes[0].bindings[i].buffer;
      if (resource_index(plan, b) < 0)
        plan.buffers[plan.resources++] = b;
    }
    return execute(c, plan, out);
  } catch (const std::bad_alloc &) {
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
}
static int submit_batch(void *opaque, const mag_native_batch *batch,
                        mag_native_ticket *out) {
  auto *c = static_cast<Context *>(opaque);
  try {
    Plan plan;
    int status = prepare_batch(c, batch, plan);
    return status ? status : execute(c, plan, out);
  } catch (const std::bad_alloc &) {
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
}
static int poll(void *opaque, mag_native_event *events, uint32_t capacity,
                uint32_t *count) {
  auto *c = static_cast<Context *>(opaque);
  *count = 0;
  const UINT64 completed = c->fence->GetCompletedValue();
  if (completed == UINT64_MAX) {
    c->failed = true;
    return MAG_NATIVE_DEVICE_FAILED;
  }
  if (FAILED(c->device->GetDeviceRemovedReason()))
    c->failed = true;
  for (auto &slot : c->slots)
    if (slot.occupied && slot.fence <= completed && *count < capacity) {
      events[(*count)++] = {sizeof(*events), 1, slot.ticket, 1, 1, 0, 0};
      clear_slot(slot);
    }
  // Already genuine once-only receipts are delivered even if a later error
  // exists.
  return *count      ? MAG_NATIVE_OK
         : c->failed ? MAG_NATIVE_DEVICE_FAILED
                     : MAG_NATIVE_OK;
}
static int close(void *opaque) {
  auto *c = static_cast<Context *>(opaque);
  for (const auto &slot : c->slots)
    if (slot.occupied)
      return MAG_NATIVE_BUSY;
  if (c->read.occupied)
    return MAG_NATIVE_BUSY;
  destroy(c);
  return MAG_NATIVE_OK;
}
static void clear_read(Context *c) {
  if (c->read.event)
    CloseHandle(c->read.event);
  c->read = Read{};
}
static int start_read(Context *c, const std::shared_ptr<Buffer> &b,
                      uint64_t offset, uint64_t length) {
  if (c->failed)
    return MAG_NATIVE_DEVICE_FAILED;
  if (c->next_fence >= UINT64_MAX - 1)
    return MAG_NATIVE_DEVICE_FAILED;
  Read &read = c->read;
  read.buffer = b;
  read.offset = offset;
  read.length = length;
  read.upload = b->upload;
  HRESULT result = allocation(c, D3D12_HEAP_TYPE_READBACK, padded(length),
                              D3D12_RESOURCE_STATE_COPY_DEST, read.staging);
  if (FAILED(result)) {
    clear_read(c);
    return failure(c, result);
  }
  result = c->device->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
      reinterpret_cast<void **>(read.allocator.put()));
  if (SUCCEEDED(result))
    result = c->device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, read.allocator.p, nullptr,
        __uuidof(ID3D12GraphicsCommandList),
        reinterpret_cast<void **>(read.list.put()));
  if (FAILED(result)) {
    clear_read(c);
    return failure(c, result);
  }
  read.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!read.event) {
    clear_read(c);
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
  Lease lease;
  int status = c->owner.try_begin(c->owner.userdata, c->owner.lifetime);
  if (status) {
    clear_read(c);
    return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
  }
  lease.context = c;
  if (read.upload)
    read.list->CopyBufferRegion(b->gpu.p, 0, read.upload->gpu.p, 0,
                                b->allocation);
  transition(read.list.p, b->gpu.p, b->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  read.list->CopyBufferRegion(read.staging.p, 0, b->gpu.p, offset, length);
  transition(read.list.p, b->gpu.p, D3D12_RESOURCE_STATE_COPY_SOURCE, b->state);
  result = read.list->Close();
  if (FAILED(result)) {
    clear_read(c);
    return failure(c, result);
  }
  read.fence = ++c->next_fence;
  read.occupied = true;
  ID3D12CommandList *lists[] = {read.list.p};
  c->queue->ExecuteCommandLists(1, lists);
  result = c->queue->Signal(c->fence.p, read.fence);
  b->upload.reset();
  if (FAILED(result) || FAILED(c->device->GetDeviceRemovedReason())) {
    c->failed = true;
    return MAG_NATIVE_BUSY;
  }
  return MAG_NATIVE_OK;
}
static int buffer_read(void *opaque, mag_native_buffer ref, uint64_t offset,
                       void *destination, uint64_t length) {
  auto *c = static_cast<Context *>(opaque);
  auto b = buffer(c, ref);
  if (!b)
    return MAG_NATIVE_STALE;
  if (!range(b->length, offset, length))
    return MAG_NATIVE_INVALID;
  for (const auto &slot : c->slots)
    if (slot.occupied)
      for (uint32_t i = 0; i < slot.resources; ++i)
        if (slot.buffers[i] == b)
          return MAG_NATIVE_BUSY;
  if (b->image) {
    const auto &image = *b->image;
    const uint64_t row = uint64_t(image.width) * 4;
    if (!range(row * image.height, offset, length))
      return MAG_NATIVE_INVALID;
    void *mapped = nullptr;
    D3D12_RANGE read = {0, static_cast<SIZE_T>(image.length)};
    HRESULT result = image.gpu->Map(0, &read, &mapped);
    if (FAILED(result) || !mapped)
      return failure(c, FAILED(result) ? result : E_FAIL);
    uint64_t copied = 0;
    while (copied < length) {
      uint64_t at = offset + copied, column = at % row, take = row - column;
      if (take > length - copied)
        take = length - copied;
      std::memcpy(static_cast<unsigned char *>(destination) + copied,
                  static_cast<unsigned char *>(mapped) +
                      (at / row) * image.pitch + column,
                  static_cast<size_t>(take));
      copied += take;
    }
    D3D12_RANGE none = {0, 0};
    image.gpu->Unmap(0, &none);
    return MAG_NATIVE_OK;
  }
  if (c->read.occupied) {
    if (c->read.buffer != b || c->read.offset != offset ||
        c->read.length != length)
      return MAG_NATIVE_BUSY;
  } else {
    int status = start_read(c, b, offset, length);
    if (status)
      return status;
  }
  // Explicit diagnostic API only. No normal submit/draw path waits or maps a
  // GPU result.
  UINT64 completed = c->fence->GetCompletedValue();
  if (completed == UINT64_MAX) {
    c->failed = true;
    return MAG_NATIVE_DEVICE_FAILED;
  }
  if (completed < c->read.fence) {
    HRESULT result =
        c->fence->SetEventOnCompletion(c->read.fence, c->read.event);
    if (FAILED(result))
      return failure(c, result);
    (void)WaitForSingleObject(c->read.event, 2000);
    completed = c->fence->GetCompletedValue();
  }
  if (completed == UINT64_MAX) {
    c->failed = true;
    return MAG_NATIVE_DEVICE_FAILED;
  }
  if (completed < c->read.fence)
    return MAG_NATIVE_BUSY; // timeout never certifies completion
  void *mapped = nullptr;
  D3D12_RANGE bytes = {0, static_cast<SIZE_T>(length)};
  HRESULT result = c->read.staging->Map(0, &bytes, &mapped);
  if (FAILED(result) || !mapped) {
    clear_read(c);
    return failure(c, FAILED(result) ? result : E_FAIL);
  }
  std::memcpy(destination, mapped, static_cast<size_t>(length));
  D3D12_RANGE none = {0, 0};
  c->read.staging->Unmap(0, &none);
  clear_read(c);
  return MAG_NATIVE_OK;
}
static const mag_native_ops operations = {caps,
                                          buffer_create,
                                          buffer_release,
                                          buffer_read,
                                          pipeline_create,
                                          pipeline_release,
                                          submit,
                                          poll,
                                          close,
                                          graphics_create,
                                          submit_batch,
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          nullptr};
static int compiler_path(std::string &out) {
  DWORD count = GetEnvironmentVariableW(L"PS3RECOMP_DXC_LIBRARY", nullptr, 0);
  if (!count)
    return MAG_NATIVE_UNSUPPORTED;
  if (count > 32768)
    return MAG_NATIVE_INVALID;
  std::wstring wide(count, L'\0');
  DWORD actual =
      GetEnvironmentVariableW(L"PS3RECOMP_DXC_LIBRARY", wide.data(), count);
  if (!actual || actual >= count)
    return MAG_NATIVE_INVALID;
  wide.resize(actual);
  int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                  static_cast<int>(wide.size()), nullptr, 0,
                                  nullptr, nullptr);
  if (bytes <= 0)
    return MAG_NATIVE_INVALID;
  out.resize(static_cast<size_t>(bytes));
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                           static_cast<int>(wide.size()), out.data(), bytes,
                           nullptr, nullptr))
    return MAG_NATIVE_INVALID;
  const bool drive =
      out.size() > 2 &&
      ((out[0] >= 'A' && out[0] <= 'Z') || (out[0] >= 'a' && out[0] <= 'z')) &&
      out[1] == ':' && (out[2] == '/' || out[2] == '\\');
  const bool unc = out.size() > 2 && out[0] == '\\' && out[1] == '\\';
  return drive || unc ? MAG_NATIVE_OK : MAG_NATIVE_INVALID;
}
static bool queue_matches_device(Context *c) {
  // Keep temporary COM references inside this scope. They must be gone before
  // any constructor failure releases the provider's retained owner lifetime.
  Ref<ID3D12Device> queue_device;
  HRESULT result = c->queue->GetDevice(
      __uuidof(ID3D12Device), reinterpret_cast<void **>(queue_device.put()));
  Ref<IUnknown> device_identity, queue_identity;
  if (SUCCEEDED(result))
    result = c->device->QueryInterface(
        __uuidof(IUnknown), reinterpret_cast<void **>(device_identity.put()));
  if (SUCCEEDED(result))
    result = queue_device->QueryInterface(
        __uuidof(IUnknown), reinterpret_cast<void **>(queue_identity.put()));
  return SUCCEEDED(result) && device_identity.p == queue_identity.p;
}
} // namespace
extern "C" int mag_native_d3d12_create(const MagRenderHostOwner *owner,
                                       mag_native_device **out) {
  if (out)
    *out = nullptr;
  if (!owner || !out || owner->abi_version != 1 ||
      owner->struct_size != sizeof(*owner) ||
      owner->backend != MAG_RENDER_HOST_D3D12 || owner->reserved ||
      owner->reserved1 || !owner->lifetime || !owner->device || !owner->queue ||
      !owner->retain || !owner->release || !owner->try_begin || !owner->end ||
      owner->instance || owner->physical_device || owner->get_instance_proc ||
      owner->get_device_proc || owner->queue_family)
    return MAG_NATIVE_INVALID;
  int status = owner->retain(owner->userdata, owner->lifetime);
  if (status)
    return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
  Context *c = nullptr;
  try {
    c = new Context;
    c->owner = *owner;
    c->pinned = true;
    status = compiler_path(c->compiler);
    if (status) {
      destroy(c);
      return status;
    }
    // All native object dereferences follow successful host retention.
    c->device.p = static_cast<ID3D12Device *>(owner->device);
    c->device->AddRef();
    c->queue.p = static_cast<ID3D12CommandQueue *>(owner->queue);
    c->queue->AddRef();
    if (c->queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        c->queue->GetDesc().NodeMask > 1) {
      destroy(c);
      return MAG_NATIVE_UNSUPPORTED;
    }
    if (!queue_matches_device(c)) {
      destroy(c);
      return MAG_NATIVE_INVALID;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL model = {D3D_SHADER_MODEL_6_0};
    if (FAILED(c->device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
                                              &model, sizeof(model))) ||
        model.HighestShaderModel < D3D_SHADER_MODEL_6_0) {
      destroy(c);
      return MAG_NATIVE_UNSUPPORTED;
    }
    c->runtime =
        LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    FARPROC address =
        c->runtime ? GetProcAddress(c->runtime, "D3D12SerializeRootSignature")
                   : nullptr;
    static_assert(sizeof(address) == sizeof(c->serialize),
                  "Windows function pointer layout");
    std::memcpy(&c->serialize, &address, sizeof(address));
    if (!c->serialize) {
      destroy(c);
      return MAG_NATIVE_UNSUPPORTED;
    }
    c->generation = mag_native_next_generation();
    if (!c->generation) {
      destroy(c);
      return MAG_NATIVE_ALLOCATION_FAILED;
    }
    HRESULT result =
        c->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                               reinterpret_cast<void **>(c->fence.put()));
    if (FAILED(result)) {
      status = failure(c, result);
      destroy(c);
      return status;
    }
    c->descriptor_step = c->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    c->rtv_step = c->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    c->dsv_step = c->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (auto &slot : c->slots) {
      result = c->device->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
          reinterpret_cast<void **>(slot.allocator.put()));
      if (SUCCEEDED(result))
        result = c->device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.p, nullptr,
            __uuidof(ID3D12GraphicsCommandList),
            reinterpret_cast<void **>(slot.list.put()));
      if (SUCCEEDED(result))
        result = slot.list->Close();
      if (FAILED(result)) {
        status = failure(c, result);
        destroy(c);
        return status;
      }
      status = heap(c, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, HeapDescriptors,
                    true, slot.descriptors);
      if (!status)
        status = heap(c, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, MaxCommands, false,
                      slot.rtv);
      if (!status)
        status = heap(c, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, MaxCommands, false,
                      slot.dsv);
      if (status) {
        destroy(c);
        return status;
      }
    }
    *out = mag_native_wrap(&operations, c);
    if (!*out) {
      destroy(c);
      return MAG_NATIVE_ALLOCATION_FAILED;
    }
    return MAG_NATIVE_OK;
  } catch (const std::bad_alloc &) {
    if (c)
      destroy(c);
    else
      owner->release(owner->userdata, owner->lifetime);
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
}
