/* Extracted from ps3recomp libs/video/rsx_frontend_vulkan.c at 6e0fcc2e026cddc02fb94f23050ac8429a03306a.
 * Copyright (c) 2026 Ned Heller / sp00nznet. MIT; see LICENSE.ps3recomp.
 * Frontend-owned implementation; no SDK runtime/header dependency. */
#include "mag_render_vulkan.h"
#include "mag_render_native_internal.h"
#include "mag_render_sampler_registry.h"
#include "mag_render_host_owner.h"
#include "mag_render_scene_validate.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define FRONTEND_VK_FUNCTIONS(X) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkGetFenceStatus) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) X(vkCmdPipelineBarrier) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdDispatch) \
    X(vkQueueSubmit) X(vkCreateGraphicsPipelines) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdCopyImageToBuffer) \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCmdCopyBufferToImage) X(vkCmdSetDepthBounds)

typedef struct frontend_vk_family {
    uint64_t id, last_version; uint32_t public_versions; struct frontend_vk_family *next;
} frontend_vk_family;
typedef struct frontend_vk_buffer {
    uint64_t id, version, length; uint32_t public_ref, readers, writers; int seed_zero,gpu_written;
    VkBuffer gpu; VkDeviceMemory memory; VkDeviceSize memory_bytes; void *mapped; struct frontend_vk_buffer *next;
} frontend_vk_buffer;
typedef struct frontend_vk_imported_image {
    uint64_t id;uint32_t public_ref,references;
    MagRenderHostImage host;MagRenderVulkanImage metadata;
    mag_native_validate_planes_fn validate_planes;
    struct frontend_vk_imported_image *next;
} frontend_vk_imported_image;
struct frontend_vk_resident;
typedef struct frontend_vk_scene_image {struct frontend_vk_resident *resident;uint8_t resident_hit,resident_candidate;VkImage image;VkDeviceMemory memory;VkDeviceSize memory_bytes;VkImageView view,sampled_view;VkImageLayout layout;VkImageAspectFlags aspect;uint32_t mip_count,layers;frontend_vk_imported_image *borrowed;} frontend_vk_scene_image;
typedef struct frontend_vk_pipeline {
    uint64_t id; uint32_t public_ref, references, count; int graphics;VkRenderPass render_pass;
    mag_native_binding_layout bindings[16];
    VkDescriptorSetLayout descriptors; VkPipelineLayout layout; VkPipeline gpu;
    int explicit_raster;mag_native_raster_state raster;int scene;uint32_t scene_binding_model;mag_native_scene_graphics_desc scene_desc;
    mag_native_scene_buffer_layout scene_buffers[32];mag_native_scene_texture_layout scene_textures[32];VkDescriptorSetLayout scene_sets[2];
    struct frontend_vk_pipeline *next;
} frontend_vk_pipeline;
typedef struct frontend_vk_slot {
    VkCommandPool pool; VkCommandBuffer command; VkFence fence; VkDescriptorPool descriptors;
    uint32_t descriptor_sets,descriptor_counts[4];
    mag_native_ticket ticket; uint32_t occupied, count;
    frontend_vk_buffer *buffers[MAG_NATIVE_SCENE_MAX_RESOURCES]; uint32_t writes[MAG_NATIVE_SCENE_MAX_RESOURCES]; frontend_vk_pipeline *pipeline;
    frontend_vk_pipeline *batch_pipelines[MAG_NATIVE_BATCH_MAX_COMMANDS + MAG_NATIVE_SCENE_MAX_DRAWS];uint32_t batch_count;
    frontend_vk_scene_image *scene_images;uint32_t scene_image_count,scene_pass_count;
    VkImageView *scene_views;uint32_t scene_view_count;
    VkFramebuffer scene_framebuffers[16];VkRenderPass scene_renderpasses[16];VkSampler scene_samplers[3];
    mag_native_sampler_entry *sampler_entries[MAG_NATIVE_MAX_SAMPLERS];uint32_t sampler_count;
    VkImage color,depth;VkDeviceMemory color_memory,depth_memory;VkDeviceSize color_bytes,depth_bytes;
    VkImageView color_view,depth_view;VkFramebuffer framebuffer;
    VkQueryPool timestamps; int timed;   /* MAG_RENDER_NATIVE_GPU_TIMING: scene start/end */
} frontend_vk_slot;
typedef struct frontend_vk {
    mag_native_vulkan_owner owner;
    mag_native_sampler_registry samplers;
    uint64_t generation, next_buffer, next_pipeline, next_submission,next_image;
    uint64_t known_features,enabled_features;
    int failed;
    frontend_vk_family *families; frontend_vk_buffer *buffers; frontend_vk_pipeline *pipelines;
    frontend_vk_imported_image *images;
    frontend_vk_slot slots[2];
    /* Memory census (mag_native_memory_stats): bytes this context allocated
     * and still holds, and scene submissions for the periodic report. */
    uint64_t allocated_bytes, scene_submissions;
    uint32_t memory_report_every;
    /* MAG_RENDER_NATIVE_GPU_TIMING=1: GPU time of each scene submission from
     * two timestamps, reported in the Metal backend's line format. */
    int timing_enabled; uint32_t timing_samples; uint64_t timestamp_mask; double timestamp_ns;
    PFN_vkCreateQueryPool vkCreateQueryPool; PFN_vkDestroyQueryPool vkDestroyQueryPool;
    PFN_vkCmdResetQueryPool vkCmdResetQueryPool; PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp;
    PFN_vkGetQueryPoolResults vkGetQueryPoolResults;
    /* Reserved-order submission (reserve_scene): the host's queue order, and
     * the one outstanding reservation's imports, validated under the lease. */
    MagRenderVulkanQueueOrder order;
    int order_ok, reserved, reserved_encoding;
    uint64_t reserved_token;
    uint32_t reserved_count;
    mag_native_scene_import reserved_imports[MAG_NATIVE_SCENE_MAX_IMAGES];
    mag_native_import_info reserved_info[MAG_NATIVE_SCENE_MAX_IMAGES];
    mag_native_ticket reserved_ticket;
    /* Resident uploaded images (MAG_RENDER_VULKAN_RESIDENT_IMAGES=1). */
    struct frontend_vk_resident *residents;
    int resident_enabled, resident_report;
    uint32_t resident_entries;
    uint64_t resident_bytes, resident_hits, resident_identity_hits, resident_misses, resident_evictions;
#define DECLARE(name) PFN_##name name;
    FRONTEND_VK_FUNCTIONS(DECLARE)
#undef DECLARE
} frontend_vk;
/* Provided by the host when it supports reserved queue order. */
#if defined(__GNUC__) || defined(__clang__)
#pragma weak mag_render_host_vulkan_queue_order
#endif
static void imported_image_unpin(frontend_vk *,frontend_vk_imported_image *);
static void resident_unref(frontend_vk *,struct frontend_vk_resident *);
static void imported_image_dispose(frontend_vk *,frontend_vk_imported_image *);
/* Every device allocation of this context goes through these two, so the
 * census holds exactly what it still owns. */
