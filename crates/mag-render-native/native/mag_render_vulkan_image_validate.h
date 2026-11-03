#ifndef MAG_RENDER_VULKAN_IMAGE_VALIDATE_H
#define MAG_RENDER_VULKAN_IMAGE_VALIDATE_H
#include "mag_render_vulkan.h"
/* Pure metadata check, never proof that image_bits names a real image. Native
 * import separately requires a successful genuine provider retain, a nonzero
 * handle and format capabilities on the actual borrowed physical device. */
static int mag_native_vulkan_image_metadata(const MagRenderVulkanImage *image,
                                            uint32_t queue_family,
                                            uint32_t format, uint32_t aspect,
                                            uint32_t usage) {
  if (image->abi_version != MAG_RENDER_VULKAN_IMAGE_ABI ||
      image->struct_size != sizeof(*image))
    return MAG_NATIVE_INVALID;
  if (image->format != format || image->aspect != aspect || image->usage == 0 ||
      (image->usage & usage) != usage || image->mip_count != 1 ||
      image->layers != 1 || image->samples != VK_SAMPLE_COUNT_1_BIT ||
      image->queue_family != queue_family ||
      image->layout != VK_IMAGE_LAYOUT_GENERAL)
    return MAG_NATIVE_UNSUPPORTED;
  return MAG_NATIVE_OK;
}
#endif
