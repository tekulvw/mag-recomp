#ifndef MAG_RENDER_D3D12_IMAGE_H
#define MAG_RENDER_D3D12_IMAGE_H
#include <stdint.h>
#define MAG_RENDER_D3D12_IMAGE_ABI 1u
/* Backend D3D12 HostImage.image points to this immutable retained descriptor,
 * not directly to a resource. Retain before reading. The actual same-device
 * ID3D12Resource is validated with GetDevice/GetDesc; enum values are verbatim.
 * State metadata certifies the provider's recorded, checked original boundary.
 * Native encoding must hand the whole resource back in handback_state before
 * queue Execute/Signal. No device, queue, implicit clear or host wait is added.
 */
typedef struct MagRenderD3D12Image {
  uint32_t abi_version, struct_size;
  void *resource;
  uint32_t acquire_state, handback_state;
} MagRenderD3D12Image;
#endif