static VkResult native_memory_allocate(frontend_vk *c,const VkMemoryAllocateInfo *info,VkDeviceMemory *memory,VkDeviceSize *bytes){
    VkResult result=c->vkAllocateMemory(c->owner.device,info,NULL,memory);
    if(result==VK_SUCCESS){c->allocated_bytes+=info->allocationSize;*bytes=info->allocationSize;}
    return result;
}
static void native_memory_free(frontend_vk *c,VkDeviceMemory memory,VkDeviceSize *bytes){
    if(!memory)return;
    c->vkFreeMemory(c->owner.device,memory,NULL);
    c->allocated_bytes-=*bytes<=c->allocated_bytes?*bytes:c->allocated_bytes;*bytes=0;
}

static frontend_vk_buffer *buffer_find(frontend_vk *c, mag_native_buffer ref) {
    if (ref.device_generation != c->generation || !ref.id || !ref.version) return NULL;
    for (frontend_vk_buffer *b = c->buffers; b; b = b->next)
        if (b->id == ref.id && b->version == ref.version && b->public_ref) return b;
    return NULL;
}
static frontend_vk_pipeline *pipeline_find(frontend_vk *c, mag_native_pipeline ref) {
    if (ref.device_generation != c->generation || !ref.id) return NULL;
    for (frontend_vk_pipeline *p = c->pipelines; p; p = p->next)
        if (p->id == ref.id && p->public_ref) return p;
    return NULL;
}
static void buffer_dispose(frontend_vk *c, frontend_vk_buffer *b) {
    frontend_vk_buffer **link = &c->buffers;
    while (*link && *link != b) link = &(*link)->next;
    if (*link) *link = b->next;
    if (b->mapped) c->vkUnmapMemory(c->owner.device, b->memory);
    if (b->gpu) c->vkDestroyBuffer(c->owner.device, b->gpu, NULL);
    native_memory_free(c, b->memory, &b->memory_bytes);
    free(b);
}
static void pipeline_dispose(frontend_vk *c, frontend_vk_pipeline *p) {
    frontend_vk_pipeline **link = &c->pipelines;
    while (*link && *link != p) link = &(*link)->next;
    if (*link) *link = p->next;
    if (p->gpu) c->vkDestroyPipeline(c->owner.device, p->gpu, NULL);
    if (p->layout) c->vkDestroyPipelineLayout(c->owner.device, p->layout, NULL);
    if (p->descriptors) c->vkDestroyDescriptorSetLayout(c->owner.device, p->descriptors, NULL);
    for(unsigned i=0;i<2;++i)if(p->scene_sets[i])c->vkDestroyDescriptorSetLayout(c->owner.device,p->scene_sets[i],NULL);
    if(p->render_pass)c->vkDestroyRenderPass(c->owner.device,p->render_pass,NULL);
    free(p);
}
static void batch_attachments_release(frontend_vk *c,frontend_vk_slot *slot){
    if(slot->framebuffer)c->vkDestroyFramebuffer(c->owner.device,slot->framebuffer,NULL);
    if(slot->color_view)c->vkDestroyImageView(c->owner.device,slot->color_view,NULL);
    if(slot->depth_view)c->vkDestroyImageView(c->owner.device,slot->depth_view,NULL);
    if(slot->color)c->vkDestroyImage(c->owner.device,slot->color,NULL);
    if(slot->depth)c->vkDestroyImage(c->owner.device,slot->depth,NULL);
    native_memory_free(c,slot->color_memory,&slot->color_bytes);
    native_memory_free(c,slot->depth_memory,&slot->depth_bytes);
    slot->framebuffer=VK_NULL_HANDLE;slot->color_view=slot->depth_view=VK_NULL_HANDLE;
    slot->color=slot->depth=VK_NULL_HANDLE;slot->color_memory=slot->depth_memory=VK_NULL_HANDLE;
}

