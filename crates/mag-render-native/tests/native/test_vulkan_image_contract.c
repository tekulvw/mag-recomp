/* CPU metadata/interop checks only: no VkImage handle or Vulkan driver call. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "mag_render_native_internal.h"
#include "mag_render_vulkan.h"
#include "mag_render_vulkan_image_validate.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#ifdef MAG_RENDER_TEST_SDK_IMAGE_ABI
#include "rsx_native_vulkan_image.h"
_Static_assert(MAG_RENDER_VULKAN_IMAGE_ABI == RSX_NATIVE_VULKAN_IMAGE_ABI,
               "cross-repo version");
_Static_assert(sizeof(MagRenderVulkanImage) == sizeof(rsx_native_vulkan_image),
               "cross-repo size");
_Static_assert(_Alignof(MagRenderVulkanImage) ==
                   _Alignof(rsx_native_vulkan_image),
               "cross-repo alignment");
_Static_assert(sizeof(MagRenderHostImage) == sizeof(rsx_native_image),
               "unchanged cross-repo HostImageABI1");
_Static_assert(offsetof(MagRenderHostImage, image) ==
                   offsetof(rsx_native_image, image),
               "HostImage payload pointer");
_Static_assert(offsetof(MagRenderHostImage, retain) ==
                   offsetof(rsx_native_image, retain),
               "HostImage retain callback");
_Static_assert(offsetof(MagRenderHostImage, validate) ==
                   offsetof(rsx_native_image, validate),
               "HostImage validation callback");
#define SAME(field)                                                            \
  _Static_assert(offsetof(MagRenderVulkanImage, field) ==                      \
                     offsetof(rsx_native_vulkan_image, field),                 \
                 "cross-repo " #field)
SAME(abi_version);
SAME(struct_size);
SAME(image_bits);
SAME(format);
SAME(usage);
SAME(aspect);
SAME(mip_count);
SAME(layers);
SAME(samples);
SAME(queue_family);
SAME(layout);
#undef SAME
#endif
int main(void) {
  _Static_assert(sizeof(MagRenderVulkanImage) == 48,
                 "Vulkan payload ABI1 size");
  MagRenderVulkanImage valid = {.abi_version = 1,
                                .struct_size = sizeof(valid),
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT,
                                .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                .mip_count = 1,
                                .layers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .queue_family = 3,
                                .layout = VK_IMAGE_LAYOUT_GENERAL};
  /* image_bits deliberately remains zero: this validates only metadata, and
   * cannot be mistaken for creating or admitting a usable imported image. */
  assert(!valid.image_bits);
  assert(mag_native_vulkan_image_metadata(
             &valid, 3, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
             VK_IMAGE_USAGE_SAMPLED_BIT) == MAG_NATIVE_OK);
  assert(mag_native_vulkan_image_metadata(&valid, 4, valid.format, valid.aspect,
                                          0) == MAG_NATIVE_UNSUPPORTED);
  assert(mag_native_vulkan_image_metadata(&valid, 3, VK_FORMAT_B8G8R8A8_UNORM,
                                          valid.aspect,
                                          0) == MAG_NATIVE_UNSUPPORTED);
  assert(mag_native_vulkan_image_metadata(&valid, 3, valid.format,
                                          VK_IMAGE_ASPECT_DEPTH_BIT,
                                          0) == MAG_NATIVE_UNSUPPORTED);
  assert(mag_native_vulkan_image_metadata(&valid, 3, valid.format, valid.aspect,
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ==
         MAG_NATIVE_UNSUPPORTED);
  MagRenderVulkanImage changed = valid;
  changed.abi_version = 2;
  assert(mag_native_vulkan_image_metadata(
             &changed, 3, valid.format, valid.aspect, 0) == MAG_NATIVE_INVALID);
  changed = valid;
  changed.struct_size -= 8;
  assert(mag_native_vulkan_image_metadata(
             &changed, 3, valid.format, valid.aspect, 0) == MAG_NATIVE_INVALID);
  changed = valid;
  changed.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  assert(mag_native_vulkan_image_metadata(&changed, 3, valid.format,
                                          valid.aspect,
                                          0) == MAG_NATIVE_UNSUPPORTED);
  changed = valid;
  changed.mip_count = 2;
  assert(mag_native_vulkan_image_metadata(&changed, 3, valid.format,
                                          valid.aspect,
                                          0) == MAG_NATIVE_UNSUPPORTED);
  changed = valid;
  changed.layers = 6;
  assert(mag_native_vulkan_image_metadata(&changed, 3, valid.format,
                                          valid.aspect,
                                          0) == MAG_NATIVE_UNSUPPORTED);
  changed = valid;
  changed.samples = VK_SAMPLE_COUNT_4_BIT;
  assert(mag_native_vulkan_image_metadata(&changed, 3, valid.format,
                                          valid.aspect,
                                          0) == MAG_NATIVE_UNSUPPORTED);
  changed = valid;
  changed.usage = 0;
  assert(mag_native_vulkan_image_metadata(&changed, 3, valid.format,
                                          valid.aspect,
                                          0) == MAG_NATIVE_UNSUPPORTED);
  puts("PASS exact Vulkan import metadata/version/layout/family/creation "
       "facts; no image or driver");
  return 0;
}
