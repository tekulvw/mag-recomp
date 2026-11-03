/* Exercise the actual non-Windows SDK exports: no renderer/device is created.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#undef _WIN32
#include "rsx_live_draw.c"
#include <assert.h>
#include <stdio.h>
int main(void) {
  uint64_t ticket = 99;
  assert(rsx_live_draw_native_boundary(&ticket) == -2 && !ticket);
  assert(rsx_live_draw_borrow_image(1, 1, 0) == -2);
  assert(rsx_live_draw_validate_image_planes(0, 1, 1, 1, 1) == -2);
  assert(rsx_live_draw_native_image(0, 0) == -2);
  assert(rsx_live_draw_native_registers(0, 0, 0) == -2);
  assert(rsx_live_draw_native_canvas(0) == -2);
  puts("PASS actual non-Windows D3D12 SDK exports remain Unsupported; no "
       "runtime/device/queue/GPU");
  return 0;
}