static void scene_attachments_release(frontend_vk *c,frontend_vk_slot *slot){
 for(uint32_t i=0;i<slot->sampler_count;++i){if(slot->sampler_entries[i])mag_native_sampler_registry_unpin(&c->samplers,slot->sampler_entries[i]);slot->sampler_entries[i]=NULL;}slot->sampler_count=0;
 for(uint32_t i=0;i<slot->scene_pass_count;++i){if(slot->scene_framebuffers[i])c->vkDestroyFramebuffer(c->owner.device,slot->scene_framebuffers[i],NULL);if(slot->scene_renderpasses[i])c->vkDestroyRenderPass(c->owner.device,slot->scene_renderpasses[i],NULL);slot->scene_framebuffers[i]=VK_NULL_HANDLE;slot->scene_renderpasses[i]=VK_NULL_HANDLE;}slot->scene_pass_count=0;
 for(uint32_t i=0;i<slot->scene_view_count;++i)if(slot->scene_views[i])c->vkDestroyImageView(c->owner.device,slot->scene_views[i],NULL);
 slot->scene_view_count=0;free(slot->scene_views);slot->scene_views=NULL;
 for(uint32_t i=0;i<slot->scene_image_count;++i){frontend_vk_scene_image *im=&slot->scene_images[i];if(im->sampled_view)c->vkDestroyImageView(c->owner.device,im->sampled_view,NULL);if(im->view)c->vkDestroyImageView(c->owner.device,im->view,NULL);if(im->image&&!im->borrowed&&!im->resident)c->vkDestroyImage(c->owner.device,im->image,NULL);if(im->memory&&!im->borrowed&&!im->resident)native_memory_free(c,im->memory,&im->memory_bytes);frontend_vk_imported_image *borrowed=im->borrowed;struct frontend_vk_resident *resident=im->resident;memset(im,0,sizeof(*im));if(borrowed)imported_image_unpin(c,borrowed);if(resident)resident_unref(c,resident);}slot->scene_image_count=0;free(slot->scene_images);slot->scene_images=NULL;
 for(unsigned i=0;i<3;++i){if(slot->scene_samplers[i])c->vkDestroySampler(c->owner.device,slot->scene_samplers[i],NULL);slot->scene_samplers[i]=VK_NULL_HANDLE;}
}
static void slot_release(frontend_vk *c, frontend_vk_slot *slot) {
    batch_attachments_release(c,slot);scene_attachments_release(c,slot);
    for(uint32_t i=0;i<slot->batch_count;++i){frontend_vk_pipeline *p=slot->batch_pipelines[i];if(!--p->references&&!p->public_ref)pipeline_dispose(c,p);}
    slot->batch_count=0;
    for (uint32_t i = 0; i < slot->count; ++i) {
        frontend_vk_buffer *b = slot->buffers[i];
        if (slot->writes[i]) --b->writers; else --b->readers;
        if (!b->public_ref && !b->readers && !b->writers) buffer_dispose(c, b);
    }
    if (slot->pipeline && !--slot->pipeline->references && !slot->pipeline->public_ref)
        pipeline_dispose(c, slot->pipeline);
    slot->occupied = slot->count = 0; slot->pipeline = NULL;
}
static int caps(void *opaque, mag_native_caps_desc *out) {
    frontend_vk *c = opaque;
    *out = (mag_native_caps_desc){sizeof(*out), 1, c->generation, 1, 1, 2, 16, UINT32_MAX, 0};
    return MAG_NATIVE_OK;
}
static int buffer_create(void *opaque, mag_native_buffer previous,
    const mag_native_buffer_desc *desc, mag_native_buffer *out) {
    frontend_vk *c = opaque;
    if (c->failed) return MAG_NATIVE_DEVICE_FAILED;
    if (!desc->byte_length || desc->byte_length % 4) return MAG_NATIVE_INVALID;
    if (desc->byte_length > UINT32_MAX) return MAG_NATIVE_UNSUPPORTED;
    frontend_vk_family *family = NULL;
    if (previous.id) {
        if (!buffer_find(c, previous)) return MAG_NATIVE_STALE;
        for (family = c->families; family && family->id != previous.id; family = family->next) {}
        if (!family || family->last_version == UINT64_MAX) return MAG_NATIVE_STALE;
    } else if (c->next_buffer == UINT64_MAX) return MAG_NATIVE_ALLOCATION_FAILED;
    frontend_vk_buffer *b = calloc(1, sizeof(*b));
    if (!b) return MAG_NATIVE_ALLOCATION_FAILED;
    b->length = desc->byte_length;b->seed_zero=!desc->initial_bytes;
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = b->length,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (c->vkCreateBuffer(c->owner.device, &info, NULL, &b->gpu) != VK_SUCCESS) goto fail;
    VkMemoryRequirements requirements; c->vkGetBufferMemoryRequirements(c->owner.device, b->gpu, &requirements);
    uint32_t type;
    for (type = 0; type < c->owner.memory.memoryTypeCount; ++type)
        if ((requirements.memoryTypeBits & (1u << type)) &&
            (c->owner.memory.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
    if (type == c->owner.memory.memoryTypeCount) { buffer_dispose(c, b); return MAG_NATIVE_UNSUPPORTED; }
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = type};
    if (native_memory_allocate(c, &allocation, &b->memory, &b->memory_bytes) != VK_SUCCESS ||
        c->vkBindBufferMemory(c->owner.device, b->gpu, b->memory, 0) != VK_SUCCESS ||
        c->vkMapMemory(c->owner.device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped) != VK_SUCCESS) goto fail;
    if (desc->initial_bytes) memcpy(b->mapped, desc->initial_bytes, (size_t)b->length);
    else memset(b->mapped, 0, (size_t)b->length);
    if (!family) {
        family = calloc(1, sizeof(*family)); if (!family) goto fail;
        family->id = ++c->next_buffer; family->next = c->families; c->families = family;
    }
    b->id = family->id; b->version = ++family->last_version; ++family->public_versions;
    b->public_ref = 1; b->next = c->buffers; c->buffers = b;
    *out = (mag_native_buffer){c->generation, b->id, b->version}; return MAG_NATIVE_OK;
fail:
    buffer_dispose(c, b); return MAG_NATIVE_ALLOCATION_FAILED;
}
static int buffer_release(void *opaque, mag_native_buffer ref) {
    frontend_vk *c = opaque; frontend_vk_buffer *b = buffer_find(c, ref);
    if (!b) return MAG_NATIVE_STALE;
    frontend_vk_family **link = &c->families;
    while (*link && (*link)->id != ref.id) link = &(*link)->next;
    if (*link && !--(*link)->public_versions) { frontend_vk_family *old = *link; *link = old->next; free(old); }
    b->public_ref = 0;
    if (!b->readers && !b->writers) buffer_dispose(c, b);
    return MAG_NATIVE_OK;
}
static int buffer_read(void *opaque, mag_native_buffer ref, uint64_t offset, void *out, uint64_t length) {
    frontend_vk *c = opaque; frontend_vk_buffer *b = buffer_find(c, ref);
    if (!b) return MAG_NATIVE_STALE;
    if (offset > b->length || length > b->length - offset) return MAG_NATIVE_INVALID;
    if (b->readers || b->writers) return MAG_NATIVE_BUSY;
    if (c->failed) return MAG_NATIVE_DEVICE_FAILED;
    memcpy(out, (const char *)b->mapped + offset, (size_t)length); return MAG_NATIVE_OK;
}
/* In-place upload rewrite for opt-in arena reuse (MAG_RENDER_RESIDENT_ARENAS=1),
 * gated exactly like readback: no submission that used this buffer may still
 * be pending or unpolled, so the GPU never observes a partial rewrite. Every
 * buffer here is host-visible and coherent, so the write needs no flush; the
 * next submission's host-write barrier orders it before GPU reads. */
static int buffer_write(void *opaque, mag_native_buffer ref, uint64_t offset, const void *source, uint64_t length) {
    frontend_vk *c = opaque; frontend_vk_buffer *b = buffer_find(c, ref);
    if (!b) return MAG_NATIVE_STALE;
    if (c->failed) return MAG_NATIVE_DEVICE_FAILED;
    if (b->readers || b->writers) return MAG_NATIVE_BUSY;
    if (b->gpu_written || !source || offset > b->length || length > b->length - offset) return MAG_NATIVE_INVALID;
    memcpy((char *)b->mapped + offset, source, (size_t)length);
    b->seed_zero = 0;
    return MAG_NATIVE_OK;
}
/* Census in the shared layout (mag_render_imported_image.h). out[0] is the
 * device memory this context allocated and still holds -- Metal reports its
 * whole device's currentAllocatedSize instead, which also counts the SDK's
 * resources. out[4] counts resident images; nothing else outlives its slot
 * (out[7]). */
static void memory_census(frontend_vk *c, uint64_t out[MAG_NATIVE_MEMORY_STATS]) {
    uint64_t buffers = 0, pipelines = 0, images = 0, pending = 0;
    for (frontend_vk_buffer *b = c->buffers; b; b = b->next) ++buffers;
    for (frontend_vk_pipeline *p = c->pipelines; p; p = p->next) ++pipelines;
    for (frontend_vk_imported_image *i = c->images; i; i = i->next) ++images;
    for (unsigned i = 0; i < 2; ++i) pending += c->slots[i].occupied != 0;
    out[0] = c->allocated_bytes; out[1] = buffers; out[2] = pipelines; out[3] = images;
    out[4] = c->resident_entries; out[5] = pending; out[6] = c->samplers.count; out[7] = 0; out[8] = c->scene_submissions;
}
static int memory_stats(void *opaque, uint64_t *out, uint32_t count) {
    if (count != MAG_NATIVE_MEMORY_STATS || !out) return MAG_NATIVE_INVALID;
    memory_census(opaque, out); return MAG_NATIVE_OK;
}
/* The same periodic line as Metal, default on (MAG_RENDER_GPU_MEMORY_REPORT=0
 * disables): the first two scene submissions, then every 120th. */
static void memory_report(frontend_vk *c) {
    const uint64_t n = ++c->scene_submissions;
    if (!c->memory_report_every || (n > 2 && n % c->memory_report_every)) return;
    uint64_t m[MAG_NATIVE_MEMORY_STATS]; memory_census(c, m);
    fprintf(stderr, "[mag native gpu memory] submission=%llu allocated_mb=%.1f buffers=%llu pipelines=%llu images=%llu resident=%llu resident_mb=%.1f pending=%llu samplers=%llu retired_alive=%llu backend=vulkan\n",
        (unsigned long long)n, (double)m[0] / 1048576.0, (unsigned long long)m[1], (unsigned long long)m[2],
        (unsigned long long)m[3], (unsigned long long)m[4], (double)c->resident_bytes / 1048576.0, (unsigned long long)m[5],
        (unsigned long long)m[6], (unsigned long long)m[7]);
}

/* Source-bound modules use a literal compute LocalSize. Reject mismatches rather
 * than accepting an unrelated caller-authored workgroup declaration. */
static int local_size_matches(const uint32_t *words, size_t count, const mag_native_pipeline_desc *desc) {
    if (count < 5 || words[0] != 0x07230203 || words[1] > 0x00010300) return 0;
    uint32_t entry = 0;
    for (size_t i = 5; i < count;) {
        uint32_t n = words[i] >> 16, op = words[i] & 0xffff;
        if (!n || n > count - i) return 0;
        if (op == 15 && n >= 4 && words[i + 1] == 5) {
            const char *name = (const char *)&words[i + 3]; size_t bytes = (n - 3) * 4;
            if (desc->entry_point_length < bytes && !memcmp(name, desc->entry_point, desc->entry_point_length) && !name[desc->entry_point_length])
                entry = words[i + 2];
        }
        i += n;
    }
    if (!entry) return 0;
    for (size_t i = 5; i < count; i += words[i] >> 16)
        if ((words[i] & 0xffff) == 16 && (words[i] >> 16) == 6 && words[i + 1] == entry && words[i + 2] == 17)
            return words[i + 3] == desc->threads_per_group[0] && words[i + 4] == desc->threads_per_group[1] && words[i + 5] == desc->threads_per_group[2];
    return 0;
}
static int pipeline_create(void *opaque, const mag_native_pipeline_desc *desc, mag_native_pipeline *out) {
    frontend_vk *c = opaque;
    if (c->failed) return MAG_NATIVE_DEVICE_FAILED;
    if (desc->shader_language != MAG_NATIVE_SPIRV) return MAG_NATIVE_UNSUPPORTED;
    if (!desc->source_bytes || desc->source_length < 20 || desc->source_length > 1024 * 1024 || desc->source_length % 4 ||
        !desc->entry_point || !desc->entry_point_length || desc->entry_point_length > 128 ||
        !desc->bindings || !desc->binding_count || desc->binding_count > 16) return MAG_NATIVE_INVALID;
    uint32_t invocations = 1, used = 0;
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t n = desc->threads_per_group[i];
        if (!n || n > c->owner.properties.limits.maxComputeWorkGroupSize[i] || n > 256 || invocations > 256 / n)
            return MAG_NATIVE_UNSUPPORTED;
        invocations *= n;
    }
    if (invocations > c->owner.properties.limits.maxComputeWorkGroupInvocations) return MAG_NATIVE_UNSUPPORTED;
    VkDescriptorSetLayoutBinding bindings[16] = {{0}};
    for (uint32_t i = 0; i < desc->binding_count; ++i) {
        mag_native_binding_layout b = desc->bindings[i];
        if (b.slot >= 16 || used & (1u << b.slot) || !b.minimum_bytes || b.minimum_bytes > UINT32_MAX || b.minimum_bytes % 4 ||
            b.alignment_bytes < 4 || b.alignment_bytes > 256 || (b.alignment_bytes & (b.alignment_bytes - 1)) ||
            b.binding_kind > MAG_NATIVE_UNIFORM || (b.access != MAG_NATIVE_READ && b.access != MAG_NATIVE_READ_WRITE) ||
            (b.binding_kind == MAG_NATIVE_UNIFORM && (b.access != MAG_NATIVE_READ || b.runtime_array_stride))) return MAG_NATIVE_INVALID;
        if (b.runtime_array_stride && b.minimum_bytes != b.runtime_array_stride) return MAG_NATIVE_UNSUPPORTED;
        if ((b.runtime_array_stride && b.runtime_array_stride % 4) ||
            (!b.runtime_array_stride && b.runtime_size_index != MAG_NATIVE_NO_SIZE_INDEX)) return MAG_NATIVE_INVALID;
        uint64_t limit = b.binding_kind == MAG_NATIVE_UNIFORM ? c->owner.properties.limits.maxUniformBufferRange : c->owner.properties.limits.maxStorageBufferRange;
        if (b.minimum_bytes > limit) return MAG_NATIVE_UNSUPPORTED;
        used |= 1u << b.slot;
        bindings[i] = (VkDescriptorSetLayoutBinding){b.slot, b.binding_kind == MAG_NATIVE_UNIFORM ?
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    }
    uint32_t *source = malloc((size_t)desc->source_length); if (!source) return MAG_NATIVE_ALLOCATION_FAILED;
    memcpy(source, desc->source_bytes, (size_t)desc->source_length);
    if (!local_size_matches(source, (size_t)desc->source_length / 4, desc)) { free(source); return MAG_NATIVE_INVALID; }
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo shader = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)desc->source_length, .pCode = source};
    VkResult result = c->vkCreateShaderModule(c->owner.device, &shader, NULL, &module); free(source);
    if (result != VK_SUCCESS) return MAG_NATIVE_UNSUPPORTED;
    frontend_vk_pipeline *p = calloc(1, sizeof(*p));
    if (!p) { c->vkDestroyShaderModule(c->owner.device, module, NULL); return MAG_NATIVE_ALLOCATION_FAILED; }
    VkDescriptorSetLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = desc->binding_count, .pBindings = bindings};
    VkPipelineLayoutCreateInfo pipeline_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &p->descriptors};
    char entry[129]; memcpy(entry, desc->entry_point, desc->entry_point_length); entry[desc->entry_point_length] = 0;
    VkComputePipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = entry}};
    result = c->vkCreateDescriptorSetLayout(c->owner.device, &layout, NULL, &p->descriptors);
    if (result == VK_SUCCESS) result = c->vkCreatePipelineLayout(c->owner.device, &pipeline_layout, NULL, &p->layout);
    pipeline.layout = p->layout;
    if (result == VK_SUCCESS) result = c->vkCreateComputePipelines(c->owner.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &p->gpu);
    c->vkDestroyShaderModule(c->owner.device, module, NULL);
    if (result != VK_SUCCESS || c->next_pipeline == UINT64_MAX) { pipeline_dispose(c, p); return MAG_NATIVE_UNSUPPORTED; }
    p->id = ++c->next_pipeline; p->public_ref = 1; p->count = desc->binding_count;
    memcpy(p->bindings, desc->bindings, sizeof(*desc->bindings) * p->count);
    p->next = c->pipelines; c->pipelines = p;
    *out = (mag_native_pipeline){c->generation, p->id}; return MAG_NATIVE_OK;
}
static int pipeline_release(void *opaque, mag_native_pipeline ref) {
    frontend_vk *c = opaque; frontend_vk_pipeline *p = pipeline_find(c, ref);
    if (!p) return MAG_NATIVE_STALE;
    p->public_ref = 0; if (!p->references) pipeline_dispose(c, p); return MAG_NATIVE_OK;
}
static int submit_owned(frontend_vk *c, const mag_native_frame *frame, mag_native_ticket *out) {
    if (c->failed || c->next_submission == UINT64_MAX) return MAG_NATIVE_DEVICE_FAILED;
    frontend_vk_slot *slot = NULL;
    for (unsigned i = 0; i < 2; ++i) if (!c->slots[i].occupied) { slot = &c->slots[i]; break; }
    if (!slot) return MAG_NATIVE_BUSY;
    frontend_vk_pipeline *p = pipeline_find(c, frame->pipeline);
    if (!p) return MAG_NATIVE_STALE;
    if (p->graphics || !frame->bindings || frame->binding_count != p->count) return MAG_NATIVE_INVALID;
    for (unsigned i = 0; i < 3; ++i)
        if (!frame->groups[i] || frame->groups[i] > c->owner.properties.limits.maxComputeWorkGroupCount[i]) return MAG_NATIVE_INVALID;
    VkDescriptorBufferInfo buffers[16] = {{0}}; VkWriteDescriptorSet writes[16] = {{0}};
    frontend_vk_buffer *records[16] = {0}; uint32_t write_access[16] = {0}, used = 0;
    for (uint32_t i = 0; i < frame->binding_count; ++i) {
        mag_native_binding b = frame->bindings[i];
        if (b.slot >= 16 || used & (1u << b.slot) || b.reserved) return MAG_NATIVE_INVALID;
        used |= 1u << b.slot;
        const mag_native_binding_layout *schema = NULL;
        for (uint32_t j = 0; j < p->count; ++j) if (p->bindings[j].slot == b.slot) schema = &p->bindings[j];
        if (!schema) return MAG_NATIVE_INVALID;
        frontend_vk_buffer *record = buffer_find(c, b.buffer); if (!record) return MAG_NATIVE_STALE;
        VkDeviceSize alignment = schema->binding_kind == MAG_NATIVE_UNIFORM ? c->owner.properties.limits.minUniformBufferOffsetAlignment : c->owner.properties.limits.minStorageBufferOffsetAlignment;
        uint64_t limit = schema->binding_kind == MAG_NATIVE_UNIFORM ? c->owner.properties.limits.maxUniformBufferRange : c->owner.properties.limits.maxStorageBufferRange;
        if (b.offset % schema->alignment_bytes || (alignment && b.offset % alignment) || b.byte_length < schema->minimum_bytes ||
            b.byte_length > limit || b.offset > record->length || b.byte_length > record->length - b.offset ||
            (schema->runtime_array_stride && b.byte_length % schema->runtime_array_stride)) return MAG_NATIVE_INVALID;
        int write = schema->access == MAG_NATIVE_READ_WRITE;
        if (record->writers || (write && record->readers)) return MAG_NATIVE_BUSY;
        for (uint32_t j = 0; j < i; ++j) if (records[j] == record && (write || write_access[j])) return MAG_NATIVE_INVALID;
        records[i] = record; write_access[i] = (uint32_t)write;
        buffers[i] = (VkDescriptorBufferInfo){record->gpu, b.offset, b.byte_length};
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b.slot,
            .descriptorCount = 1, .descriptorType = schema->binding_kind == MAG_NATIVE_UNIFORM ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffers[i]};
    }
    if (c->vkResetCommandPool(c->owner.device, slot->pool, 0) != VK_SUCCESS ||
        c->vkResetDescriptorPool(c->owner.device, slot->descriptors, 0) != VK_SUCCESS ||
        c->vkResetFences(c->owner.device, 1, &slot->fence) != VK_SUCCESS) return MAG_NATIVE_DEVICE_FAILED;
    VkDescriptorSet set; VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = slot->descriptors, .descriptorSetCount = 1, .pSetLayouts = &p->descriptors};
    if (c->vkAllocateDescriptorSets(c->owner.device, &allocate, &set) != VK_SUCCESS) return MAG_NATIVE_ALLOCATION_FAILED;
    for (uint32_t i = 0; i < p->count; ++i) writes[i].dstSet = set;
    c->vkUpdateDescriptorSets(c->owner.device, p->count, writes, 0, NULL);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (c->vkBeginCommandBuffer(slot->command, &begin) != VK_SUCCESS) return MAG_NATIVE_DEVICE_FAILED;
    VkMemoryBarrier before = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    c->vkCmdPipelineBarrier(slot->command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, NULL, 0, NULL);
    c->vkCmdBindPipeline(slot->command, VK_PIPELINE_BIND_POINT_COMPUTE, p->gpu);
    c->vkCmdBindDescriptorSets(slot->command, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &set, 0, NULL);
    c->vkCmdDispatch(slot->command, frame->groups[0], frame->groups[1], frame->groups[2]);
    VkMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    c->vkCmdPipelineBarrier(slot->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, NULL, 0, NULL);
    if (c->vkEndCommandBuffer(slot->command) != VK_SUCCESS) return MAG_NATIVE_DEVICE_FAILED;
    slot->count = p->count; slot->pipeline = p; ++p->references; slot->occupied = 1;
    for (uint32_t i = 0; i < p->count; ++i) {
        slot->buffers[i] = records[i]; slot->writes[i] = write_access[i];
        if (write_access[i]) {++records[i]->writers;records[i]->gpu_written=1;} else ++records[i]->readers;
    }
    slot->ticket = (mag_native_ticket){c->generation, ++c->next_submission};
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &slot->command};
    VkResult result = c->vkQueueSubmit(c->owner.queue, 1, &submit, slot->fence);
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        slot_release(c, slot); return MAG_NATIVE_ALLOCATION_FAILED;
    }
    *out = slot->ticket;
    if (result != VK_SUCCESS) { c->failed = 1; return MAG_NATIVE_INDETERMINATE; }
    return MAG_NATIVE_OK;
}
static int submit(void *opaque,const mag_native_frame *frame,mag_native_ticket *out) {
    frontend_vk *c=opaque;
    int status=c->owner.host.try_begin(c->owner.host.userdata,c->owner.host.lifetime);
    if (status!=MAG_NATIVE_OK) return status<0?status:MAG_NATIVE_DEVICE_FAILED;
    int result=submit_owned(c,frame,out);
    c->owner.host.end(c->owner.host.userdata,c->owner.host.lifetime);return result;
}
static void timing_report(frontend_vk *c,frontend_vk_slot *slot,VkResult status);
static int poll(void *opaque, mag_native_event *events, uint32_t capacity, uint32_t *count) {
    frontend_vk *c = opaque; int fault = 0;
    for (unsigned i = 0; i < 2; ++i) {
        frontend_vk_slot *slot = &c->slots[i]; if (!slot->occupied) continue;
        VkResult result = c->vkGetFenceStatus(c->owner.device, slot->fence);
        if (result == VK_NOT_READY) continue;
        if (result != VK_SUCCESS) { c->failed = fault = 1; continue; }
        if (*count == capacity) continue;
        events[(*count)++] = (mag_native_event){sizeof(*events), 1, slot->ticket, 1, 1, VK_SUCCESS, 0};
        timing_report(c, slot, result);
        slot_release(c, slot);
    }
    /* Never return an error alongside a consumed once-only valid receipt: the
     * caller would discard it. Deliver genuine terminals, then report the
     * independent fault on a later poll; admission is already blocked. */
    return *count || !fault ? MAG_NATIVE_OK : MAG_NATIVE_DEVICE_FAILED;
}
static void resident_close(frontend_vk *);
static int close_context(void *opaque) {
    frontend_vk *c = opaque;
    if (c->reserved) return MAG_NATIVE_BUSY;
    for (unsigned i=0;i<2;++i) if (c->slots[i].occupied) return MAG_NATIVE_BUSY;
    for (unsigned i = 0; i < 2; ++i) {
        frontend_vk_slot *slot = &c->slots[i]; if (slot->occupied) slot_release(c, slot);
        if (slot->descriptors) c->vkDestroyDescriptorPool(c->owner.device, slot->descriptors, NULL);
        if (slot->pool) c->vkDestroyCommandPool(c->owner.device, slot->pool, NULL);
        if (slot->fence) c->vkDestroyFence(c->owner.device, slot->fence, NULL);
        if (slot->timestamps) c->vkDestroyQueryPool(c->owner.device, slot->timestamps, NULL);
    }
    int sampler_status=mag_native_sampler_registry_close(&c->samplers);if(sampler_status)return sampler_status;
    while (c->buffers) buffer_dispose(c, c->buffers);
    while (c->pipelines) pipeline_dispose(c, c->pipelines);
    while (c->images) imported_image_dispose(c,c->images);
    resident_close(c);
    while (c->families) { frontend_vk_family *old = c->families; c->families = old->next; free(old); }
    MagRenderHostOwner host=c->owner.host;free(c);
    host.release(host.userdata,host.lifetime);return MAG_NATIVE_OK;
}

