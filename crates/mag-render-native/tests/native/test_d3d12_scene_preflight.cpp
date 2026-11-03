/* Actual D3D scene CPU admission. No COM device, queue, shader or GPU is made.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../native/mag_render_d3d12.cpp"
#include <cassert>
extern "C" mag_native_device *mag_native_wrap(const mag_native_ops *, void *) {
  assert(!"preflight must not create a device");
  return nullptr;
}
extern "C" uint64_t mag_native_next_generation(void) { return 1; }
static mag_native_buffer data(Context &c, uint64_t id, uint64_t length) {
  auto b = std::make_shared<Buffer>();
  b->length = length;
  b->allocation = padded(length);
  b->seeded = true;
  c.buffers[{id, 1}] = b;
  return {c.generation, id, 1};
}
static unsigned serialized;
static HRESULT WINAPI check_stage_spaces(
    const D3D12_ROOT_SIGNATURE_DESC *signature, D3D_ROOT_SIGNATURE_VERSION,
    ID3DBlob **bytes, ID3DBlob **error) {
  *bytes = nullptr;
  *error = nullptr;
  bool seen[2][3] = {};
  for (UINT i = 0; i < signature->NumParameters; ++i) {
    const auto &root = signature->pParameters[i];
    assert(root.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
    const unsigned stage =
        root.ShaderVisibility == D3D12_SHADER_VISIBILITY_VERTEX ? 0 : 1;
    assert(root.ShaderVisibility == (stage ? D3D12_SHADER_VISIBILITY_PIXEL
                                           : D3D12_SHADER_VISIBILITY_VERTEX));
    for (UINT j = 0; j < root.DescriptorTable.NumDescriptorRanges; ++j) {
      const auto &r = root.DescriptorTable.pDescriptorRanges[j];
      assert(r.RegisterSpace ==
             stage); // actual canonical HLSL: VS space0, FS space1
      if (r.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
        assert(r.BaseShaderRegister == stage);
        seen[stage][0] = true;
      } else if (r.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
        assert(r.BaseShaderRegister == 0);
        seen[stage][1] = true;
      } else {
        assert(r.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER &&
               r.BaseShaderRegister == 0);
        seen[stage][2] = true;
      }
    }
  }
  for (const auto &stage : seen)
    for (bool binding : stage)
      assert(binding);
  ++serialized;
  return E_FAIL; // stop before ANY device/PSO call
}
static void actual_pipeline_root_matches_canonical_stages() {
  Context context;
  context.serialize = check_stage_spaces;
  const char source[] = "source-owned HLSL";
  const char entry[] = "main";
  mag_native_scene_buffer_layout buffers[] = {
      {0,
       0,
       {0, MAG_NATIVE_READ, 16, 0, MAG_NATIVE_NO_SIZE_INDEX, 4,
        MAG_NATIVE_UNIFORM}},
      {1,
       0,
       {1, MAG_NATIVE_READ, 16, 0, MAG_NATIVE_NO_SIZE_INDEX, 4,
        MAG_NATIVE_UNIFORM}}};
  mag_native_scene_texture_layout textures[] = {
      {0, 0, 0, MAG_NATIVE_SCENE_RGBA8, 0, 0},
      {1, 0, 0, MAG_NATIVE_SCENE_RGBA8, 0, 0}};
  const mag_native_vertex_attribute attribute = {0, MAG_NATIVE_VERTEX_FLOAT4, 0,
                                                 0};
  mag_native_scene_graphics_v2_desc desc = {};
  desc.struct_size = sizeof(desc);
  desc.abi_version = 2;
  desc.base.struct_size = sizeof(desc.base);
  desc.base.abi_version = 1;
  desc.base.vertex_stride = 16;
  desc.base.color_format = MAG_NATIVE_SCENE_BGRA8;
  desc.base.vertex = {source, sizeof(source) - 1, entry, 4, MAG_NATIVE_HLSL};
  desc.base.fragment = desc.base.vertex;
  desc.base.buffers = buffers;
  desc.base.buffer_count = 2;
  desc.base.textures = textures;
  desc.base.texture_count = 2;
  desc.attributes = &attribute;
  desc.attribute_count = 1;
  desc.binding_model = 1;
  mag_native_pipeline result = {};
  assert(scene_graphics_create_v2(&context, &desc, &result) ==
         MAG_NATIVE_DEVICE_FAILED);
  assert(serialized == 1 && result.id == 0);
}
static void whole_scene_resources_exceed_retained_batch_capacity() {
  Context c;c.generation=7;
  std::vector<mag_native_batch_resource> resources;
  std::vector<mag_native_scene_image> images;
  images.push_back({MAG_NATIVE_SCENE_RGBA8,1,1,0,{}});
  for(uint32_t i=0;i<65;++i){
    auto ref=data(c,100+i,4);
    resources.push_back({ref,MAG_NATIVE_UPLOAD,0});
    images.push_back({MAG_NATIVE_SCENE_RGBA8,1,1,0,ref});
  }
  mag_native_scene_pass pass={};pass.color={0,MAG_NATIVE_SCENE_CLEAR,MAG_NATIVE_SCENE_STORE,0,{0,0,0,1}};
  pass.depth.image=MAG_NATIVE_SCENE_NONE;
  mag_native_scene scene={};scene.struct_size=sizeof(scene);scene.abi_version=1;
  scene.resources=resources.data();scene.resource_count=static_cast<uint32_t>(resources.size());
  scene.images=images.data();scene.image_count=static_cast<uint32_t>(images.size());
  scene.passes=&pass;scene.pass_count=1;scene.output_image=0;
  ScenePlan plan;assert(scene_prepare(&c,&scene,plan)==MAG_NATIVE_OK);
  assert(plan.buffers.resources==65 && plan.buffers.buffers.size()==65);
  assert(plan.buffers.buffers.back()==buffer(&c,resources.back().buffer));
}
static void asymmetric_stencil_is_refused_before_any_COM_call(){
 Context c;mag_native_scene_graphics_desc desc={};mag_native_raster_state state={};
 state.stencil_enabled=1;state.front_stencil.reference=0x81;state.back_stencil.reference=0x80;mag_native_pipeline out={};
 assert(scene_graphics_layout(&c,&desc,nullptr,0,1,&state,&out)==MAG_NATIVE_UNSUPPORTED);
 state.back_stencil.reference=0x81;state.front_stencil.read_mask=255;
 assert(scene_graphics_layout(&c,&desc,nullptr,0,1,&state,&out)==MAG_NATIVE_UNSUPPORTED);
 state.back_stencil.read_mask=255;state.front_stencil.write_mask=255;
 assert(scene_graphics_layout(&c,&desc,nullptr,0,1,&state,&out)==MAG_NATIVE_UNSUPPORTED);
}
int main() {
  asymmetric_stencil_is_refused_before_any_COM_call();
  actual_pipeline_root_matches_canonical_stages();
  whole_scene_resources_exceed_retained_batch_capacity();
  Context c;
  c.generation = 7;
  auto p = std::make_shared<Pipeline>();
  p->graphics = p->scene = true;
  p->scene_model = 1;
  p->scene_buffers.push_back(
      {1,
       0,
       {0, MAG_NATIVE_READ, 16, 0, MAG_NATIVE_NO_SIZE_INDEX, 4,
        MAG_NATIVE_UNIFORM}});
  p->scene_textures.push_back({1, 0, 0, MAG_NATIVE_SCENE_RGBA8, 0, 0});
  p->scene_desc.struct_size = sizeof(p->scene_desc);
  p->scene_desc.abi_version = 1;
  p->scene_desc.vertex_stride = 24;
  p->scene_desc.color_format = MAG_NATIVE_SCENE_BGRA8;
  p->scene_desc.fragment.source_length = 1;
  p->scene_desc.buffers = p->scene_buffers.data();
  p->scene_desc.buffer_count = 1;
  p->scene_desc.textures = p->scene_textures.data();
  p->scene_desc.texture_count = 1;
  p->scene_resource_count[1] = 2;
  p->scene_sampler_index.fill(UINT32_MAX);
  p->scene_sampler_mask[1] = 1;
  c.pipelines[1] = p;
  const auto vertices = data(c, 1, 72), indices = data(c, 2, 12),
             uniform = data(c, 3, 272), texels = data(c, 4, 256);
  mag_native_batch_resource resources[] = {{vertices, MAG_NATIVE_UPLOAD, 0},
                                           {indices, MAG_NATIVE_UPLOAD, 0},
                                           {uniform, MAG_NATIVE_UPLOAD, 0},
                                           {texels, MAG_NATIVE_UPLOAD, 0}};
  mag_native_scene_image images[] = {{MAG_NATIVE_SCENE_BGRA8, 8, 8, 0, {}},
                                     {MAG_NATIVE_SCENE_RGBA8, 8, 8, 0, texels}};
  mag_native_scene_buffer_binding binding = {1, 0, {0, 0, uniform, 0, 16}};
  mag_native_scene_texture_binding texture = {
      1, 0, 0, 1, MAG_NATIVE_SCENE_NEAREST, 0};
  mag_native_scene_draw draw = {};
  draw.pipeline = {7, 1};
  draw.vertices = vertices;
  draw.indices = indices;
  draw.vertex_length = 72;
  draw.index_length = 12;
  draw.index_count = 3;
  draw.buffer_count = draw.texture_count = 1;
  draw.buffers = &binding;
  draw.textures = &texture;
  mag_native_scene_pass pass = {};
  pass.color = {
      0, MAG_NATIVE_SCENE_CLEAR, MAG_NATIVE_SCENE_STORE, 0, {0, 0, 0, 1}};
  pass.depth.image = MAG_NATIVE_SCENE_NONE;
  pass.draws = &draw;
  pass.draw_count = 1;
  mag_native_scene scene = {};
  scene.struct_size = sizeof(scene);
  scene.abi_version = 1;
  scene.resources = resources;
  scene.resource_count = 4;
  scene.images = images;
  scene.image_count = 2;
  scene.passes = &pass;
  scene.pass_count = 1;
  scene.output_image = 0;
  {
    ScenePlan plan;
    assert(scene_prepare(&c, &scene, plan) == MAG_NATIVE_OK);
    assert(plan.draws.size() == 1 && plan.descriptors == 2 &&
           plan.sampler_records == 0);
  }
  binding.binding.offset = 4;
  {
    ScenePlan plan;
    assert(scene_prepare(&c, &scene, plan) == MAG_NATIVE_INVALID);
  } // actual256-byte CBV alignment
  binding.binding.offset = 256;
  {
    ScenePlan plan;
    assert(scene_prepare(&c, &scene, plan) == MAG_NATIVE_OK);
  }
  binding.binding.offset = 0;
  draw.vertex_length = 70;
  {
    ScenePlan plan;
    assert(scene_prepare(&c, &scene, plan) == MAG_NATIVE_INVALID);
  } // torn canonical vertex
  draw.vertex_length = 72;
  p->scene_model = 0;
  {
    ScenePlan plan;
    assert(scene_prepare(&c, &scene, plan) == MAG_NATIVE_INVALID);
  } // b0/t0/s0 requires explicit namespaces
  p->scene_model = 1;
  draw.texture_count = 0;
  {
    ScenePlan plan;
    assert(scene_prepare(&c, &scene, plan) == MAG_NATIVE_INVALID);
  }
  draw.texture_count = 1;
  draw.pipeline.device_generation = 6;
  {
    ScenePlan plan;
    assert(scene_prepare(&c, &scene, plan) == MAG_NATIVE_STALE);
  }
  std::puts(
      "D3D12 complete scene preflight: BGRA, typedstride and material "
      "bindings; badCBV/tornvertex/namespace/missing/stale refuse withoutGPU");
}
