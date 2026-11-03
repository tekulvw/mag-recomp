/* Descriptor layout only: no Windows or GPU headers/runtime required. */
#include "mag_render_d3d12_image.h"
#include "mag_render_host_image.h"
#include <stddef.h>
#include <stdio.h>
#ifdef MAG_RENDER_TEST_SDK_IMAGE_ABI
#include "rsx_native_d3d12_image.h"
_Static_assert(MAG_RENDER_D3D12_IMAGE_ABI == RSX_NATIVE_D3D12_IMAGE_ABI,
               "descriptor version");
_Static_assert(sizeof(MagRenderD3D12Image) == sizeof(rsx_native_d3d12_image),
               "descriptor size");
_Static_assert(_Alignof(MagRenderD3D12Image) ==
                   _Alignof(rsx_native_d3d12_image),
               "descriptor alignment");
#define FIELD(f)                                                               \
  _Static_assert(offsetof(MagRenderD3D12Image, f) ==                           \
                     offsetof(rsx_native_d3d12_image, f),                      \
                 "descriptor " #f)
FIELD(abi_version);
FIELD(struct_size);
FIELD(resource);
FIELD(acquire_state);
FIELD(handback_state);
#undef FIELD
_Static_assert(sizeof(MagRenderHostImage) == sizeof(rsx_native_image),
               "base HostImage unchanged");
_Static_assert(offsetof(MagRenderHostImage, image) ==
                   offsetof(rsx_native_image, image),
               "base image payload");
_Static_assert(offsetof(MagRenderHostImage, retain) ==
                   offsetof(rsx_native_image, retain),
               "base retain");
_Static_assert(offsetof(MagRenderHostImage, validate) ==
                   offsetof(rsx_native_image, validate),
               "base validation");
#endif
int main(void) {
  _Static_assert(offsetof(MagRenderD3D12Image, resource) == 8,
                 "descriptor pointer offset");
  _Static_assert(offsetof(MagRenderD3D12Image, handback_state) ==
                     12 + sizeof(void *),
                 "descriptor handback offset");
  puts("PASS D3D12 import descriptor ABI only; no "
       "resource/runtime/device/queue/GPU");
  return 0;
}