/* Every refusal inside the scene, pipeline and import paths records where it
 * was decided, and each refused scene entry point reports it on one line
 * (rate limited), so an unsupported game scene is visible rather than a bare
 * status. Plain statics: these entry points run on the one client thread. */
static const char *vk_refusal_file;
static int vk_refusal_line;
static unsigned long long vk_refusal_count;
#define VK_REFUSE(status) (vk_refusal_file = __FILE__, vk_refusal_line = __LINE__, (status))
static void vk_refusal_begin(void) { vk_refusal_file = NULL; vk_refusal_line = 0; }
static int vk_refusal_end(const char *operation, int status) {
    /* BUSY and STALE are ordinary back-pressure and lifetime answers; an
     * unattributed UNSUPPORTED from a reservation means only that this host
     * has no queue order (the commit is then synchronous). */
    if (status >= 0 || status == MAG_NATIVE_BUSY || status == MAG_NATIVE_STALE) return status;
    if (status == MAG_NATIVE_UNSUPPORTED && !vk_refusal_file && !strcmp(operation, "reserve_scene")) return status;
    ++vk_refusal_count;
    if (vk_refusal_count <= 16 || vk_refusal_count % 256 == 0) {
        const char *file = vk_refusal_file ? vk_refusal_file : "unattributed";
        for (const char *p = file; *p; ++p) if (*p == '/' || *p == '\\') file = p + 1;
        fprintf(stderr, "[mag native vulkan refusal] op=%s status=%d at=%s:%d count=%llu\n", operation, status,
                file, vk_refusal_file ? vk_refusal_line : 0, vk_refusal_count);
    }
    return status;
}
#include "mag_render_vulkan_sampler.inc"
#include "mag_render_vulkan_batch.inc"
#include "mag_render_vulkan_import.inc"
#include "mag_render_vulkan_resident.inc"
#include "mag_render_vulkan_scene.inc"
/* The reporting entry points (see vk_refusal_end). */
#define VK_REPORTED(name, params, args) \
    static int reported_##name params { vk_refusal_begin(); return vk_refusal_end(#name, name args); }
