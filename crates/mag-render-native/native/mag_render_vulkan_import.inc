#include "mag_render_scene_validate.h"
#include "mag_render_vulkan_image_validate.h"
static VkFormat scene_vk_format(uint32_t);
static frontend_vk_imported_image *imported_image_find(frontend_vk *c,
                                                       mag_native_image ref) {
  if (ref.device_generation != c->generation || !ref.id)
    return NULL;
  for (frontend_vk_imported_image *image = c->images; image;
       image = image->next)
    if (image->id == ref.id && image->public_ref)
      return image;
  return NULL;
}
static void imported_image_dispose(frontend_vk *c,
                                   frontend_vk_imported_image *image) {
  frontend_vk_imported_image **link = &c->images;
  while (*link && *link != image)
    link = &(*link)->next;
  if (*link)
    *link = image->next;
  MagRenderHostImage host = image->host;
  free(image);
  /* Public handles and each occupied native slot own this shared lease. Its
   * final release is outside the queue lease, after actual fence retirement. */
  host.release(host.userdata, host.owner_lifetime, host.resource_id,
               host.resource_generation);
}
static void imported_image_unpin(frontend_vk *c,
                                 frontend_vk_imported_image *image) {
  if (image->references)
    --image->references;
  if (!image->references && !image->public_ref)
    imported_image_dispose(c, image);
}
static int import_image(frontend_vk *c, const MagRenderHostImage *host,
                        mag_native_validate_planes_fn planes, int certified,
                        mag_native_image *out) {
  if (c->failed)
    return MAG_NATIVE_DEVICE_FAILED;
  if (host->backend != MAG_RENDER_HOST_VULKAN ||
      host->owner_lifetime != c->owner.host.lifetime)
    return MAG_NATIVE_STALE;
  unsigned count = 0;
  for (frontend_vk_imported_image *p = c->images; p; p = p->next)
    ++count;
  if (count >= MAG_NATIVE_SCENE_MAX_IMAGES + 2 || c->next_image == UINT64_MAX)
    return MAG_NATIVE_ALLOCATION_FAILED;
  int status = host->retain(host->userdata, host->owner_lifetime,
                            host->resource_id, host->resource_generation);
  if (status)
    return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
  MagRenderVulkanImage metadata = {0};
  if (!host->image) {
    status = VK_REFUSE(MAG_NATIVE_INVALID);
    goto fail;
  }
  /* A foreign provider type has no C effective-type alias with this header. */
  uint32_t header[2];
  memcpy(header, host->image, sizeof(header));
  if (header[0] != MAG_RENDER_VULKAN_IMAGE_ABI ||
      header[1] != sizeof(metadata)) {
    status = VK_REFUSE(MAG_NATIVE_INVALID);
    goto fail;
  }
  memcpy(&metadata, host->image, sizeof(metadata));
  mag_native_image_format format;
  if (!host->width || !host->height || !metadata.image_bits ||
      host->width > c->owner.properties.limits.maxImageDimension2D ||
      host->height > c->owner.properties.limits.maxImageDimension2D ||
      !mag_native_image_format_info(host->format, &format)) {
    status = VK_REFUSE(MAG_NATIVE_UNSUPPORTED);
    goto fail;
  }
  VkFormat vkformat = scene_vk_format(host->format);
  VkImageAspectFlags aspect = format.depth_attachment
                                  ? VK_IMAGE_ASPECT_DEPTH_BIT
                                  : VK_IMAGE_ASPECT_COLOR_BIT;
  if (mag_native_image_has_stencil(host->format))
    aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
  VkImageUsageFlags attachment =
      format.depth_attachment ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                              : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  status = mag_native_vulkan_image_metadata(&metadata, c->owner.queue_family,
                                            vkformat, aspect,
                                            certified ? 0 : attachment);
  if (status)
    goto fail;
  if (vkformat == VK_FORMAT_UNDEFINED ||
      !(metadata.usage & (attachment | VK_IMAGE_USAGE_SAMPLED_BIT))) {
    status = VK_REFUSE(MAG_NATIVE_UNSUPPORTED);
    goto fail;
  }
  if (host->format >= MAG_NATIVE_SCENE_BC1 &&
      host->format <= MAG_NATIVE_SCENE_BC3 &&
      !(c->known_features & c->enabled_features & MAG_RENDER_HOST_FEATURE_BC)) {
    status = VK_REFUSE(MAG_NATIVE_UNSUPPORTED);
    goto fail;
  }
  for (frontend_vk_imported_image *p = c->images; p; p = p->next)
    if (p->metadata.image_bits == metadata.image_bits &&
        (p->host.resource_id != host->resource_id ||
         p->host.resource_generation != host->resource_generation)) {
      status = MAG_NATIVE_STALE;
      goto fail;
    }
  PFN_vkGetPhysicalDeviceFormatProperties query =
      (PFN_vkGetPhysicalDeviceFormatProperties)c->owner.host.get_instance_proc(
          c->owner.host.instance, "vkGetPhysicalDeviceFormatProperties");
  if (!query) {
    status = VK_REFUSE(MAG_NATIVE_UNSUPPORTED);
    goto fail;
  }
  VkFormatProperties properties;
  query((VkPhysicalDevice)c->owner.host.physical_device, vkformat, &properties);
  VkFormatFeatureFlags features = 0;
  if (metadata.usage & VK_IMAGE_USAGE_SAMPLED_BIT)
    features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  if (metadata.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
    features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  if (metadata.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
    features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if ((properties.optimalTilingFeatures & features) != features) {
    status = VK_REFUSE(MAG_NATIVE_UNSUPPORTED);
    goto fail;
  }
  PFN_vkGetPhysicalDeviceImageFormatProperties image_query =
      (PFN_vkGetPhysicalDeviceImageFormatProperties)c->owner.host
          .get_instance_proc(c->owner.host.instance,
                             "vkGetPhysicalDeviceImageFormatProperties");
  VkImageFormatProperties image_properties;
  if (!image_query ||
      image_query((VkPhysicalDevice)c->owner.host.physical_device, vkformat,
                  VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, metadata.usage, 0,
                  &image_properties) != VK_SUCCESS ||
      host->width > image_properties.maxExtent.width ||
      host->height > image_properties.maxExtent.height ||
      !(image_properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) {
    status = VK_REFUSE(MAG_NATIVE_UNSUPPORTED);
    goto fail;
  }
  frontend_vk_imported_image *image = calloc(1, sizeof(*image));
  if (!image) {
    status = MAG_NATIVE_ALLOCATION_FAILED;
    goto fail;
  }
  image->host = *host;
  image->metadata = metadata;
  image->validate_planes = planes;
  image->id = ++c->next_image;
  image->public_ref = 1;
  image->next = c->images;
  c->images = image;
  *out = (mag_native_image){c->generation, image->id};
  return MAG_NATIVE_OK;
fail:
  host->release(host->userdata, host->owner_lifetime, host->resource_id,
                host->resource_generation);
  return status;
}
static int image_import(void *opaque, const MagRenderHostImage *host,
                        mag_native_image *out) {
  return import_image(opaque, host, NULL, 0, out);
}
static int image_import_certified(void *opaque,
                                  const mag_native_image_certificate *cert,
                                  mag_native_image *out) {
  return import_image(opaque, &cert->host, cert->validate_planes, 1, out);
}
static int image_release(void *opaque, mag_native_image ref) {
  frontend_vk *c = opaque;
  frontend_vk_imported_image *image = imported_image_find(c, ref);
  if (!image)
    return MAG_NATIVE_STALE;
  image->public_ref = 0;
  if (!image->references)
    imported_image_dispose(c, image);
  return MAG_NATIVE_OK;
}
static int scene_import_info(void *opaque, mag_native_image ref,
                             uint32_t required, mag_native_import_info *out) {
  frontend_vk *c = opaque;
  frontend_vk_imported_image *image = imported_image_find(c, ref);
  if (!image)
    return MAG_NATIVE_STALE;
  /* Encoding a reservation runs without the owner lease: only the exact
   * imports validated under the lease when it was reserved are admitted. */
  if (c->reserved_encoding) {
    for (uint32_t k = 0; k < c->reserved_count; ++k) {
      const mag_native_scene_import *r = &c->reserved_imports[k];
      if (r->ref.device_generation == ref.device_generation && r->ref.id == ref.id &&
          r->required_initialized_planes == required) {
        *out = c->reserved_info[k];
        return MAG_NATIVE_OK;
      }
    }
    return MAG_NATIVE_STALE;
  }
  const MagRenderHostImage *h = &image->host;
  int status = h->validate(h->userdata, h->owner_lifetime, h->resource_id,
                           h->resource_generation);
  if (status)
    return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
  if (required) {
    if (!image->validate_planes)
      return VK_REFUSE(MAG_NATIVE_UNSUPPORTED);
    status =
        image->validate_planes(h->userdata, h->owner_lifetime, h->resource_id,
                               h->resource_generation, required);
    if (status)
      return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
  }
  *out = (mag_native_import_info){h->format,
                                  h->width,
                                  h->height,
                                  0,
                                  h->owner_lifetime,
                                  h->resource_id,
                                  h->resource_generation};
  return MAG_NATIVE_OK;
}
