/* Extracted from ps3recomp libs/video/rsx_frontend_vulkan.c at 6e0fcc2e026cddc02fb94f23050ac8429a03306a.
 * Copyright (c) 2026 Ned Heller / sp00nznet. MIT; see LICENSE.ps3recomp.
 * Frontend-owned implementation; no SDK runtime/header dependency. */
#include "mag_render_vulkan.h"
#include "mag_render_native_internal.h"
#include <stdlib.h>
#include <math.h>
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
    X(vkCreateSampler) X(vkDestroySampler) X(vkCmdCopyBufferToImage)

typedef struct frontend_vk_family {
    uint64_t id, last_version; uint32_t public_versions; struct frontend_vk_family *next;
} frontend_vk_family;
typedef struct frontend_vk_buffer {
    uint64_t id, version, length; uint32_t public_ref, readers, writers; int seed_zero,gpu_written;
    VkBuffer gpu; VkDeviceMemory memory; void *mapped; struct frontend_vk_buffer *next;
} frontend_vk_buffer;
typedef struct frontend_vk_scene_image {VkImage image;VkDeviceMemory memory;VkImageView view;VkImageLayout layout;VkImageAspectFlags aspect;} frontend_vk_scene_image;
typedef struct frontend_vk_pipeline {
    uint64_t id; uint32_t public_ref, references, count; int graphics;VkRenderPass render_pass;
    mag_native_binding_layout bindings[16];
    VkDescriptorSetLayout descriptors; VkPipelineLayout layout; VkPipeline gpu;
    int scene;mag_native_scene_graphics_desc scene_desc;
    mag_native_scene_buffer_layout scene_buffers[32];mag_native_scene_texture_layout scene_textures[32];VkDescriptorSetLayout scene_sets[2];
    struct frontend_vk_pipeline *next;
} frontend_vk_pipeline;
typedef struct frontend_vk_slot {
    VkCommandPool pool; VkCommandBuffer command; VkFence fence; VkDescriptorPool descriptors;
    uint32_t descriptor_sets,descriptor_counts[4];
    mag_native_ticket ticket; uint32_t occupied, count;
    frontend_vk_buffer *buffers[64]; uint32_t writes[64]; frontend_vk_pipeline *pipeline;
    frontend_vk_pipeline *batch_pipelines[MAG_NATIVE_BATCH_MAX_COMMANDS + MAG_NATIVE_SCENE_MAX_DRAWS];uint32_t batch_count;
    frontend_vk_scene_image scene_images[16];uint32_t scene_image_count,scene_pass_count;
    VkFramebuffer scene_framebuffers[16];VkRenderPass scene_renderpasses[16];VkSampler scene_samplers[3];
    VkImage color,depth;VkDeviceMemory color_memory,depth_memory;
    VkImageView color_view,depth_view;VkFramebuffer framebuffer;
} frontend_vk_slot;
typedef struct frontend_vk {
    mag_native_vulkan_owner owner;
    uint64_t generation, next_buffer, next_pipeline, next_submission;
    int failed;
    frontend_vk_family *families; frontend_vk_buffer *buffers; frontend_vk_pipeline *pipelines;
    frontend_vk_slot slots[2];
#define DECLARE(name) PFN_##name name;
    FRONTEND_VK_FUNCTIONS(DECLARE)
#undef DECLARE
} frontend_vk;

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
    if (b->memory) c->vkFreeMemory(c->owner.device, b->memory, NULL);
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
    if(slot->color_memory)c->vkFreeMemory(c->owner.device,slot->color_memory,NULL);
    if(slot->depth_memory)c->vkFreeMemory(c->owner.device,slot->depth_memory,NULL);
    slot->framebuffer=VK_NULL_HANDLE;slot->color_view=slot->depth_view=VK_NULL_HANDLE;
    slot->color=slot->depth=VK_NULL_HANDLE;slot->color_memory=slot->depth_memory=VK_NULL_HANDLE;
}

