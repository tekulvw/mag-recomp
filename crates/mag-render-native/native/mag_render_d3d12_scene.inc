/* Frontend-owned D3D12 multipass scene; shares the actual Context/Slot fence
 * ownership with compute/batch. No device/queue/window or SDK is created. */
static DXGI_FORMAT scene_format(uint32_t format) {
  static const DXGI_FORMAT formats[] = {DXGI_FORMAT_UNKNOWN,
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT,
    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8_UNORM,
    DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_BC1_UNORM,
    DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC3_UNORM,
    DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM,
    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
    DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_D24_UNORM_S8_UINT,DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
  return format < sizeof(formats)/sizeof(formats[0]) ? formats[format] : DXGI_FORMAT_UNKNOWN;
}
struct SceneBlob {
  mag_render_dxc_blob value = {};
  ~SceneBlob() { mag_render_dxc_free(&value); }
};
static int scene_graphics_layout(Context *c,
                                 const mag_native_scene_graphics_desc *desc,
                                 const mag_native_vertex_attribute *attributes,
                                 uint32_t attribute_count, uint32_t model,
                                 const mag_native_raster_state *raster,mag_native_pipeline *out) {
  if (c->failed)
    return MAG_NATIVE_DEVICE_FAILED;
  if(raster&&raster->stencil_enabled&&
     ((raster->front_stencil.reference&255u)!=(raster->back_stencil.reference&255u)||
      (raster->front_stencil.read_mask&255u)!=(raster->back_stencil.read_mask&255u)||
      (raster->front_stencil.write_mask&255u)!=(raster->back_stencil.write_mask&255u)))return MAG_NATIVE_UNSUPPORTED;
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
        scene_format(desc->depth_format);
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
    if(raster){
      p->raster=*raster;p->explicit_raster=true;
      const D3D12_CULL_MODE culls[]={D3D12_CULL_MODE_NONE,D3D12_CULL_MODE_FRONT,D3D12_CULL_MODE_BACK};
      gpu.RasterizerState.CullMode=culls[raster->cull_mode];
      gpu.RasterizerState.FrontCounterClockwise=raster->front_face==MAG_NATIVE_FRONT_CCW;
      const D3D12_COMPARISON_FUNC full_compares[]={D3D12_COMPARISON_FUNC_NEVER,D3D12_COMPARISON_FUNC_LESS,D3D12_COMPARISON_FUNC_EQUAL,D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER,D3D12_COMPARISON_FUNC_NOT_EQUAL,D3D12_COMPARISON_FUNC_GREATER_EQUAL,D3D12_COMPARISON_FUNC_ALWAYS};
      gpu.DepthStencilState.DepthEnable=raster->depth_enabled&&desc->depth_format;
      gpu.DepthStencilState.DepthWriteMask=raster->depth_enabled&&desc->depth_format&&raster->depth_write?D3D12_DEPTH_WRITE_MASK_ALL:D3D12_DEPTH_WRITE_MASK_ZERO;
      gpu.DepthStencilState.DepthFunc=full_compares[raster->depth_compare];
      gpu.DepthStencilState.StencilEnable=raster->stencil_enabled;
      if(raster->stencil_enabled){
        const D3D12_STENCIL_OP ops[]={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_ZERO,D3D12_STENCIL_OP_REPLACE,D3D12_STENCIL_OP_INCR_SAT,D3D12_STENCIL_OP_DECR_SAT,D3D12_STENCIL_OP_INVERT,D3D12_STENCIL_OP_INCR,D3D12_STENCIL_OP_DECR};
        const mag_native_stencil_face *faces[]={&raster->front_stencil,&raster->back_stencil};
        for(unsigned i=0;i<2;++i){const auto &f=*faces[i];D3D12_DEPTH_STENCILOP_DESC value={ops[f.fail],ops[f.depth_fail],ops[f.pass],full_compares[f.compare]};if(i)gpu.DepthStencilState.BackFace=value;else gpu.DepthStencilState.FrontFace=value;}
        gpu.DepthStencilState.StencilReadMask=static_cast<UINT8>(raster->front_stencil.read_mask);
        gpu.DepthStencilState.StencilWriteMask=static_cast<UINT8>(raster->front_stencil.write_mask);
      }

      const D3D12_BLEND factors[]={D3D12_BLEND_ZERO,D3D12_BLEND_ONE,D3D12_BLEND_SRC_COLOR,D3D12_BLEND_INV_SRC_COLOR,D3D12_BLEND_DEST_COLOR,D3D12_BLEND_INV_DEST_COLOR,
        D3D12_BLEND_SRC_ALPHA,D3D12_BLEND_INV_SRC_ALPHA,D3D12_BLEND_DEST_ALPHA,D3D12_BLEND_INV_DEST_ALPHA,D3D12_BLEND_BLEND_FACTOR,D3D12_BLEND_INV_BLEND_FACTOR,
        D3D12_BLEND_BLEND_FACTOR,D3D12_BLEND_INV_BLEND_FACTOR,D3D12_BLEND_SRC_ALPHA_SAT};
      const D3D12_BLEND_OP equations[]={D3D12_BLEND_OP_ADD,D3D12_BLEND_OP_SUBTRACT,D3D12_BLEND_OP_REV_SUBTRACT,D3D12_BLEND_OP_MIN,D3D12_BLEND_OP_MAX};
      auto &blend=gpu.BlendState.RenderTarget[0];blend.BlendEnable=raster->blend_enabled;
      blend.SrcBlend=factors[raster->src_rgb];blend.DestBlend=factors[raster->dst_rgb];
      blend.SrcBlendAlpha=factors[raster->src_alpha];blend.DestBlendAlpha=factors[raster->dst_alpha];
      blend.BlendOp=equations[raster->rgb_equation];blend.BlendOpAlpha=equations[raster->alpha_equation];blend.RenderTargetWriteMask=static_cast<UINT8>(raster->color_write_mask);
    }
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
                               1, 0, nullptr, out);
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
                               desc->binding_model, nullptr, out);
}
static int scene_graphics_create_v3(void *opaque,const mag_native_scene_graphics_v3_desc *desc,mag_native_pipeline *out){
  int status=mag_native_scene_graphics_v3_validate(desc);if(status)return status;
    if(desc->raster.depth_bounds_enabled)return MAG_NATIVE_UNSUPPORTED;
  mag_native_scene_graphics_desc schema;mag_native_scene_graphics_v3_schema(desc,&schema);
  return scene_graphics_layout(static_cast<Context*>(opaque),&schema,desc->base.attributes,desc->base.attribute_count,desc->base.binding_model,&desc->raster,out);
}
struct SceneDrawPlan {
  const mag_native_scene_draw *source = nullptr;
  std::shared_ptr<Pipeline> program;
  std::array<uint32_t, 2> descriptor_base = {};
  std::array<uint64_t, 2> sampler_indices = {{UINT64_MAX, UINT64_MAX}};
};
struct ScenePlan {
  const mag_native_scene_v4 *stencil=nullptr;
  const mag_native_scene_v3 *extended=nullptr;
  const mag_native_sampler *sampler_table=nullptr;uint32_t sampler_count=0;
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
static int scene_raster_info(void *opaque,mag_native_pipeline ref,const mag_native_raster_state **out){
 auto p=pipeline(static_cast<Context*>(opaque),ref);if(!p)return MAG_NATIVE_STALE;if(!p->scene)return MAG_NATIVE_INVALID;*out=p->explicit_raster?&p->raster:nullptr;return MAG_NATIVE_OK;
}
static int scene_prepare(Context *c, const mag_native_scene *scene,
                         ScenePlan &plan,const mag_native_sampler *sampler_table=nullptr,uint32_t sampler_count=0,const mag_native_scene_v3 *extended=nullptr,const mag_native_scene_v4 *stencil=nullptr) {
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
  plan.sampler_table=sampler_table;plan.sampler_count=sampler_count;
  mag_native_scene_registry_v2 registry_v2={&registry,sampler_table,sampler_count,sampler_info};
  plan.extended=extended;plan.stencil=stencil;
  mag_native_scene_registry_v4 registry_v4={&registry_v2,scene_raster_info};
  int status = stencil?mag_native_scene_validate_v4(stencil,&registry_v4,&plan.usage):extended ? mag_native_scene_validate_v3(extended, &registry_v2, &plan.usage) : mag_native_scene_validate_v2(scene, &registry_v2, &plan.usage);
  if (status) {return status;}
    /* Unequal attachment encoding is currently enabled only for Metal. */
    for(uint32_t n=0;n<scene->pass_count;++n){const mag_native_scene_pass *p=&scene->passes[n];
        if(p->color.image!=MAG_NATIVE_SCENE_NONE&&p->depth.image!=MAG_NATIVE_SCENE_NONE){
            const mag_native_scene_image *color=&scene->images[p->color.image],*depth=&scene->images[p->depth.image];
            if(color->width!=depth->width||color->height!=depth->height)return MAG_NATIVE_UNSUPPORTED;
        }
    }
  plan.buffers.buffers.resize(scene->resource_count);
  plan.buffers.roles.resize(scene->resource_count);
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
    if (bytes > MAG_NATIVE_SCENE_MAX_BYTES - plan.bytes)
      return false;
    plan.bytes += bytes;
    return true;
  };
  if (!reserve(uint64_t(plan.sampler_records) * 64))
    return MAG_NATIVE_ALLOCATION_FAILED;
  // A Texture2D shader cannot sample a D3D12 Texture2DArray face view.
  // Refuse this capability before allocating or submitting any scene work.
  if (extended) for(uint32_t i=0;i<extended->view_count;++i) {
    const auto &view=extended->views[i];
    if(view.dimension==2 && extended->image_layout_count &&
       extended->image_layouts[view.image].layers!=1) return MAG_NATIVE_UNSUPPORTED;
  }
  for (uint32_t i = 0; i < scene->image_count; ++i) {
    const auto &im=scene->images[i];
    if (!im.upload.id) continue;
    const auto *layout=extended && extended->image_layout_count ? &extended->image_layouts[i] : nullptr;
    const uint32_t levels=layout ? layout->subresource_count : 1;
    uint64_t staging=0;
    for(uint32_t j=0;j<levels;++j) {
      const uint32_t w=layout ? layout->subresources[j].width : im.width;
      const uint32_t h=layout ? layout->subresources[j].height : im.height;
      uint64_t row=0,rows=0,bytes=0;
      if(!mag_native_image_level_layout(im.format,w,h,&row,&rows,&bytes)) return MAG_NATIVE_INVALID;
      staging=(staging+511)&~UINT64_C(511);
      staging+=padded(row)*rows;
    }
    if(!reserve(staging)) return MAG_NATIVE_ALLOCATION_FAILED;
  }
  return MAG_NATIVE_OK;
}
static int scene_image(Context *c, const mag_native_scene_image &im,
                       const mag_native_scene_image_layout *layout, SceneImage &out) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = im.width;
  desc.Height = im.height;
  desc.DepthOrArraySize = static_cast<UINT16>(layout ? layout->layers : 1);
  desc.MipLevels = static_cast<UINT16>(layout ? layout->mip_count : 1);
  desc.SampleDesc.Count = 1;
  const bool depth = im.format == MAG_NATIVE_SCENE_D32||mag_native_image_has_stencil(im.format);
  mag_native_image_format info={};
  if(!mag_native_image_format_info(im.format,&info)) return MAG_NATIVE_INVALID;
  out.format = scene_format(im.format);
  desc.Format = im.format==MAG_NATIVE_SCENE_D24S8?DXGI_FORMAT_R24G8_TYPELESS:
      im.format==MAG_NATIVE_SCENE_D32S8?DXGI_FORMAT_R32G8X24_TYPELESS:depth?DXGI_FORMAT_R32_TYPELESS:out.format;
  desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                     : info.color_attachment ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {
      out.format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
  HRESULT result = c->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                  &support, sizeof(support));
  const UINT required = D3D12_FORMAT_SUPPORT1_TEXTURE2D |
      (depth ? D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL : D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) |
      (info.color_attachment ? D3D12_FORMAT_SUPPORT1_RENDER_TARGET : 0);
  if (FAILED(result) || (UINT(support.Support1) & required) != required)
    return MAG_NATIVE_UNSUPPORTED;
  out.state = im.upload.id ? D3D12_RESOURCE_STATE_COPY_DEST
              : depth      ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                           : D3D12_RESOURCE_STATE_RENDER_TARGET;
  result = c->device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, out.state, nullptr,
      __uuidof(ID3D12Resource), reinterpret_cast<void **>(out.gpu.put()));
  if (FAILED(result)) return failure(c, result);
  if (im.upload.id) {
    const uint32_t count=layout ? layout->subresource_count : 1;
    out.footprints.resize(count);out.rows.resize(count);out.row_bytes.resize(count);
    UINT64 total=0;
    c->device->GetCopyableFootprints(&desc,0,count,0,out.footprints.data(),out.rows.data(),out.row_bytes.data(),&total);
    if(total==UINT64_MAX || total>MAG_NATIVE_SCENE_MAX_BYTES) return MAG_NATIVE_ALLOCATION_FAILED;
    result = allocation(c, D3D12_HEAP_TYPE_DEFAULT, total,
                        D3D12_RESOURCE_STATE_COPY_DEST, out.upload);
    if (FAILED(result)) return failure(c, result);
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
      return (draw.textures[i].sampler&MAG_NATIVE_SCENE_SAMPLER_TABLE_BIT)?3+MAG_NATIVE_SCENE_SAMPLER_TABLE_INDEX(draw.textures[i].sampler):draw.textures[i].sampler;
  return UINT32_MAX;
}
static void scene_descriptors(Context *c, Slot &slot,
                              const SceneDrawPlan &record, const mag_native_scene_v3 *extended) {
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
        const uint32_t image=mag_native_scene_image_index(extended,t.image);
        const auto &im = slot.scene_images[image];
        const auto *explicit_view=(t.image & MAG_NATIVE_SCENE_VIEW_TABLE_BIT) ? &extended->views[MAG_NATIVE_SCENE_VIEW_TABLE_INDEX(t.image)] : nullptr;
        const auto *image_layout=extended && extended->image_layout_count ? &extended->image_layouts[image] : nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = layout.format==MAG_NATIVE_SCENE_D24S8?DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
          layout.format==MAG_NATIVE_SCENE_D32S8?DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:layout.format == MAG_NATIVE_SCENE_D32
                          ? DXGI_FORMAT_R32_FLOAT
                          : scene_format(layout.format);
        if(explicit_view) view.Shader4ComponentMapping=D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
            explicit_view->remap[0],explicit_view->remap[1],explicit_view->remap[2],explicit_view->remap[3]);
        const uint32_t dimension=explicit_view ? explicit_view->dimension : image_layout ? image_layout->dimension : 2;
        const uint32_t mip=explicit_view ? explicit_view->base_mip : 0;
        const uint32_t levels=explicit_view ? explicit_view->mip_count : image_layout ? image_layout->mip_count : 1;
        if(dimension==4) {
          view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;
          view.TextureCube.MostDetailedMip=mip;view.TextureCube.MipLevels=levels;
        } else {
          view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
          view.Texture2D.MostDetailedMip=mip;view.Texture2D.MipLevels=levels;
        }
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
  slot.sampler_registry=&c->samplers;
  slot.sampler_entries.reserve(plan.sampler_count);
  for(uint32_t i=0;i<plan.sampler_count;++i)if(plan.usage.samplers[i]){
    mag_native_sampler_entry *entry=nullptr;status=mag_native_sampler_registry_pin(&c->samplers,plan.sampler_table[i],&entry);if(status)return status;
    slot.sampler_entries.push_back(entry);
    auto *payload=static_cast<NativeSamplerPayload*>(entry->payload);
    auto dest=slot.samplers->GetCPUDescriptorHandleForHeapStart();dest.ptr+=SIZE_T(3+i)*c->sampler_step;
    c->device->CopyDescriptorsSimple(1,dest,payload->cpu,D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  }
  slot.buffers.resize((std::max)(scene->resource_count,MaxResources));
  slot.uploads.resize((std::max)(scene->resource_count,MaxResources));
  slot.writes.resize((std::max)(scene->resource_count,MaxResources));
  slot.scene_images.resize(scene->image_count);
  for (uint32_t i = 0; i < scene->image_count; ++i) {
    status = scene_image(c, scene->images[i], plan.extended && plan.extended->image_layout_count ? &plan.extended->image_layouts[i] : nullptr, slot.scene_images[i]);
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
    scene_descriptors(c, slot, record,plan.extended);
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
    std::vector<D3D12_RESOURCE_STATES> states(scene->resource_count);
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
        const auto *layout=plan.extended && plan.extended->image_layout_count ? &plan.extended->image_layouts[i] : nullptr;
        for(uint32_t sub=0;sub<im.footprints.size();++sub) {
          const auto &footprint=im.footprints[sub];
          const uint64_t source_offset=layout ? layout->subresources[sub].offset : 0;
          for(uint32_t y=0;y<im.rows[sub];++y)
            list->CopyBufferRegion(im.upload.p,footprint.Offset+uint64_t(y)*footprint.Footprint.RowPitch,
                b->gpu.p,source_offset+uint64_t(y)*im.row_bytes[sub],im.row_bytes[sub]);
        }
        transition(list, im.upload.p, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        for(uint32_t sub=0;sub<im.footprints.size();++sub) {
          D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
          src.pResource = im.upload.p;
          src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          src.PlacedFootprint=im.footprints[sub];
          dst.pResource = im.gpu.p;
          dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          dst.SubresourceIndex=sub;
          list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
      }
    uint32_t descriptor_cursor = 0;
    for (uint32_t n = 0; n < scene->dispatch_count; ++n) {
      const auto &command = plan.compute[n];
      std::vector<D3D12_RESOURCE_STATES> desired(scene->resource_count);
      std::vector<uint8_t> used(scene->resource_count);
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
          auto &image = slot->scene_images[mag_native_scene_image_index(plan.extended,pass.draws[j].textures[k].image)];
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
        view.Format = image.format;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        c->device->CreateDepthStencilView(image.gpu.p, &view, depth);
      }
      list->OMSetRenderTargets(
          pass.color.image == MAG_NATIVE_SCENE_NONE ? 0 : 1,
          color.ptr ? &color : nullptr, FALSE, depth.ptr ? &depth : nullptr);
      if (color.ptr && pass.color.load == MAG_NATIVE_SCENE_CLEAR)
        list->ClearRenderTargetView(color, pass.color.clear, 0, nullptr);
      if(depth.ptr){
        UINT flags=pass.depth.load==MAG_NATIVE_SCENE_CLEAR?D3D12_CLEAR_FLAG_DEPTH:0;
        UINT8 stencil_value=0;
        if(plan.stencil&&mag_native_image_has_stencil(scene->images[pass.depth.image].format)){
          const auto &ops=plan.stencil->stencils[n];if(ops.load==MAG_NATIVE_STENCIL_CLEAR)flags|=D3D12_CLEAR_FLAG_STENCIL;stencil_value=static_cast<UINT8>(ops.clear_value);
        }
        if(flags)list->ClearDepthStencilView(depth,static_cast<D3D12_CLEAR_FLAGS>(flags),pass.depth.clear[0],stencil_value,0,nullptr);
      }
      D3D12_VIEWPORT viewport = {
          0, 0, static_cast<float>(im.width), static_cast<float>(im.height),
          0, 1};
      D3D12_RECT scissor = {0, 0, static_cast<LONG>(im.width),
                                  static_cast<LONG>(im.height)};
      if(plan.extended && plan.extended->viewport_count) {
        const auto &v=plan.extended->viewports[n];
        viewport={v.x,v.y,v.width,v.height,v.min_depth,v.max_depth};
        scissor={LONG(v.scissor_x),LONG(v.scissor_y),LONG(v.scissor_x+v.scissor_width),LONG(v.scissor_y+v.scissor_height)};
        if(!v.scissor_width || !v.scissor_height) {draw_index+=pass.draw_count;continue;}
      }
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      for (uint32_t j = 0; j < pass.draw_count; ++j) {
        const auto &record = plan.draws[draw_index++];
        const auto &draw = *record.source;
        const auto &p = *record.program;
        std::vector<D3D12_RESOURCE_STATES> desired(scene->resource_count);
        std::vector<uint8_t> used(scene->resource_count);
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
        if(p.explicit_raster){
          float constants[4];std::copy(p.raster.blend_constant,p.raster.blend_constant+4,constants);
          if(p.raster.src_rgb==12||p.raster.src_rgb==13||p.raster.dst_rgb==12||p.raster.dst_rgb==13)
            constants[0]=constants[1]=constants[2]=constants[3];
          list->OMSetBlendFactor(constants);
          if(p.raster.stencil_enabled)list->OMSetStencilRef(p.raster.front_stencil.reference&255u);
        }
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
static int submit_scene_impl(void *opaque,const mag_native_scene *scene,mag_native_ticket *out,const mag_native_sampler *sampler_table,uint32_t sampler_count,const mag_native_scene_v3 *extended=nullptr,const mag_native_scene_v4 *stencil=nullptr) {
  auto *c = static_cast<Context *>(opaque);
  try {
    ScenePlan plan;
    int status = scene_prepare(c, scene, plan,sampler_table,sampler_count,extended,stencil);
    return status ? status : scene_execute(c, scene, plan, out);
  } catch (const std::bad_alloc &) {
    return MAG_NATIVE_ALLOCATION_FAILED;
  }
}

static int submit_scene(void *opaque,const mag_native_scene *scene,mag_native_ticket *out){return submit_scene_impl(opaque,scene,out,nullptr,0);}
static int submit_scene_v2(void *opaque,const mag_native_scene_v2 *desc,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
 if(target||frame)return MAG_NATIVE_UNSUPPORTED;
 return submit_scene_impl(opaque,desc->scene,out,desc->samplers,desc->sampler_count);
}

static int submit_scene_v3(void *opaque,const mag_native_scene_v3 *desc,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
 if(target||frame)return MAG_NATIVE_UNSUPPORTED;
 return submit_scene_impl(opaque,desc->base.scene,out,desc->base.samplers,desc->base.sampler_count,desc);
}

static int submit_scene_v4(void *opaque,const mag_native_scene_v4 *desc,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
 if(target||frame)return MAG_NATIVE_UNSUPPORTED;
 return submit_scene_impl(opaque,desc->base.base.scene,out,desc->base.base.samplers,desc->base.base.sampler_count,&desc->base,desc);
}