VK_REPORTED(scene_graphics_create, (void *o, const mag_native_scene_graphics_desc *d, mag_native_pipeline *p), (o, d, p))
VK_REPORTED(scene_graphics_create_v2, (void *o, const mag_native_scene_graphics_v2_desc *d, mag_native_pipeline *p), (o, d, p))
VK_REPORTED(scene_graphics_create_v3, (void *o, const mag_native_scene_graphics_v3_desc *d, mag_native_pipeline *p), (o, d, p))
VK_REPORTED(submit_scene, (void *o, const mag_native_scene *s, mag_native_ticket *t), (o, s, t))
VK_REPORTED(submit_scene_v2, (void *o, const mag_native_scene_v2 *d, const mag_native_image *i, const MagRenderHostFrame *f, mag_native_ticket *t), (o, d, i, f, t))
VK_REPORTED(submit_scene_v3, (void *o, const mag_native_scene_v3 *d, const mag_native_image *i, const MagRenderHostFrame *f, mag_native_ticket *t), (o, d, i, f, t))
VK_REPORTED(submit_scene_v4, (void *o, const mag_native_scene_v4 *d, const mag_native_image *i, const MagRenderHostFrame *f, mag_native_ticket *t), (o, d, i, f, t))
VK_REPORTED(submit_scene_v5, (void *o, const mag_native_scene_v5 *d, const mag_native_image *i, const MagRenderHostFrame *f, mag_native_ticket *t), (o, d, i, f, t))
VK_REPORTED(reserve_scene, (void *o, const mag_native_scene_import *im, uint32_t n, mag_native_ticket *t), (o, im, n, t))
VK_REPORTED(sampler_create, (void *o, const mag_native_sampler_desc *d, mag_native_sampler *s), (o, d, s))
VK_REPORTED(image_import, (void *o, const MagRenderHostImage *h, mag_native_image *i), (o, h, i))
VK_REPORTED(image_import_certified, (void *o, const mag_native_image_certificate *c, mag_native_image *i), (o, c, i))
/* Designated: every operation this backend lacks is an explicit NULL, which
 * the wrapper reports as UNSUPPORTED (scene_chunk_stats: Metal's chunked
 * encoding only). reserve_scene reports UNSUPPORTED itself unless
 * MAG_RENDER_VULKAN_RESERVED_COMMIT=1 and the host provides a queue order;
 * the reserved-order commit then falls back to the synchronous one. */
