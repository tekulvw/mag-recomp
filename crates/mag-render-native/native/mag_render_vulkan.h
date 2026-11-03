/* Extracted from ps3recomp libs/video/rsx_frontend_vulkan.h at 6e0fcc2e026cddc02fb94f23050ac8429a03306a.
 * Copyright (c) 2026 Ned Heller / sp00nznet. MIT; see LICENSE.ps3recomp.
 * Frontend-owned implementation; no SDK runtime/header dependency. */
#ifndef MAG_RENDER_NATIVE_VULKAN_H
#define MAG_RENDER_NATIVE_VULKAN_H
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "mag_render_native.h"
#include "mag_render_vulkan_image.h"

/* Native implementation state only; no SDK headers or pthread internals. */
typedef struct mag_native_vulkan_owner {
    MagRenderHostOwner host;
    VkDevice device;VkQueue queue;uint32_t queue_family;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
} mag_native_vulkan_owner;
#endif
