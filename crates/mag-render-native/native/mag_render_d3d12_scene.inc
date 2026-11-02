/* Frontend-owned D3D12 multipass scene; shares the actual Context/Slot fence
 * ownership with compute/batch. No device/queue/window or SDK is created. */
static DXGI_FORMAT scene_format(uint32_t format) {
  return format == MAG_NATIVE_SCENE_D32     ? DXGI_FORMAT_D32_FLOAT
         : format == MAG_NATIVE_SCENE_BGRA8 ? DXGI_FORMAT_B8G8R8A8_UNORM
                                            : DXGI_FORMAT_R8G8B8A8_UNORM;
}
struct SceneBlob {
  mag_render_dxc_blob value = {};
  ~SceneBlob() { mag_render_dxc_free(&value); }
};
static int scene_graphics_layout(Context *c,
                                 const mag_native_scene_graphics_desc *desc,
                                 const mag_native_vertex_attribute *attributes,
                                 uint32_t attribute_count, uint32_t model,
                                 mag_native_pipeline *out) {
  if (c->failed)
    return MAG_NATIVE_DEVICE_FAILED;
  try {
    auto p = std::make_shared<Pipeline>();
    p->graphics = true;
    p->scene = true;
    p->scene_model = model;
    p->scene_desc = *desc;
    if (desc->buffer_count)
      p->scene_buffers.assign(desc->buffers,
                              desc->buffers + desc->buffer_count);
    if (desc->texture_count)
      p->scene_textures.assign(desc->textures,
                               desc->textures + desc->texture_count);
    p->scene_desc.buffers = p->scene_buffers.data();
    p->scene_desc.textures = p->scene_textures.data();
    std::array<std::array<D3D12_DESCRIPTOR_RANGE, 33>, 2> resources = {};
    std::array<D3D12_DESCRIPTOR_RANGE, 34> sampler_ranges = {};
    std::array<D3D12_ROOT_PARAMETER, 36> parameters = {};
    uint32_t parameter_count = 0, sampler_range_count = 0;
    for (uint32_t stage = 0; stage < 2; ++stage) {
      const auto visibility = stage ? D3D12_SHADER_VISIBILITY_PIXEL
                                    : D3D12_SHADER_VISIBILITY_VERTEX;
      uint32_t count = 0, samplers = 0;
      for (uint32_t i = 0; i < desc->buffer_count; ++i) {
        const auto &b = desc->buffers[i];
        if (b.stage != stage)
          continue;
        auto &r = resources[stage][count];
        r.RangeType = b.binding.binding_kind == MAG_NATIVE_UNIFORM
                          ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                          : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r.NumDescriptors = 1;
        r.BaseShaderRegister =
            b.binding.slot +
            (model && b.binding.binding_kind == MAG_NATIVE_STORAGE ? 32u : 0u);
        r.RegisterSpace = stage;
        r.OffsetInDescriptorsFromTableStart = count;
        ++count;
      }
      for (uint32_t i = 0; i < desc->texture_count; ++i) {
        const auto &t = desc->textures[i];
        if (t.stage != stage)
          continue;
        auto &r = resources[stage][count];
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r.NumDescriptors = 1;
        r.BaseShaderRegister = t.texture_slot;
        r.RegisterSpace = stage;
        r.OffsetInDescriptorsFromTableStart = count;
        ++count;
        samplers |= 1u << t.sampler_slot;
      }
      p->scene_sampler_mask[stage] = samplers;
      p->scene_sampler_index[stage] = UINT32_MAX;
      if (!model && samplers) {
        // Naga's explicit helper contract: StructuredBuffer<uint> t31,
        // in each stage's WGSL register space. Contents select the
        // actual finite sampler heap entries; no guessed state.
        p->scene_sampler_index[stage] = count;
        auto &r = resources[stage][count];
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r.NumDescriptors = 1;
        r.BaseShaderRegister = 31;
        r.RegisterSpace = stage;
        r.OffsetInDescriptorsFromTableStart = count;
        ++count;
      }
      p->scene_resource_count[stage] = count;
      p->scene_resource_root[stage] = UINT32_MAX;
      if (count) {
        auto &root = parameters[parameter_count];
        root.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root.ShaderVisibility = visibility;
        root.DescriptorTable = {count, resources[stage].data()};
        p->scene_resource_root[stage] = parameter_count++;
      }
      p->scene_sampler_root[stage].fill(UINT32_MAX);
      if (model)
        for (uint32_t unit = 0; unit < 16; ++unit)
          if (samplers & (1u << unit)) {
            auto &range = sampler_ranges[sampler_range_count++];
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            range.NumDescriptors = 1;
            range.BaseShaderRegister = unit;
            range.RegisterSpace = stage;
            range.OffsetInDescriptorsFromTableStart = 0;
            auto &root = parameters[parameter_count];
            root.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            root.ShaderVisibility = visibility;
            root.DescriptorTable = {1, &range};
            p->scene_sampler_root[stage][unit] = parameter_count++;
          }
    }
    p->scene_heap_root = UINT32_MAX;
    if (!model && (p->scene_sampler_mask[0] || p->scene_sampler_mask[1])) {
      D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
      HRESULT capability = c->device->CheckFeatureSupport(
          D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
      if (FAILED(capability) ||
          options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
        return MAG_NATIVE_UNSUPPORTED;

      // The two typed Naga sampler arrays alias one actual2048-entry
      // sampler heap. Distinct register spaces2/3; heap offsets agree.
      auto &standard = sampler_ranges[sampler_range_count++];
      standard = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2048, 0, 2, 0};
      auto &comparison = sampler_ranges[sampler_range_count++];
      comparison = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2048, 0, 3, 0};
      auto &root = parameters[parameter_count];
      root.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      root.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      root.DescriptorTable = {2, &standard};
      p->scene_heap_root = parameter_count++;
    }
    D3D12_ROOT_SIGNATURE_DESC root = {};
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    root.NumParameters = parameter_count;
    root.pParameters = parameters.data();
    HRESULT result = root_signature(c, root, p->root);
    if (FAILED(result))
      return failure(c, result);
    SceneBlob vertex, fragment;
    int status = compile(c, desc->vertex, MAG_RENDER_DXC_VERTEX, vertex.value);
    if (status)
      return status;
    if (desc->fragment.source_length) {
      status =
          compile(c, desc->fragment, MAG_RENDER_DXC_FRAGMENT, fragment.value);
      if (status)
        return status;
    }
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_UNKNOWN,
                                   DXGI_FORMAT_R32_FLOAT,
                                   DXGI_FORMAT_R32G32_FLOAT,
                                   DXGI_FORMAT_R32G32B32_FLOAT,
                                   DXGI_FORMAT_R32G32B32A32_FLOAT,
                                   DXGI_FORMAT_R8G8B8A8_UNORM};
    std::array<D3D12_INPUT_ELEMENT_DESC, 16> input = {};
    for (uint32_t i = 0; i < attribute_count; ++i) {
      const auto &a = attributes[i];
      input[i] = {"LOC", a.location, formats[a.format],
                  0,     a.offset,   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                  0};
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gpu = {};
    gpu.pRootSignature = p->root.p;
    gpu.VS = {vertex.value.bytes, static_cast<SIZE_T>(vertex.value.length)};
    gpu.PS = {fragment.value.bytes, static_cast<SIZE_T>(fragment.value.length)};
    gpu.InputLayout = {input.data(), attribute_count};
    gpu.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gpu.NumRenderTargets = desc->color_format ? 1 : 0;
    if (desc->color_format)
      gpu.RTVFormats[0] = scene_format(desc->color_format);
    gpu.DSVFormat =
        desc->depth_format ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
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
    if (desc->blend == MAG_NATIVE_SCENE_ALPHA) {
      auto &blend = gpu.BlendState.RenderTarget[0];
      blend.BlendEnable = TRUE;
      blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
      blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
      blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    }
    const D3D12_COMPARISON_FUNC comparisons[] = {
        D3D12_COMPARISON_FUNC_ALWAYS, D3D12_COMPARISON_FUNC_LESS,
        D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS};
    gpu.DepthStencilState.DepthEnable =
        desc->depth_compare != MAG_NATIVE_SCENE_DEPTH_DISABLED;
    gpu.DepthStencilState.DepthWriteMask = desc->depth_write
                                               ? D3D12_DEPTH_WRITE_MASK_ALL
                                               : D3D12_DEPTH_WRITE_MASK_ZERO;
    gpu.DepthStencilState.DepthFunc = comparisons[desc->depth_compare];
    gpu.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    gpu.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    gpu.DepthStencilState.FrontFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_COMPARISON_FUNC_ALWAYS};
    gpu.DepthStencilState.BackFace = gpu.DepthStencilState.FrontFace;
    result = c->device->CreateGraphicsPipelineState(
        &gpu, __uuidof(ID3D12PipelineState),
        reinterpret_cast<void **>(p->gpu.put()));
    if (FAILED(result))
      return failure(c, result);
    if (c->next_pipeline == UINT64_MAX)
      return MAG_NATIVE_ALLOCATION_FAILED;
    p->scene_desc.vertex.source = nullptr;
    p->scene_desc.vertex.entry = nullptr;
    p->scene_desc.fragment.source = nullptr;
    p->scene_desc.fragment.entry = nullptr;
    p->id = ++c->next_pipeline;
    c->pipelines.emplace(p->id, p);
    *out = {c->generation, p->id};
    return MAG_NATIVE_OK;
  } catch (const std::bad_alloc &) {
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
}
static int scene_graphics_create(void *opaque,
                                 const mag_native_scene_graphics_desc *desc,
                                 mag_native_pipeline *out) {
  int status = mag_native_scene_graphics_validate(desc);
  if (status)
    return status;
  const mag_native_vertex_attribute attribute = {0, MAG_NATIVE_VERTEX_FLOAT4, 0,
                                                 0};
  return scene_graphics_layout(static_cast<Context *>(opaque), desc, &attribute,
                               1, 0, out);
}
static int
scene_graphics_create_v2(void *opaque,
                         const mag_native_scene_graphics_v2_desc *desc,
                         mag_native_pipeline *out) {
  int status = mag_native_scene_graphics_v2_validate(desc);
  if (status)
    return status;
  return scene_graphics_layout(static_cast<Context *>(opaque), &desc->base,
                               desc->attributes, desc->attribute_count,
                               desc->binding_model, out);
}
struct SceneDrawPlan {
  const mag_native_scene_draw *source = nullptr;
  std::shared_ptr<Pipeline> program;
  std::array<uint32_t, 2> descriptor_base = {};
  std::array<uint64_t, 2> sampler_indices = {{UINT64_MAX, UINT64_MAX}};
};
struct ScenePlan {
  Plan buffers;
  std::array<Command, MaxCommands> compute = {};
  std::vector<SceneDrawPlan> draws;
  mag_native_scene_usage usage = {};
  uint32_t descriptors = 0, sampler_records = 0;
  uint64_t bytes = 0;
};
static int scene_buffer_info(void *opaque, mag_native_buffer ref,
                             mag_native_scene_buffer_info *out) {
  auto *c = static_cast<Context *>(opaque);
  auto b = buffer(c, ref);
  if (!b)
    return MAG_NATIVE_STALE;
  *out = {b->length, uses(c, b) ? 1u : 0u, b->gpu_dirty ? 1u : 0u,
          b->seeded ? 0u : 1u};
  return MAG_NATIVE_OK;
}
static int scene_compute_info(void *opaque, mag_native_pipeline ref,
                              mag_native_scene_compute_info *out) {
  auto p = pipeline(static_cast<Context *>(opaque), ref);
  if (!p)
    return MAG_NATIVE_STALE;
  if (p->graphics)
    return MAG_NATIVE_INVALID;
  *out = {p->bindings.data(), p->count, {1, 1, 1}};
  return MAG_NATIVE_OK;
}
static int scene_graphics_info(void *opaque, mag_native_pipeline ref,
                               const mag_native_scene_graphics_desc **out,
                               uint32_t *model) {
  auto p = pipeline(static_cast<Context *>(opaque), ref);
  if (!p)
    return MAG_NATIVE_STALE;
  if (!p->graphics || !p->scene)
    return MAG_NATIVE_INVALID;
  *out = &p->scene_desc;
  *model = p->scene_model;
  return MAG_NATIVE_OK;
}
static int scene_prepare(Context *c, const mag_native_scene *scene,
                         ScenePlan &plan) {
  if (c->failed)
    return MAG_NATIVE_DEVICE_FAILED;
  mag_native_scene_registry registry = {c,
                                        scene_buffer_info,
                                        scene_compute_info,
                                        scene_graphics_info,
                                        256,
                                        16,
                                        65536,
                                        UINT32_MAX};
  int status = mag_native_scene_validate(scene, &registry, &plan.usage);
  if (status)
    return status;
  plan.buffers.resources = scene->resource_count;
  plan.buffers.typed = true;
  plan.bytes = plan.usage.bytes;
  for (uint32_t i = 0; i < scene->resource_count; ++i) {
    plan.buffers.buffers[i] = buffer(c, scene->resources[i].buffer);
    plan.buffers.roles[i] = scene->resources[i].role;
  }
  for (uint32_t i = 0; i < scene->dispatch_count; ++i) {
    status = prepare_frame(c, &scene->dispatches[i], plan.compute[i]);
    if (status)
      return status;
    plan.descriptors += plan.compute[i].count;
  }
  uint32_t draws = 0;
  for (uint32_t i = 0; i < scene->pass_count; ++i)
    draws += scene->passes[i].draw_count;
  plan.draws.reserve(draws);
  for (uint32_t i = 0; i < scene->pass_count; ++i)
    for (uint32_t j = 0; j < scene->passes[i].draw_count; ++j) {
      const auto &draw = scene->passes[i].draws[j];
      SceneDrawPlan record;
      record.source = &draw;
      record.program = pipeline(c, draw.pipeline);
      const auto &p = *record.program;
      for (uint32_t stage = 0; stage < 2; ++stage) {
        record.descriptor_base[stage] = plan.descriptors;
        plan.descriptors += p.scene_resource_count[stage];
        if (p.scene_sampler_index[stage] != UINT32_MAX) {
          record.sampler_indices[stage] = uint64_t(plan.sampler_records++) * 64;
        }
      }
      for (uint32_t k = 0; k < draw.buffer_count; ++k) {
        const auto &b = draw.buffers[k];
        auto resource = buffer(c, b.binding.buffer);
        for (const auto &layout : p.scene_buffers)
          if (layout.stage == b.stage &&
              layout.binding.slot == b.binding.slot) {
            if (layout.binding.binding_kind == MAG_NATIVE_UNIFORM &&
                (b.binding.offset > resource->allocation ||
                 padded(b.binding.byte_length) >
                     resource->allocation - b.binding.offset))
              return MAG_NATIVE_INVALID;
          }
      }
      plan.draws.push_back(std::move(record));
    }
  // Fixed hardware limit, never truncate an admitted complete scene.
  if (plan.descriptors > 1000000u)
    return MAG_NATIVE_UNSUPPORTED;
  auto reserve = [&](uint64_t bytes) {
    if (bytes > MAG_NATIVE_BATCH_MAX_BYTES - plan.bytes)
      return false;
    plan.bytes += bytes;
    return true;
  };
  if (!reserve(uint64_t(plan.sampler_records) * 64))
    return MAG_NATIVE_ALLOCATION_FAILED;
  for (uint32_t i = 0; i < scene->image_count; ++i)
    if (scene->images[i].upload.id) {
      const auto &im = scene->images[i];
      if (!reserve(padded(uint64_t(im.width) * 4) * im.height))
        return MAG_NATIVE_ALLOCATION_FAILED;
    }
  return MAG_NATIVE_OK;
}
static int scene_image(Context *c, const mag_native_scene_image &im,
                       SceneImage &out) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = im.width;
  desc.Height = im.height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  const bool depth = im.format == MAG_NATIVE_SCENE_D32;
  out.format = scene_format(im.format);
  desc.Format = depth ? DXGI_FORMAT_R32_TYPELESS : out.format;
  desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                     : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {
      out.format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
  HRESULT result = c->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                  &support, sizeof(support));
  const auto required = depth ? D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL
                              : D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
  if (FAILED(result) || (support.Support1 & required) != required)
    return MAG_NATIVE_UNSUPPORTED;
  out.state = im.upload.id ? D3D12_RESOURCE_STATE_COPY_DEST
              : depth      ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                           : D3D12_RESOURCE_STATE_RENDER_TARGET;
  result = c->device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, out.state, nullptr,
      __uuidof(ID3D12Resource), reinterpret_cast<void **>(out.gpu.put()));
  if (FAILED(result))
    return failure(c, result);
  if (im.upload.id) {
    out.pitch = padded(uint64_t(im.width) * 4);
    result = allocation(c, D3D12_HEAP_TYPE_DEFAULT, out.pitch * im.height,
                        D3D12_RESOURCE_STATE_COPY_DEST, out.upload);
    if (FAILED(result))
      return failure(c, result);
  }
  return MAG_NATIVE_OK;
}
static int scene_sampler_heap(Context *c, Slot &slot) {
  if (slot.samplers)
    return MAG_NATIVE_OK;
  int status =
      heap(c, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048, true, slot.samplers);
  if (status)
    return status;
  auto handle = slot.samplers->GetCPUDescriptorHandleForHeapStart();
  D3D12_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW =
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sampler.MinLOD = sampler.MaxLOD = 0;
  for (uint32_t i = 0; i < 2048; ++i) {
    auto at = handle;
    at.ptr += SIZE_T(i) * c->sampler_step;
    sampler.Filter = i == MAG_NATIVE_SCENE_LINEAR
                         ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT
                     : i == MAG_NATIVE_SCENE_COMPARE_LEQUAL
                         ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT
                         : D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.ComparisonFunc = i == MAG_NATIVE_SCENE_COMPARE_LEQUAL
                                 ? D3D12_COMPARISON_FUNC_LESS_EQUAL
                                 : D3D12_COMPARISON_FUNC_ALWAYS;
    c->device->CreateSampler(&sampler, at);
  }
  return MAG_NATIVE_OK;
}
static const mag_native_scene_buffer_binding *
scene_buffer_binding(const mag_native_scene_draw &draw, uint32_t stage,
                     uint32_t unit) {
  for (uint32_t i = 0; i < draw.buffer_count; ++i)
    if (draw.buffers[i].stage == stage && draw.buffers[i].binding.slot == unit)
      return &draw.buffers[i];
  return nullptr;
}
static const mag_native_scene_texture_binding *
scene_texture_binding(const mag_native_scene_draw &draw,
                      const mag_native_scene_texture_layout &layout) {
  for (uint32_t i = 0; i < draw.texture_count; ++i) {
    const auto &b = draw.textures[i];
    if (b.stage == layout.stage && b.texture_slot == layout.texture_slot &&
        b.sampler_slot == layout.sampler_slot)
      return &b;
  }
  return nullptr;
}
static uint32_t scene_sampler(const mag_native_scene_draw &draw, uint32_t stage,
                              uint32_t unit) {
  for (uint32_t i = 0; i < draw.texture_count; ++i)
    if (draw.textures[i].stage == stage &&
        draw.textures[i].sampler_slot == unit)
      return draw.textures[i].sampler;
  return UINT32_MAX;
}
static void scene_descriptors(Context *c, Slot &slot,
                              const SceneDrawPlan &record) {
  const auto &draw = *record.source;
  const auto &p = *record.program;
  for (uint32_t stage = 0; stage < 2; ++stage) {
    uint32_t at = record.descriptor_base[stage];
    for (const auto &layout : p.scene_buffers)
      if (layout.stage == stage) {
        const auto &b =
            scene_buffer_binding(draw, stage, layout.binding.slot)->binding;
        Bound bound;
        bound.buffer = buffer(c, b.buffer);
        bound.offset = b.offset;
        bound.length = b.byte_length;
        descriptor(c, slot, at++, bound, layout.binding);
      }
    for (const auto &layout : p.scene_textures)
      if (layout.stage == stage) {
        const auto &t = *scene_texture_binding(draw, layout);
        const auto &im = slot.scene_images[t.image];
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = layout.format == MAG_NATIVE_SCENE_D32
                          ? DXGI_FORMAT_R32_FLOAT
                          : scene_format(layout.format);
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        c->device->CreateShaderResourceView(im.gpu.p, &view,
                                            cpu(c, slot, at++));
      }
    if (record.sampler_indices[stage] != UINT64_MAX) {
      D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
      view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      view.Format = DXGI_FORMAT_UNKNOWN;
      view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      view.Buffer.FirstElement = record.sampler_indices[stage] / 4;
      view.Buffer.NumElements = 16;
      view.Buffer.StructureByteStride = 4;
      c->device->CreateShaderResourceView(slot.sampler_indices.p, &view,
                                          cpu(c, slot, at++));
    }
  }
}
static int scene_reserve(Context *c, Slot &slot, const mag_native_scene *scene,
                         const ScenePlan &plan) {
  if (plan.descriptors > slot.descriptor_capacity) {
    Ref<ID3D12DescriptorHeap> replacement;
    int status = heap(c, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                      plan.descriptors, true, replacement);
    if (status)
      return status;
    slot.descriptors = std::move(replacement);
    slot.descriptor_capacity = plan.descriptors;
  }
  int status = scene_sampler_heap(c, slot);
  if (status)
    return status;
  slot.scene_images.resize(scene->image_count);
  for (uint32_t i = 0; i < scene->image_count; ++i) {
    status = scene_image(c, scene->images[i], slot.scene_images[i]);
    if (status)
      return status;
  }
  if (scene->diagnostic_readback.id) {
    const auto &im = scene->images[scene->output_image];
    slot.scene_readback = std::make_shared<ImageReadback>();
    auto &r = *slot.scene_readback;
    r.width = im.width;
    r.height = im.height;
    r.pitch = padded(uint64_t(im.width) * 4);
    r.length = r.pitch * im.height;
    HRESULT result = allocation(c, D3D12_HEAP_TYPE_READBACK, r.length,
                                D3D12_RESOURCE_STATE_COPY_DEST, r.gpu);
    if (FAILED(result))
      return failure(c, result);
  }
  if (plan.sampler_records) {
    HRESULT result = allocation(
        c, D3D12_HEAP_TYPE_UPLOAD, uint64_t(plan.sampler_records) * 64,
        D3D12_RESOURCE_STATE_GENERIC_READ, slot.sampler_indices);
    if (FAILED(result))
      return failure(c, result);
    void *mapped = nullptr;
    D3D12_RANGE none = {0, 0};
    result = slot.sampler_indices->Map(0, &none, &mapped);
    if (FAILED(result) || !mapped)
      return failure(c, FAILED(result) ? result : E_FAIL);
    auto *bytes = static_cast<uint8_t *>(mapped);
    std::memset(bytes, 0, size_t(plan.sampler_records) * 64);
    for (const auto &record : plan.draws)
      for (uint32_t stage = 0; stage < 2; ++stage)
        if (record.sampler_indices[stage] != UINT64_MAX) {
          auto *indices = reinterpret_cast<uint32_t *>(
              bytes + record.sampler_indices[stage]);
          for (uint32_t unit = 0; unit < 16; ++unit)
            if (record.program->scene_sampler_mask[stage] & (1u << unit))
              indices[unit] = scene_sampler(*record.source, stage, unit);
        }
    slot.sampler_indices->Unmap(0, nullptr);
  }
  for (const auto &record : plan.draws)
    scene_descriptors(c, slot, record);
  // Allocate every retained program reference before Execute; no host heap
  // growth is allowed after queue acceptance becomes indeterminate.
  slot.scene_programs.reserve(plan.draws.size() + scene->dispatch_count);
  for (uint32_t i = 0; i < scene->dispatch_count; ++i)
    slot.scene_programs.push_back(plan.compute[i].pipeline);
  for (const auto &record : plan.draws)
    slot.scene_programs.push_back(record.program);
  return MAG_NATIVE_OK;
}
static int scene_execute(Context *c, const mag_native_scene *scene,
                         ScenePlan &plan, mag_native_ticket *out) {
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
  try {
    clear_slot(*slot);
    int status = scene_reserve(c, *slot, scene, plan);
    if (status) {
      clear_slot(*slot);
      return status;
    }
    Lease lease;
    status = c->owner.try_begin(c->owner.userdata, c->owner.lifetime);
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
    auto *list = slot->list.p;
    ID3D12DescriptorHeap *heaps[] = {slot->descriptors.p, slot->samplers.p};
    list->SetDescriptorHeaps(2, heaps);
    std::array<D3D12_RESOURCE_STATES, MaxResources> states = {};
    slot->resources = plan.buffers.resources;
    for (uint32_t i = 0; i < slot->resources; ++i) {
      auto &b = plan.buffers.buffers[i];
      slot->buffers[i] = b;
      slot->uploads[i] = b->upload;
      slot->writes[i] = plan.usage.write[i] != 0;
      states[i] = b->state;
      if (b->upload && scene->resources[i].role != MAG_NATIVE_READBACK)
        list->CopyBufferRegion(b->gpu.p, 0, b->upload->gpu.p, 0, b->allocation);
    }
    for (uint32_t i = 0; i < scene->image_count; ++i)
      if (scene->images[i].upload.id) {
        const auto &desc = scene->images[i];
        auto &im = slot->scene_images[i];
        auto b = buffer(c, desc.upload);
        int source = resource_index(plan.buffers, b);
        transition(list, b->gpu.p, states[source],
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        states[source] = D3D12_RESOURCE_STATE_COPY_SOURCE;
        // Copy tight immutable rows into a real D3D footprint, entirely on
        // the same GPU. No CPU texture readback/reinterpretation is used.
        const uint64_t row = uint64_t(desc.width) * 4;
        for (uint32_t y = 0; y < desc.height; ++y)
          list->CopyBufferRegion(im.upload.p, uint64_t(y) * im.pitch, b->gpu.p,
                                 uint64_t(y) * row, row);
        transition(list, im.upload.p, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = im.upload.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {im.format, desc.width, desc.height, 1,
                                         static_cast<UINT>(im.pitch)};
        dst.pResource = im.gpu.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
    uint32_t descriptor_cursor = 0;
    for (uint32_t n = 0; n < scene->dispatch_count; ++n) {
      const auto &command = plan.compute[n];
      std::array<D3D12_RESOURCE_STATES, MaxResources> desired = {};
      std::array<bool, MaxResources> used = {};
      for (uint32_t i = 0; i < command.count; ++i) {
        const auto &b = command.bindings[i];
        int r = resource_index(plan.buffers, b.buffer);
        const auto &layout = command.pipeline->bindings[b.layout];
        auto state = layout.binding_kind == MAG_NATIVE_UNIFORM
                         ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
                     : layout.access == MAG_NATIVE_READ_WRITE
                         ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                         : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        desired[r] = static_cast<D3D12_RESOURCE_STATES>(desired[r] | state);
        used[r] = true;
      }
      for (uint32_t i = 0; i < slot->resources; ++i)
        if (used[i]) {
          transition(list, slot->buffers[i]->gpu.p, states[i], desired[i]);
          states[i] = desired[i];
        }
      list->SetComputeRootSignature(command.pipeline->root.p);
      list->SetPipelineState(command.pipeline->gpu.p);
      for (uint32_t i = 0; i < command.count; ++i) {
        const auto &b = command.bindings[i];
        descriptor(c, *slot, descriptor_cursor, b,
                   command.pipeline->bindings[b.layout]);
        list->SetComputeRootDescriptorTable(b.layout,
                                            gpu(c, *slot, descriptor_cursor++));
      }
      list->Dispatch(command.groups[0], command.groups[1], command.groups[2]);
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      list->ResourceBarrier(1, &barrier);
    }
    uint32_t draw_index = 0;
    for (uint32_t n = 0; n < scene->pass_count; ++n) {
      const auto &pass = scene->passes[n];
      const uint32_t extent = pass.color.image != MAG_NATIVE_SCENE_NONE
                                  ? pass.color.image
                                  : pass.depth.image;
      const auto &im = scene->images[extent];
      for (uint32_t j = 0; j < pass.draw_count; ++j)
        for (uint32_t k = 0; k < pass.draws[j].texture_count; ++k) {
          auto &image = slot->scene_images[pass.draws[j].textures[k].image];
          const auto desired = static_cast<D3D12_RESOURCE_STATES>(
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          transition(list, image.gpu.p, image.state, desired);
          image.state = desired;
        }
      D3D12_CPU_DESCRIPTOR_HANDLE color = {}, depth = {};
      if (pass.color.image != MAG_NATIVE_SCENE_NONE) {
        auto &image = slot->scene_images[pass.color.image];
        transition(list, image.gpu.p, image.state,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        image.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        color = slot->rtv->GetCPUDescriptorHandleForHeapStart();
        color.ptr += SIZE_T(pass.color.image) * c->rtv_step;
        c->device->CreateRenderTargetView(image.gpu.p, nullptr, color);
      }
      if (pass.depth.image != MAG_NATIVE_SCENE_NONE) {
        auto &image = slot->scene_images[pass.depth.image];
        transition(list, image.gpu.p, image.state,
                   D3D12_RESOURCE_STATE_DEPTH_WRITE);
        image.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        depth = slot->dsv->GetCPUDescriptorHandleForHeapStart();
        depth.ptr += SIZE_T(pass.depth.image) * c->dsv_step;
        D3D12_DEPTH_STENCIL_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_D32_FLOAT;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        c->device->CreateDepthStencilView(image.gpu.p, &view, depth);
      }
      list->OMSetRenderTargets(
          pass.color.image == MAG_NATIVE_SCENE_NONE ? 0 : 1,
          color.ptr ? &color : nullptr, FALSE, depth.ptr ? &depth : nullptr);
      if (color.ptr && pass.color.load == MAG_NATIVE_SCENE_CLEAR)
        list->ClearRenderTargetView(color, pass.color.clear, 0, nullptr);
      if (depth.ptr && pass.depth.load == MAG_NATIVE_SCENE_CLEAR)
        list->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH,
                                    pass.depth.clear[0], 0, 0, nullptr);
      const D3D12_VIEWPORT viewport = {
          0, 0, static_cast<float>(im.width), static_cast<float>(im.height),
          0, 1};
      const D3D12_RECT scissor = {0, 0, static_cast<LONG>(im.width),
                                  static_cast<LONG>(im.height)};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      for (uint32_t j = 0; j < pass.draw_count; ++j) {
        const auto &record = plan.draws[draw_index++];
        const auto &draw = *record.source;
        const auto &p = *record.program;
        std::array<D3D12_RESOURCE_STATES, MaxResources> desired = {};
        std::array<bool, MaxResources> used = {};
        auto vertices = buffer(c, draw.vertices),
             indices = buffer(c, draw.indices);
        int v = resource_index(plan.buffers, vertices),
            ix = resource_index(plan.buffers, indices);
        desired[v] = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        used[v] = true;
        desired[ix] = static_cast<D3D12_RESOURCE_STATES>(
            desired[ix] | D3D12_RESOURCE_STATE_INDEX_BUFFER);
        used[ix] = true;
        for (uint32_t k = 0; k < draw.buffer_count; ++k) {
          const auto &b = draw.buffers[k];
          int r = resource_index(plan.buffers, buffer(c, b.binding.buffer));
          const mag_native_binding_layout *layout = nullptr;
          for (const auto &l : p.scene_buffers)
            if (l.stage == b.stage && l.binding.slot == b.binding.slot) {
              layout = &l.binding;
              break;
            }
          const auto state =
              layout->binding_kind == MAG_NATIVE_UNIFORM
                  ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
                  : static_cast<D3D12_RESOURCE_STATES>(
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          desired[r] = static_cast<D3D12_RESOURCE_STATES>(desired[r] | state);
          used[r] = true;
        }
        for (uint32_t k = 0; k < slot->resources; ++k)
          if (used[k]) {
            transition(list, slot->buffers[k]->gpu.p, states[k], desired[k]);
            states[k] = desired[k];
          }
        list->SetGraphicsRootSignature(p.root.p);
        list->SetPipelineState(p.gpu.p);
        for (uint32_t stage = 0; stage < 2; ++stage) {
          if (p.scene_resource_root[stage] != UINT32_MAX)
            list->SetGraphicsRootDescriptorTable(
                p.scene_resource_root[stage],
                gpu(c, *slot, record.descriptor_base[stage]));
          if (p.scene_model)
            for (uint32_t unit = 0; unit < 16; ++unit)
              if (p.scene_sampler_mask[stage] & (1u << unit)) {
                auto handle =
                    slot->samplers->GetGPUDescriptorHandleForHeapStart();
                handle.ptr += uint64_t(scene_sampler(draw, stage, unit)) *
                              c->sampler_step;
                list->SetGraphicsRootDescriptorTable(
                    p.scene_sampler_root[stage][unit], handle);
              }
        }
        if (p.scene_heap_root != UINT32_MAX)
          list->SetGraphicsRootDescriptorTable(
              p.scene_heap_root,
              slot->samplers->GetGPUDescriptorHandleForHeapStart());
        const D3D12_VERTEX_BUFFER_VIEW vb = {
            vertices->gpu->GetGPUVirtualAddress() + draw.vertex_offset,
            static_cast<UINT>(draw.vertex_length), p.scene_desc.vertex_stride};
        const D3D12_INDEX_BUFFER_VIEW ib = {
            indices->gpu->GetGPUVirtualAddress() + draw.index_offset,
            static_cast<UINT>(draw.index_length), DXGI_FORMAT_R32_UINT};
        list->IASetVertexBuffers(0, 1, &vb);
        list->IASetIndexBuffer(&ib);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawIndexedInstanced(draw.index_count, 1, 0, 0, 0);
      }
      // Discard remains a logical initialization contract enforced by the
      // validator; it need not destroy bytes or issue a CPU operation.
    }
    if (slot->scene_readback) {
      auto &image = slot->scene_images[scene->output_image];
      const auto &im = scene->images[scene->output_image];
      transition(list, image.gpu.p, image.state,
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
      image.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
      D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
      src.pResource = image.gpu.p;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.pResource = slot->scene_readback->gpu.p;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint.Footprint = {
          image.format, im.width, im.height, 1,
          static_cast<UINT>(slot->scene_readback->pitch)};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    result = list->Close();
    if (FAILED(result)) {
      clear_slot(*slot);
      return failure(c, result);
    }
    // Same genuinely retained two-slot contract as execute(batch): reserve
    // ticket/resources before Execute, and every subsequent fault is unsure.
    slot->ticket = {c->generation, ++c->next_ticket};
    slot->fence = ++c->next_fence;
    slot->occupied = true;
    ID3D12CommandList *commands[] = {list};
    c->queue->ExecuteCommandLists(1, commands);
    result = c->queue->Signal(c->fence.p, slot->fence);
    for (uint32_t i = 0; i < slot->resources; ++i) {
      auto &b = slot->buffers[i];
      b->state = states[i];
      if (scene->resources[i].role != MAG_NATIVE_READBACK)
        b->upload.reset();
      if (plan.usage.write[i])
        b->gpu_dirty = true;
    }
    if (slot->scene_readback) {
      auto b = buffer(c, scene->diagnostic_readback);
      b->image = slot->scene_readback;
      b->upload.reset();
    }
    *out = slot->ticket;
    if (FAILED(result) || FAILED(c->device->GetDeviceRemovedReason())) {
      c->failed = true;
      return MAG_NATIVE_INDETERMINATE;
    }
    return MAG_NATIVE_OK;
  } catch (const std::bad_alloc &) {
    if (slot->occupied) {
      c->failed = true;
      *out = slot->ticket;
      return MAG_NATIVE_INDETERMINATE;
    }
    clear_slot(*slot);
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
}
static int submit_scene(void *opaque, const mag_native_scene *scene,
                        mag_native_ticket *out) {
  auto *c = static_cast<Context *>(opaque);
  try {
    ScenePlan plan;
    int status = scene_prepare(c, scene, plan);
    return status ? status : scene_execute(c, scene, plan, out);
  } catch (const std::bad_alloc &) {
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
}