static const mag_native_ops operations = {
    .caps = caps, .buffer_create = buffer_create, .buffer_release = buffer_release, .buffer_read = buffer_read,
    .pipeline_create = pipeline_create, .pipeline_release = pipeline_release, .submit = submit, .poll = poll,
    .close = close_context, .graphics_create = graphics_create, .submit_batch = submit_batch,
    .scene_graphics_create = reported_scene_graphics_create, .submit_scene = reported_submit_scene, .image_import = reported_image_import,
    .image_release = image_release, .submit_scene_target = NULL, .submit_scene_frame = NULL,
    .scene_graphics_create_v2 = reported_scene_graphics_create_v2, .scene_graphics_create_v3 = reported_scene_graphics_create_v3,
    .sampler_create = reported_sampler_create, .sampler_release = sampler_release, .submit_scene_v2 = reported_submit_scene_v2,
    .submit_scene_v3 = reported_submit_scene_v3, .submit_scene_v4 = reported_submit_scene_v4,
    .image_import_certified = reported_image_import_certified, .scene_graphics_create_aux = NULL,
    .submit_scene_v5 = reported_submit_scene_v5, .reserve_scene = reported_reserve_scene, .abandon_scene_reservation = abandon_scene_reservation,
    .buffer_write = buffer_write, .scene_chunk_stats = NULL, .memory_stats = memory_stats};
