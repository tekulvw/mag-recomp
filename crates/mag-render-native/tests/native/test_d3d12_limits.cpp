/* Real preflight arithmetic only: no native device, queue or GPU execution. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../native/mag_render_d3d12_limits.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>

int main(int argc, char **argv) {
  const bool raw = argc == 1 || std::strcmp(argv[1], "raw") == 0;
  const bool budget = argc == 1 || std::strcmp(argv[1], "budget") == 0;
  if (raw) {
    assert(mag_d3d12_raw_offset(0));
    assert(mag_d3d12_raw_offset(16));
    assert(mag_d3d12_raw_offset(256));
    assert(!mag_d3d12_raw_offset(4));
    assert(!mag_d3d12_raw_offset(12));
    assert(!mag_d3d12_raw_offset(17));
  }
  if (budget) {
    uint64_t bytes = 208;
    assert(mag_d3d12_attachment_budget(&bytes, 1, 1, false));
    assert(bytes == 216); // RGBA8 color plus D32 depth.
    assert(mag_d3d12_attachment_budget(&bytes, 1, 1, true));
    assert(bytes == 480); // Two images plus a 256-byte readback row.
    bytes = MAG_NATIVE_BATCH_MAX_BYTES - 8;
    assert(mag_d3d12_attachment_budget(&bytes, 1, 1, false));
    assert(bytes == MAG_NATIVE_BATCH_MAX_BYTES);
    assert(!mag_d3d12_attachment_budget(&bytes, 1, 1, false));
    assert(bytes == MAG_NATIVE_BATCH_MAX_BYTES); // Failure is atomic.
    bytes = 0;
    assert(!mag_d3d12_attachment_budget(&bytes, 4096, 4096, false));
    assert(!mag_d3d12_attachment_budget(&bytes, UINT32_MAX, UINT32_MAX, true));
    assert(!mag_d3d12_attachment_budget(&bytes, 0, 8, false));
    assert(bytes == 0);
    bytes = std::numeric_limits<uint64_t>::max();
    assert(!mag_d3d12_attachment_budget(&bytes, 1, 1, false));
  }
  std::puts("D3D12 CPU preflight: raw-view alignment and bounded attachments");
}
