/* Actual native CPU preflight. No device, queue, shader or GPU is created. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../native/mag_render_d3d12.cpp"
#include <cassert>

extern "C" mag_native_device *mag_native_wrap(const mag_native_ops *, void *) {
  assert(!"CPU preflight must not construct a native device");
  return nullptr;
}
extern "C" uint64_t mag_native_next_generation(void) { return 1; }

static mag_native_buffer add_buffer(Context &c, uint64_t id, uint64_t length) {
  auto b = std::make_shared<Buffer>();
  b->length = length;
  c.buffers[{id, 1}] = b;
  return {c.generation, id, 1};
}
int main() {
  Context c;
  c.generation = 1;
  auto graphics = std::make_shared<Pipeline>();
  graphics->graphics = true;
  c.pipelines[1] = graphics;
  mag_native_indexed_draw draw = {};
  draw.pipeline = {1, 1};
  draw.vertices = add_buffer(c, 1, 16);
  draw.indices = add_buffer(c, 2, 12);
  draw.vertex_length = 16;
  draw.index_length = 12;
  draw.index_count = 3; // A valid degenerate triangle may use {0, 0, 0}.
  draw.width = draw.height = 8;
  draw.clear_depth = 1;
  Command command;
  assert(prepare_draw(&c, &draw, command) == MAG_NATIVE_OK);
  draw.vertex_length = 0;
  assert(prepare_draw(&c, &draw, command) == MAG_NATIVE_INVALID);
  draw.vertex_length = 32; // View exceeds the actual16-byte allocation.
  assert(prepare_draw(&c, &draw, command) == MAG_NATIVE_INVALID);
  draw.vertex_length = 16;
  draw.vertex_offset = 4;
  assert(prepare_draw(&c, &draw, command) == MAG_NATIVE_INVALID);
  std::puts("D3D12 actual draw preflight: one vertex is valid, empty/overrun/misaligned views reject");
}