/* Opt-in GPU timing. Unavailable timing is said once, never silently absent. */
static void timing_setup(frontend_vk *c,const MagRenderHostOwner *host){
    const char *timing=getenv("MAG_RENDER_NATIVE_GPU_TIMING");
    if(!timing||strcmp(timing,"1"))return;
    const char *reason=NULL;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties families=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)
        host->get_instance_proc(host->instance,"vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t count=0,bits=0;
    if(families){families((VkPhysicalDevice)host->physical_device,&count,NULL);
        VkQueueFamilyProperties *props=count?calloc(count,sizeof(*props)):NULL;
        if(props){families((VkPhysicalDevice)host->physical_device,&count,props);
            if(host->queue_family<count)bits=props[host->queue_family].timestampValidBits;
            free(props);}}
#define TIMING_LOAD(name) c->name=(PFN_##name)host->get_device_proc(host->device,#name);if(!c->name&&!reason)reason="missing " #name;
    TIMING_LOAD(vkCreateQueryPool) TIMING_LOAD(vkDestroyQueryPool) TIMING_LOAD(vkCmdResetQueryPool)
    TIMING_LOAD(vkCmdWriteTimestamp) TIMING_LOAD(vkGetQueryPoolResults)
#undef TIMING_LOAD
    if(!reason&&!bits)reason="queue family has no timestamps";
    if(!reason&&c->owner.properties.limits.timestampPeriod<=0)reason="no timestamp period";
    VkQueryPoolCreateInfo info={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
    for(unsigned i=0;i<2&&!reason;++i)
        if(c->vkCreateQueryPool(c->owner.device,&info,NULL,&c->slots[i].timestamps)!=VK_SUCCESS)reason="query pool creation failed";
    if(reason){
        for(unsigned i=0;i<2;++i)if(c->slots[i].timestamps){c->vkDestroyQueryPool(c->owner.device,c->slots[i].timestamps,NULL);c->slots[i].timestamps=VK_NULL_HANDLE;}
        fprintf(stderr,"[mag native gpu timing] unavailable backend=vulkan reason=%s\n",reason);return;
    }
    c->timestamp_mask=bits>=64?~0ull:((1ull<<bits)-1);
    c->timestamp_ns=c->owner.properties.limits.timestampPeriod;
    c->timing_enabled=1;
}
/* After a timed slot's fence signalled: one line per submission, the first
 * 256, as the Metal backend reports them (seconds on the device's clock). */
static void timing_report(frontend_vk *c,frontend_vk_slot *slot,VkResult status){
    if(!slot->timed)return;
    slot->timed=0;
    if(c->timing_samples>=256)return;
    ++c->timing_samples;
    uint64_t ticks[2]={0,0};
    const int read=status==VK_SUCCESS&&c->vkGetQueryPoolResults(c->owner.device,slot->timestamps,0,2,sizeof(ticks),ticks,
        sizeof(ticks[0]),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS;
    const double start=read?(double)(ticks[0]&c->timestamp_mask)*c->timestamp_ns*1e-9:0,
                 end=read?(double)(ticks[1]&c->timestamp_mask)*c->timestamp_ns*1e-9:0;
    const int available=read&&end>=start&&start>0;
    fprintf(stderr,"[mag native gpu timing] generation=%llu ticket=%llu status=%u available=%d gpu_start_seconds=%.9f gpu_end_seconds=%.9f gpu_ms=%.6f backend=vulkan\n",
        (unsigned long long)slot->ticket.device_generation,(unsigned long long)slot->ticket.submission_id,(unsigned)status,
        available,start,end,available?(end-start)*1000.0:-1.0);
}
static int vulkan_create(const MagRenderHostOwner *host,uint64_t known,uint64_t enabled,mag_native_device **out) {
    if (!host || !out || host->backend!=MAG_RENDER_HOST_VULKAN) return MAG_NATIVE_INVALID;
    *out=NULL;int status=host->retain(host->userdata,host->lifetime);
    if (status!=MAG_NATIVE_OK) return status<0?status:MAG_NATIVE_DEVICE_FAILED;
    if (!host->device || !host->queue || !host->instance || !host->physical_device || !host->get_instance_proc || !host->get_device_proc) {host->release(host->userdata,host->lifetime);return MAG_NATIVE_INVALID;}
    PFN_vkGetPhysicalDeviceProperties properties=(PFN_vkGetPhysicalDeviceProperties)host->get_instance_proc(host->instance,"vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties memory=(PFN_vkGetPhysicalDeviceMemoryProperties)host->get_instance_proc(host->instance,"vkGetPhysicalDeviceMemoryProperties");
    if (!properties || !memory) {host->release(host->userdata,host->lifetime);return MAG_NATIVE_UNSUPPORTED;}
    frontend_vk *c=calloc(1,sizeof(*c));if (!c) {host->release(host->userdata,host->lifetime);return MAG_NATIVE_ALLOCATION_FAILED;}
    c->known_features=known;c->enabled_features=enabled;
    { const char *memory=getenv("MAG_RENDER_GPU_MEMORY_REPORT"); c->memory_report_every=memory&&!strcmp(memory,"0")?0u:120u; }
    /* Opt-in (MAG_RENDER_VULKAN_RESERVED_COMMIT=1) while it is measured:
     * otherwise reservations stay UNSUPPORTED and the reserved-order commit
     * falls back to the synchronous one, as before. */
    const char *reserved = getenv("MAG_RENDER_VULKAN_RESERVED_COMMIT");
    if (reserved && !strcmp(reserved, "1") && mag_render_host_vulkan_queue_order) {
        c->order = (MagRenderVulkanQueueOrder){.abi_version = MAG_RENDER_HOST_QUEUE_ORDER_ABI, .struct_size = sizeof(c->order)};
        c->order_ok = mag_render_host_vulkan_queue_order(&c->order) == 0 && c->order.lifetime == host->lifetime &&
                      c->order.reserve && c->order.submit;
    }
    { const char *resident=getenv("MAG_RENDER_VULKAN_RESIDENT_IMAGES"); c->resident_enabled=resident&&!strcmp(resident,"1");
      const char *report=getenv("MAG_RENDER_FIFO_TIMING"); c->resident_report=report&&!strcmp(report,"1"); }
    c->owner.host=*host;c->owner.device=(VkDevice)host->device;c->owner.queue=(VkQueue)host->queue;c->owner.queue_family=host->queue_family;
    properties((VkPhysicalDevice)host->physical_device,&c->owner.properties);memory((VkPhysicalDevice)host->physical_device,&c->owner.memory);
    if (c->owner.properties.apiVersion<VK_API_VERSION_1_1) {close_context(c);return MAG_NATIVE_UNSUPPORTED;}
#define LOAD(name) c->name=(PFN_##name)host->get_device_proc(host->device,#name);if(!c->name) {close_context(c);return MAG_NATIVE_UNSUPPORTED;}
    FRONTEND_VK_FUNCTIONS(LOAD)
#undef LOAD
    c->generation=mag_native_next_generation();if(!c->generation) {close_context(c);return MAG_NATIVE_ALLOCATION_FAILED;}
    mag_native_sampler_registry_init(&c->samplers,c->generation,c,sampler_payload_destroy);
    for (unsigned i=0;i<2;++i) {
        frontend_vk_slot *slot=&c->slots[i];
        VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=host->queue_family};
        VkCommandBufferAllocateInfo command={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkDescriptorPoolSize sizes[4]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2048},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2048},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1024},{VK_DESCRIPTOR_TYPE_SAMPLER,1024}};
        VkDescriptorPoolCreateInfo descriptors={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=160,.poolSizeCount=4,.pPoolSizes=sizes};
        VkResult result=c->vkCreateCommandPool(c->owner.device,&pool,NULL,&slot->pool);command.commandPool=slot->pool;
        if(result==VK_SUCCESS) result=c->vkAllocateCommandBuffers(c->owner.device,&command,&slot->command);
        if(result==VK_SUCCESS) result=c->vkCreateFence(c->owner.device,&fence,NULL,&slot->fence);
        if(result==VK_SUCCESS) result=c->vkCreateDescriptorPool(c->owner.device,&descriptors,NULL,&slot->descriptors);
        if(result==VK_SUCCESS){slot->descriptor_sets=160;slot->descriptor_counts[0]=2048;slot->descriptor_counts[1]=2048;slot->descriptor_counts[2]=1024;slot->descriptor_counts[3]=1024;}
        if(result!=VK_SUCCESS) {close_context(c);return MAG_NATIVE_ALLOCATION_FAILED;}
    }
    timing_setup(c,host);
    *out=mag_native_wrap(&operations,c);if(!*out) {close_context(c);return MAG_NATIVE_ALLOCATION_FAILED;}
    return MAG_NATIVE_OK;
}

int mag_native_vulkan_create(const MagRenderHostOwner *host,mag_native_device **out){
    return vulkan_create(host,0,0,out);
}
int mag_native_vulkan_create_v2(const MagRenderHostOwnerV2 *host,mag_native_device **out){
    if(!host||!out||host->abi_version!=2||host->struct_size!=sizeof(*host)||
       (host->known_features&~MAG_RENDER_HOST_FEATURE_ALL)||
       (host->enabled_features&~host->known_features))return MAG_NATIVE_INVALID;
    return vulkan_create(&host->base,host->known_features,host->enabled_features,out);
}
