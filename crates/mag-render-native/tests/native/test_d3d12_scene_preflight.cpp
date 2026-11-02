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
int main() {
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
