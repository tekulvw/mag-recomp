#ifndef MAG_RENDER_VULKAN_IMAGE_H
#define MAG_RENDER_VULKAN_IMAGE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MAG_RENDER_VULKAN_IMAGE_ABI 1u
/* For HostImage.backend=VULKAN, HostImage.image points to this immutable
 * provider-owned descriptor, NOT directly to a VkImage. Retain the HostImage
 * before reading it. It describes a genuine same-device optimal-tiled2D image
 * with exact creation facts; the provider owns its allocation and destruction.
 * All users of the borrowed queue must acquire it in GENERAL and leave it in
 * GENERAL before releasing the owner lease. No queue-family transfer, external
 * semaphore, second device, ownership of VkDeviceMemory or implicit clear is
 * inferred. Callbacks/generation also cover the descriptor's storage lifetime.
 * Vulkan enum/flag values are stored verbatim without a Vulkan SDK dependency.
 * image_bits preserves the opaque64-bit VkImage handle; no address dereference.
 */
typedef struct MagRenderVulkanImage {
  uint32_t abi_version, struct_size;
  uint64_t image_bits;
  uint32_t format, usage, aspect, mip_count, layers, samples, queue_family,
      layout;
} MagRenderVulkanImage;
#ifdef __cplusplus
}
#endif
#endif
