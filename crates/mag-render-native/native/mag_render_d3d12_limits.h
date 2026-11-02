#ifndef MAG_RENDER_D3D12_LIMITS_H
#define MAG_RENDER_D3D12_LIMITS_H
#include "mag_render_batch.h"

/* CPU-only D3D12 view/allocation limits, shared by preflight and its fixture.
 * Resource creation/queue ownership remain in the native implementation. */
static inline bool mag_d3d12_raw_offset(uint64_t offset) {
  /* D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT, independent of scalar WGSL alignment. */
  return offset % 16 == 0;
}
static inline bool mag_d3d12_attachment_budget(uint64_t *bytes, uint32_t width,
                                               uint32_t height, bool readback) {
  const uint64_t limit = MAG_NATIVE_BATCH_MAX_BYTES;
  if (!width || !height || *bytes > limit)
    return false;
  const uint64_t image_row = uint64_t(width) * 8; // RGBA8 + D32.
  const uint64_t readback_row =
      readback ? (uint64_t(width) * 4 + 255) & ~uint64_t(255) : 0;
  const uint64_t row = image_row + readback_row;
  if (row > (limit - *bytes) / height)
    return false;
  *bytes += row * height;
  return true;
}
#endif