static void scene_attachments_release(frontend_vk *c,frontend_vk_slot *slot){
 for(uint32_t i=0;i<slot->scene_pass_count;++i){if(slot->scene_framebuffers[i])c->vkDestroyFramebuffer(c->owner.device,slot->scene_framebuffers[i],NULL);if(slot->scene_renderpasses[i])c->vkDestroyRenderPass(c->owner.device,slot->scene_renderpasses[i],NULL);slot->scene_framebuffers[i]=VK_NULL_HANDLE;slot->scene_renderpasses[i]=VK_NULL_HANDLE;}slot->scene_pass_count=0;
 for(uint32_t i=0;i<slot->scene_image_count;++i){frontend_vk_scene_image *im=&slot->scene_images[i];if(im->view)c->vkDestroyImageView(c->owner.device,im->view,NULL);if(im->image)c->vkDestroyImage(c->owner.device,im->image,NULL);if(im->memory)c->vkFreeMemory(c->owner.device,im->memory,NULL);memset(im,0,sizeof(*im));}slot->scene_image_count=0;
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
    if (c->vkAllocateMemory(c->owner.device, &allocation, NULL, &b->memory) != VK_SUCCESS ||
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
static int poll(void *opaque, mag_native_event *events, uint32_t capacity, uint32_t *count) {
    frontend_vk *c = opaque; int fault = 0;
    for (unsigned i = 0; i < 2; ++i) {
        frontend_vk_slot *slot = &c->slots[i]; if (!slot->occupied) continue;
        VkResult result = c->vkGetFenceStatus(c->owner.device, slot->fence);
        if (result == VK_NOT_READY) continue;
        if (result != VK_SUCCESS) { c->failed = fault = 1; continue; }
        if (*count == capacity) continue;
        events[(*count)++] = (mag_native_event){sizeof(*events), 1, slot->ticket, 1, 1, VK_SUCCESS, 0};
        slot_release(c, slot);
    }
    /* Never return an error alongside a consumed once-only valid receipt: the
     * caller would discard it. Deliver genuine terminals, then report the
     * independent fault on a later poll; admission is already blocked. */
    return *count || !fault ? MAG_NATIVE_OK : MAG_NATIVE_DEVICE_FAILED;
}
static int close_context(void *opaque) {
    frontend_vk *c = opaque;
    for (unsigned i=0;i<2;++i) if (c->slots[i].occupied) return MAG_NATIVE_BUSY;
    for (unsigned i = 0; i < 2; ++i) {
        frontend_vk_slot *slot = &c->slots[i]; if (slot->occupied) slot_release(c, slot);
        if (slot->descriptors) c->vkDestroyDescriptorPool(c->owner.device, slot->descriptors, NULL);
        if (slot->pool) c->vkDestroyCommandPool(c->owner.device, slot->pool, NULL);
        if (slot->fence) c->vkDestroyFence(c->owner.device, slot->fence, NULL);
    }
    while (c->buffers) buffer_dispose(c, c->buffers);
    while (c->pipelines) pipeline_dispose(c, c->pipelines);
    while (c->families) { frontend_vk_family *old = c->families; c->families = old->next; free(old); }
    MagRenderHostOwner host=c->owner.host;free(c);
    host.release(host.userdata,host.lifetime);return MAG_NATIVE_OK;
}

#include "mag_render_vulkan_batch.inc"
#include "mag_render_vulkan_scene.inc"
static const mag_native_ops operations = {caps, buffer_create, buffer_release, buffer_read,
    pipeline_create, pipeline_release, submit, poll, close_context, graphics_create, submit_batch, scene_graphics_create, submit_scene, NULL, NULL, NULL, NULL, scene_graphics_create_v2};
int mag_native_vulkan_create(const MagRenderHostOwner *host,mag_native_device **out) {
    if (!host || !out || host->backend!=MAG_RENDER_HOST_VULKAN) return MAG_NATIVE_INVALID;
    *out=NULL;int status=host->retain(host->userdata,host->lifetime);
    if (status!=MAG_NATIVE_OK) return status<0?status:MAG_NATIVE_DEVICE_FAILED;
    if (!host->device || !host->queue || !host->instance || !host->physical_device || !host->get_instance_proc || !host->get_device_proc) {host->release(host->userdata,host->lifetime);return MAG_NATIVE_INVALID;}
    PFN_vkGetPhysicalDeviceProperties properties=(PFN_vkGetPhysicalDeviceProperties)host->get_instance_proc(host->instance,"vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties memory=(PFN_vkGetPhysicalDeviceMemoryProperties)host->get_instance_proc(host->instance,"vkGetPhysicalDeviceMemoryProperties");
    if (!properties || !memory) {host->release(host->userdata,host->lifetime);return MAG_NATIVE_UNSUPPORTED;}
    frontend_vk *c=calloc(1,sizeof(*c));if (!c) {host->release(host->userdata,host->lifetime);return MAG_NATIVE_ALLOCATION_FAILED;}
    c->owner.host=*host;c->owner.device=(VkDevice)host->device;c->owner.queue=(VkQueue)host->queue;c->owner.queue_family=host->queue_family;
    properties((VkPhysicalDevice)host->physical_device,&c->owner.properties);memory((VkPhysicalDevice)host->physical_device,&c->owner.memory);
    if (c->owner.properties.apiVersion<VK_API_VERSION_1_1) {close_context(c);return MAG_NATIVE_UNSUPPORTED;}
#define LOAD(name) c->name=(PFN_##name)host->get_device_proc(host->device,#name);if(!c->name) {close_context(c);return MAG_NATIVE_UNSUPPORTED;}
    FRONTEND_VK_FUNCTIONS(LOAD)
#undef LOAD
    c->generation=mag_native_next_generation();if(!c->generation) {close_context(c);return MAG_NATIVE_ALLOCATION_FAILED;}
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
    *out=mag_native_wrap(&operations,c);if(!*out) {close_context(c);return MAG_NATIVE_ALLOCATION_FAILED;}
    return MAG_NATIVE_OK;
}
