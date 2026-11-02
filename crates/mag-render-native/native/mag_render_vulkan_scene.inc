/* Frontend-owned ordered multipass Vulkan scene on the genuine borrowed queue. */
#include "mag_render_scene_validate.h"
static VkFormat scene_vk_format(uint32_t format) {
    return format == MAG_NATIVE_SCENE_D32     ? VK_FORMAT_D32_SFLOAT
           : format == MAG_NATIVE_SCENE_BGRA8 ? VK_FORMAT_B8G8R8A8_UNORM
                                              : VK_FORMAT_R8G8B8A8_UNORM;
}
static int scene_renderpass(frontend_vk *c, uint32_t color, uint32_t depth, const mag_native_scene_pass *pass,
                            VkRenderPass *out) {
    VkAttachmentDescription attachments[2];
    VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                          depth_ref = {color ? 1u : 0u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    uint32_t count = 0;
    if (color)
        attachments[count++] = (VkAttachmentDescription){
            .format = scene_vk_format(color),
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = pass && pass->color.load == MAG_NATIVE_SCENE_LOAD ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                                        : VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = pass && pass->color.store == MAG_NATIVE_SCENE_DISCARD
                           ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                           : VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    if (depth)
        attachments[count++] = (VkAttachmentDescription){
            .format = VK_FORMAT_D32_SFLOAT,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = pass && pass->depth.load == MAG_NATIVE_SCENE_LOAD ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                                        : VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = pass && pass->depth.store == MAG_NATIVE_SCENE_DISCARD
                           ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                           : VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = color ? 1u : 0u,
                                .pColorAttachments = color ? &color_ref : NULL,
                                .pDepthStencilAttachment = depth ? &depth_ref : NULL};
    VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = count,
                                   .pAttachments = attachments,
                                   .subpassCount = 1,
                                   .pSubpasses = &sub};
    return c->vkCreateRenderPass(c->owner.device, &info, NULL, out) == VK_SUCCESS
               ? MAG_NATIVE_OK
               : MAG_NATIVE_ALLOCATION_FAILED;
}
static int scene_graphics_create_layout(void *opaque, const mag_native_scene_graphics_desc *desc,
                                        const mag_native_vertex_attribute *attributes,
                                        uint32_t attribute_count, uint32_t binding_model,
                                        mag_native_pipeline *out) {
    frontend_vk *c = opaque;
    if (c->failed)
        return MAG_NATIVE_DEVICE_FAILED;
    PFN_vkGetPhysicalDeviceFormatProperties format_properties =
        (PFN_vkGetPhysicalDeviceFormatProperties)c->owner.host.get_instance_proc(
            c->owner.host.instance, "vkGetPhysicalDeviceFormatProperties");
    if (!format_properties)
        return MAG_NATIVE_UNSUPPORTED;
    VkFormatProperties color_format, depth_format;
    format_properties((VkPhysicalDevice)c->owner.host.physical_device, scene_vk_format(desc->color_format),
                      &color_format);
    format_properties((VkPhysicalDevice)c->owner.host.physical_device, VK_FORMAT_D32_SFLOAT, &depth_format);
    if (!(color_format.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
        !(depth_format.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        return MAG_NATIVE_UNSUPPORTED;
    const mag_native_shader_stage *source[2] = {&desc->vertex, &desc->fragment};
    VkShaderModule modules[2] = {0};
    char entries[2][129];
    int status = MAG_NATIVE_UNSUPPORTED;
    frontend_vk_pipeline *p = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        const mag_native_shader_stage *s = source[i];
        if (i == 1 && !s->source_length)
            continue;
        if (s->language != MAG_NATIVE_SPIRV)
            goto done;
        if (!s->source || s->source_length < 20 || s->source_length > 1024 * 1024 || s->source_length % 4 ||
            !s->entry || !s->entry_length || s->entry_length > 128) {
            status = MAG_NATIVE_INVALID;
            goto done;
        }
        uint32_t header[2];
        memcpy(header, s->source, sizeof(header));
        if (header[0] != 0x07230203 || header[1] > 0x00010300)
            goto done;
        memcpy(entries[i], s->entry, s->entry_length);
        entries[i][s->entry_length] = 0;
        uint32_t *code = malloc((size_t)s->source_length);
        if (!code) {
            status = MAG_NATIVE_ALLOCATION_FAILED;
            goto done;
        }
        memcpy(code, s->source, (size_t)s->source_length);
        VkShaderModuleCreateInfo module = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                           .codeSize = (size_t)s->source_length,
                                           .pCode = code};
        VkResult compiled = c->vkCreateShaderModule(c->owner.device, &module, NULL, &modules[i]);
        free(code);
        if (compiled != VK_SUCCESS)
            goto done;
    }
    p = calloc(1, sizeof(*p));
    if (!p) {
        status = MAG_NATIVE_ALLOCATION_FAILED;
        goto done;
    }
    status = scene_renderpass(c, desc->color_format, desc->depth_format, NULL, &p->render_pass);
    if (status)
        goto done;
    status = MAG_NATIVE_UNSUPPORTED;
    uint32_t total_resources[4] = {0};
    for (unsigned stage = 0; stage < 2; ++stage) {
        VkDescriptorSetLayoutBinding bindings[48];
        uint32_t count = 0, samplers_seen = 0;
        for (uint32_t i = 0; i < desc->buffer_count; ++i)
            if (desc->buffers[i].stage == stage) {
                const mag_native_binding_layout *b = &desc->buffers[i].binding;
                bindings[count++] = (VkDescriptorSetLayoutBinding){
                    b->slot,
                    b->binding_kind == MAG_NATIVE_UNIFORM ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, stage ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT, NULL};
            }
        for (uint32_t i = 0; i < desc->texture_count; ++i)
            if (desc->textures[i].stage == stage) {
                const mag_native_scene_texture_layout *t = &desc->textures[i];
                bindings[count++] = (VkDescriptorSetLayoutBinding){
                    (binding_model ? 16u : 0u) + t->texture_slot, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                    stage ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT, NULL};

                if (!(samplers_seen & (1u << t->sampler_slot))) {
                    bindings[count++] = (VkDescriptorSetLayoutBinding){
                        (binding_model ? 32u : 0u) + t->sampler_slot, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                        stage ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT, NULL};
                    samplers_seen |= 1u << t->sampler_slot;
                }
            }
        uint32_t resources[4] = {0};
        for (uint32_t i = 0; i < count; ++i) {
            switch (bindings[i].descriptorType) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                ++resources[0];
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                ++resources[1];
                break;
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                ++resources[2];
                break;
            case VK_DESCRIPTOR_TYPE_SAMPLER:
                ++resources[3];
                break;
            default:
                goto done;
            }
        }
        const VkPhysicalDeviceLimits *limits = &c->owner.properties.limits;
        if (resources[0] > limits->maxPerStageDescriptorUniformBuffers ||
            resources[1] > limits->maxPerStageDescriptorStorageBuffers ||
            resources[2] > limits->maxPerStageDescriptorSampledImages ||
            resources[3] > limits->maxPerStageDescriptorSamplers || count > limits->maxPerStageResources)
            goto done;
        for (unsigned i = 0; i < 4; ++i)
            total_resources[i] += resources[i];
        if (total_resources[0] > limits->maxDescriptorSetUniformBuffers ||
            total_resources[1] > limits->maxDescriptorSetStorageBuffers ||
            total_resources[2] > limits->maxDescriptorSetSampledImages ||
            total_resources[3] > limits->maxDescriptorSetSamplers)
            goto done;
        VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .bindingCount = count,
                                                .pBindings = bindings};
        if (c->vkCreateDescriptorSetLayout(c->owner.device, &info, NULL, &p->scene_sets[stage]) != VK_SUCCESS)
            goto done;
    }
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 2,
                                         .pSetLayouts = p->scene_sets};
    if (c->vkCreatePipelineLayout(c->owner.device, &layout, NULL, &p->layout) != VK_SUCCESS)
        goto done;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = modules[0],
         .pName = entries[0]},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = modules[1],
         .pName = entries[1]}};
    VkVertexInputBindingDescription binding = {0, desc->vertex_stride, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkFormat formats[] = {VK_FORMAT_UNDEFINED,           VK_FORMAT_R32_SFLOAT,
                                VK_FORMAT_R32G32_SFLOAT,       VK_FORMAT_R32G32B32_SFLOAT,
                                VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM};
    VkVertexInputAttributeDescription native_attributes[16];
    for (uint32_t i = 0; i < attribute_count; ++i)
        native_attributes[i] = (VkVertexInputAttributeDescription){
            attributes[i].location, 0, formats[attributes[i].format], attributes[i].offset};
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = attribute_count,
        .pVertexAttributeDescriptions = native_attributes};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {.sType =
                                                      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1,
                                                  .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depth_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc->depth_compare != MAG_NATIVE_SCENE_DEPTH_DISABLED,
        .depthWriteEnable = desc->depth_write,
        .depthCompareOp = desc->depth_compare == MAG_NATIVE_SCENE_DEPTH_LESS     ? VK_COMPARE_OP_LESS
                          : desc->depth_compare == MAG_NATIVE_SCENE_DEPTH_LEQUAL ? VK_COMPARE_OP_LESS_OR_EQUAL
                                                                                 : VK_COMPARE_OP_ALWAYS};
    VkPipelineColorBlendAttachmentState blend = {.colorWriteMask =
                                                     VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    blend.blendEnable = desc->blend == MAG_NATIVE_SCENE_ALPHA;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = desc->color_format ? 1u : 0u,
        .pAttachments = &blend};
    VkDynamicState states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = 2,
                                                .pDynamicStates = states};
    VkGraphicsPipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                             .stageCount = desc->fragment.source_length ? 2u : 1u,
                                             .pStages = stages,
                                             .pVertexInputState = &vertex,
                                             .pInputAssemblyState = &assembly,
                                             .pViewportState = &viewport,
                                             .pRasterizationState = &raster,
                                             .pMultisampleState = &samples,
                                             .pDepthStencilState = &depth_state,
                                             .pColorBlendState = &color_blend,
                                             .pDynamicState = &dynamic,
                                             .layout = p->layout,
                                             .renderPass = p->render_pass};
    if (c->vkCreateGraphicsPipelines(c->owner.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &p->gpu) !=
            VK_SUCCESS ||
        c->next_pipeline == UINT64_MAX)
        goto done;
    p->id = ++c->next_pipeline;
    p->public_ref = 1;
    p->graphics = 1;
    p->scene = 1;
    p->scene_binding_model = binding_model;
    p->scene_desc = *desc;
    if (desc->buffer_count)
        memcpy(p->scene_buffers, desc->buffers, desc->buffer_count * sizeof(*desc->buffers));
    if (desc->texture_count)
        memcpy(p->scene_textures, desc->textures, desc->texture_count * sizeof(*desc->textures));
    p->scene_desc.buffers = p->scene_buffers;
    p->scene_desc.textures = p->scene_textures;
    p->scene_desc.vertex.source = NULL;
    p->scene_desc.vertex.entry = NULL;
    p->scene_desc.fragment.source = NULL;
    p->scene_desc.fragment.entry = NULL;
    p->next = c->pipelines;
    c->pipelines = p;
    *out = (mag_native_pipeline){c->generation, p->id};
    p = NULL;
    status = MAG_NATIVE_OK;
done:
    for (unsigned i = 0; i < 2; ++i)
        if (modules[i])
            c->vkDestroyShaderModule(c->owner.device, modules[i], NULL);
    if (p)
        pipeline_dispose(c, p);
    return status;
}
static int scene_graphics_create(void *opaque, const mag_native_scene_graphics_desc *desc,
                                 mag_native_pipeline *out) {
    int status = mag_native_scene_graphics_validate(desc);
    if (status)
        return status;
    const mag_native_vertex_attribute attributes[] = {{0, MAG_NATIVE_VERTEX_FLOAT4, 0, 0}};
    return scene_graphics_create_layout(opaque, desc, attributes, 1, 0, out);
}
static int scene_graphics_create_v2(void *opaque, const mag_native_scene_graphics_v2_desc *desc,
                                    mag_native_pipeline *out) {
    int status = mag_native_scene_graphics_v2_validate(desc);
    if (status)
        return status;
    return scene_graphics_create_layout(opaque, &desc->base, desc->attributes, desc->attribute_count,
                                        desc->binding_model, out);
}
static int scene_buffer_info(void *opaque, mag_native_buffer ref, mag_native_scene_buffer_info *out) {
    frontend_vk_buffer *b = buffer_find(opaque, ref);
    if (!b)
        return MAG_NATIVE_STALE;
    *out = (mag_native_scene_buffer_info){b->length, b->readers || b->writers, b->gpu_written, b->seed_zero};
    return MAG_NATIVE_OK;
}
static int scene_compute_info(void *opaque, mag_native_pipeline ref, mag_native_scene_compute_info *out) {
    frontend_vk_pipeline *p = pipeline_find(opaque, ref);
    if (!p)
        return MAG_NATIVE_STALE;
    if (p->graphics)
        return MAG_NATIVE_INVALID;
    *out = (mag_native_scene_compute_info){p->bindings, p->count, {1, 1, 1}};
    return MAG_NATIVE_OK;
}
static int scene_graphics_info(void *opaque, mag_native_pipeline ref,
                               const mag_native_scene_graphics_desc **out, uint32_t *model) {
    frontend_vk_pipeline *p = pipeline_find(opaque, ref);
    if (!p)
        return MAG_NATIVE_STALE;
    if (!p->scene)
        return MAG_NATIVE_INVALID;
    *out = &p->scene_desc;
    *model = p->scene_binding_model;
    return MAG_NATIVE_OK;
}
static void scene_image_transition(frontend_vk *c, frontend_vk_slot *slot, uint32_t index,
                                   VkImageLayout layout, VkAccessFlags access, VkPipelineStageFlags stages) {
    frontend_vk_scene_image *im = &slot->scene_images[index];
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = im->layout == VK_IMAGE_LAYOUT_UNDEFINED
                                                   ? 0
                                                   : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask = access,
                              .oldLayout = im->layout,
                              .newLayout = layout,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = im->image,
                              .subresourceRange = {im->aspect, 0, 1, 0, 1}};
    c->vkCmdPipelineBarrier(slot->command,
                            im->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                                    : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            stages, 0, 0, NULL, 0, NULL, 1, &b);
    im->layout = layout;
}
static int scene_descriptors(frontend_vk *c, frontend_vk_slot *slot, frontend_vk_pipeline *pipeline,
                             const mag_native_scene_draw *draw, VkDescriptorSet sets[2]) {
    VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = slot->descriptors,
                                         .descriptorSetCount = 2,
                                         .pSetLayouts = pipeline->scene_sets};
    if (c->vkAllocateDescriptorSets(c->owner.device, &alloc, sets) != VK_SUCCESS)
        return MAG_NATIVE_ALLOCATION_FAILED;
    VkWriteDescriptorSet writes[96];
    VkDescriptorBufferInfo buffers[32];
    VkDescriptorImageInfo images[64];
    uint32_t count = 0, image_count = 0, samplers[2] = {0};
    for (uint32_t j = 0; j < draw->buffer_count; ++j) {
        const mag_native_scene_buffer_binding *b = &draw->buffers[j];
        frontend_vk_buffer *buffer = buffer_find(c, b->binding.buffer);
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        for (uint32_t k = 0; k < pipeline->scene_desc.buffer_count; ++k)
            if (pipeline->scene_buffers[k].stage == b->stage &&
                pipeline->scene_buffers[k].binding.slot == b->binding.slot &&
                pipeline->scene_buffers[k].binding.binding_kind == MAG_NATIVE_UNIFORM)
                type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        buffers[j] = (VkDescriptorBufferInfo){buffer->gpu, b->binding.offset, b->binding.byte_length};
        writes[count++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                 .dstSet = sets[b->stage],
                                                 .dstBinding = b->binding.slot,
                                                 .descriptorCount = 1,
                                                 .descriptorType = type,
                                                 .pBufferInfo = &buffers[j]};
    }
    for (uint32_t j = 0; j < draw->texture_count; ++j) {
        const mag_native_scene_texture_binding *t = &draw->textures[j];
        frontend_vk_scene_image *im = &slot->scene_images[t->image];
        images[image_count] = (VkDescriptorImageInfo){VK_NULL_HANDLE, im->view,
                                                      im->aspect == VK_IMAGE_ASPECT_DEPTH_BIT
                                                          ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[count++] =
            (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = sets[t->stage],
                                   .dstBinding = (pipeline->scene_binding_model ? 16u : 0u) + t->texture_slot,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                   .pImageInfo = &images[image_count++]};
        if (!(samplers[t->stage] & (1u << t->sampler_slot))) {
            samplers[t->stage] |= 1u << t->sampler_slot;
            images[image_count] = (VkDescriptorImageInfo){slot->scene_samplers[t->sampler], VK_NULL_HANDLE,
                                                          VK_IMAGE_LAYOUT_UNDEFINED};
            writes[count++] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = sets[t->stage],
                .dstBinding = (pipeline->scene_binding_model ? 32u : 0u) + t->sampler_slot,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .pImageInfo = &images[image_count++]};
        }
    }
    c->vkUpdateDescriptorSets(c->owner.device, count, writes, 0, NULL);
    return MAG_NATIVE_OK;
}
/* Grow an idle slot to the admitted complete scene, never a fixture draw cap.
 * Capacities are bounded by MAX_DRAWS and all declared per-stage binding caps.
 * Existing pool remains intact if reservation fails. */
static int scene_descriptor_capacity(frontend_vk *c, frontend_vk_slot *slot, const mag_native_scene *scene) {
    uint32_t counts[4] = {512, 512, 1, 1}, sets = 32;
    for (uint32_t i = 0; i < scene->dispatch_count; ++i) {
        frontend_vk_pipeline *p = pipeline_find(c, scene->dispatches[i].pipeline);
        for (uint32_t j = 0; j < p->count; ++j)
            ++counts[p->bindings[j].binding_kind == MAG_NATIVE_UNIFORM ? 1 : 0];
    }
    for (uint32_t i = 0; i < scene->pass_count; ++i)
        for (uint32_t j = 0; j < scene->passes[i].draw_count; ++j) {
            const mag_native_scene_graphics_desc *p =
                &pipeline_find(c, scene->passes[i].draws[j].pipeline)->scene_desc;
            sets += 2;
            for (uint32_t k = 0; k < p->buffer_count; ++k)
                ++counts[p->buffers[k].binding.binding_kind == MAG_NATIVE_UNIFORM ? 1 : 0];
            uint32_t samplers[2] = {0};
            for (uint32_t k = 0; k < p->texture_count; ++k) {
                const mag_native_scene_texture_layout *t = &p->textures[k];
                ++counts[2];
                if (!(samplers[t->stage] & (1u << t->sampler_slot))) {
                    ++counts[3];
                    samplers[t->stage] |= 1u << t->sampler_slot;
                }
            }
        }
    int enough = sets <= slot->descriptor_sets;
    for (unsigned i = 0; i < 4; ++i)
        if (counts[i] > slot->descriptor_counts[i])
            enough = 0;
    if (enough)
        return MAG_NATIVE_OK;
    for (unsigned i = 0; i < 4; ++i)
        if (counts[i] < slot->descriptor_counts[i])
            counts[i] = slot->descriptor_counts[i];
    if (sets < slot->descriptor_sets)
        sets = slot->descriptor_sets;
    VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, counts[0]},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, counts[1]},
                                     {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, counts[2]},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, counts[3]}};
    VkDescriptorPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = sets,
                                       .poolSizeCount = 4,
                                       .pPoolSizes = sizes};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (c->vkCreateDescriptorPool(c->owner.device, &info, NULL, &pool) != VK_SUCCESS)
        return MAG_NATIVE_ALLOCATION_FAILED;
    c->vkDestroyDescriptorPool(c->owner.device, slot->descriptors, NULL);
    slot->descriptors = pool;
    slot->descriptor_sets = sets;
    memcpy(slot->descriptor_counts, counts, sizeof(counts));
    return MAG_NATIVE_OK;
}
static int submit_scene_owned(frontend_vk *c, const mag_native_scene *scene, mag_native_ticket *out) {
    if (c->failed || c->next_submission == UINT64_MAX)
        return MAG_NATIVE_DEVICE_FAILED;
    frontend_vk_slot *slot = NULL;
    for (unsigned i = 0; i < 2; ++i)
        if (!c->slots[i].occupied) {
            slot = &c->slots[i];
            break;
        }
    if (!slot)
        return MAG_NATIVE_BUSY;
    const VkPhysicalDeviceLimits *limits = &c->owner.properties.limits;
    mag_native_scene_registry registry = {c,
                                          scene_buffer_info,
                                          scene_compute_info,
                                          scene_graphics_info,
                                          limits->minUniformBufferOffsetAlignment,
                                          limits->minStorageBufferOffsetAlignment,
                                          limits->maxUniformBufferRange,
                                          limits->maxStorageBufferRange};
    mag_native_scene_usage usage;
    int status = mag_native_scene_validate(scene, &registry, &usage);
    if (status)
        return status;
    uint32_t total_draws = 0;
    for (uint32_t i = 0; i < scene->pass_count; ++i)
        total_draws += scene->passes[i].draw_count;
    VkDescriptorSet(*draw_sets)[2] = total_draws ? calloc(total_draws, sizeof(*draw_sets)) : NULL;
    if (total_draws && !draw_sets)
        return MAG_NATIVE_ALLOCATION_FAILED;
    frontend_vk_pipeline **used = slot->batch_pipelines;
    uint32_t used_count = 0, draw_count = 0;
    status = scene_descriptor_capacity(c, slot, scene);
    if (status) {
        free(draw_sets);
        return status;
    }
    // Reserve every image, framebuffer, sampler and descriptor set before commit.
    if (c->vkResetCommandPool(c->owner.device, slot->pool, 0) != VK_SUCCESS ||
        c->vkResetDescriptorPool(c->owner.device, slot->descriptors, 0) != VK_SUCCESS ||
        c->vkResetFences(c->owner.device, 1, &slot->fence) != VK_SUCCESS) {
        free(draw_sets);
        return MAG_NATIVE_DEVICE_FAILED;
    }
    slot->scene_images=calloc(scene->image_count,sizeof(*slot->scene_images));
    if(!slot->scene_images){free(draw_sets);return MAG_NATIVE_ALLOCATION_FAILED;}
    for (uint32_t i = 0; i < scene->image_count; ++i) {
        const mag_native_scene_image *desc = &scene->images[i];
        frontend_vk_scene_image *im = &slot->scene_images[i];
        slot->scene_image_count = i + 1;
        im->aspect =
            desc->format != MAG_NATIVE_SCENE_D32 ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        VkImageUsageFlags flags =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            (desc->format != MAG_NATIVE_SCENE_D32 ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                                  : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        status = batch_image(c, desc->width, desc->height, scene_vk_format(desc->format), im->aspect, flags,
                             &im->image, &im->memory, &im->view);
        if (status)
            goto fail;
    }
    for (uint32_t i = 0; i < scene->pass_count; ++i) {
        const mag_native_scene_pass *p = &scene->passes[i];
        slot->scene_pass_count = i + 1;
        status = scene_renderpass(
            c, p->color.image == MAG_NATIVE_SCENE_NONE ? 0 : scene->images[p->color.image].format,
            p->depth.image == MAG_NATIVE_SCENE_NONE ? 0 : MAG_NATIVE_SCENE_D32, p,
            &slot->scene_renderpasses[i]);
        if (status)
            goto fail;
        VkImageView views[2];
        uint32_t count = 0;
        uint32_t ref = p->color.image != MAG_NATIVE_SCENE_NONE ? p->color.image : p->depth.image;
        if (p->color.image != MAG_NATIVE_SCENE_NONE)
            views[count++] = slot->scene_images[p->color.image].view;
        if (p->depth.image != MAG_NATIVE_SCENE_NONE)
            views[count++] = slot->scene_images[p->depth.image].view;
        VkFramebufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                        .renderPass = slot->scene_renderpasses[i],
                                        .attachmentCount = count,
                                        .pAttachments = views,
                                        .width = scene->images[ref].width,
                                        .height = scene->images[ref].height,
                                        .layers = 1};
        if (c->vkCreateFramebuffer(c->owner.device, &info, NULL, &slot->scene_framebuffers[i]) !=
            VK_SUCCESS) {
            status = MAG_NATIVE_ALLOCATION_FAILED;
            goto fail;
        }
    }
    for (uint32_t i = 0; i < 3; ++i) {
        VkSamplerCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = i == MAG_NATIVE_SCENE_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .minFilter = i == MAG_NATIVE_SCENE_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .compareEnable = i == MAG_NATIVE_SCENE_COMPARE_LEQUAL,
            .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
            .minLod = 0,
            .maxLod = 0,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
        if (c->vkCreateSampler(c->owner.device, &info, NULL, &slot->scene_samplers[i]) != VK_SUCCESS) {
            status = MAG_NATIVE_ALLOCATION_FAILED;
            goto fail;
        }
    }
    VkDescriptorSet compute_sets[MAG_NATIVE_BATCH_MAX_COMMANDS];
    for (uint32_t n = 0; n < scene->dispatch_count; ++n) {
        const mag_native_frame *f = &scene->dispatches[n];
        frontend_vk_pipeline *p = pipeline_find(c, f->pipeline);
        used[used_count++] = p;
        VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                             .descriptorPool = slot->descriptors,
                                             .descriptorSetCount = 1,
                                             .pSetLayouts = &p->descriptors};
        if (c->vkAllocateDescriptorSets(c->owner.device, &alloc, &compute_sets[n]) != VK_SUCCESS) {
            status = MAG_NATIVE_ALLOCATION_FAILED;
            goto fail;
        }
        VkDescriptorBufferInfo buffers[16];
        VkWriteDescriptorSet writes[16];
        for (uint32_t j = 0; j < f->binding_count; ++j) {
            mag_native_binding b = f->bindings[j];
            buffers[j] = (VkDescriptorBufferInfo){buffer_find(c, b.buffer)->gpu, b.offset, b.byte_length};
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            for (uint32_t k = 0; k < p->count; ++k)
                if (p->bindings[k].slot == b.slot && p->bindings[k].binding_kind == MAG_NATIVE_UNIFORM)
                    type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[j] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                               .dstSet = compute_sets[n],
                                               .dstBinding = b.slot,
                                               .descriptorCount = 1,
                                               .descriptorType = type,
                                               .pBufferInfo = &buffers[j]};
        }
        c->vkUpdateDescriptorSets(c->owner.device, f->binding_count, writes, 0, NULL);
    }
    for (uint32_t n = 0; n < scene->pass_count; ++n)
        for (uint32_t j = 0; j < scene->passes[n].draw_count; ++j) {
            const mag_native_scene_draw *d = &scene->passes[n].draws[j];
            frontend_vk_pipeline *p = pipeline_find(c, d->pipeline);
            used[used_count++] = p;
            status = scene_descriptors(c, slot, p, d, draw_sets[draw_count++]);
            if (status)
                goto fail;
        }
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (c->vkBeginCommandBuffer(slot->command, &begin) != VK_SUCCESS) {
        status = MAG_NATIVE_DEVICE_FAILED;
        goto fail;
    }
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                                             VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                             VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT};
    c->vkCmdPipelineBarrier(slot->command, VK_PIPELINE_STAGE_HOST_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                            0, 1, &host, 0, NULL, 0, NULL);
    for (uint32_t i = 0; i < scene->image_count; ++i)
        if (scene->images[i].upload.id) {
            const mag_native_scene_image *im = &scene->images[i];
            scene_image_transition(c, slot, i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .imageExtent = {im->width, im->height, 1}};
            c->vkCmdCopyBufferToImage(slot->command, buffer_find(c, im->upload)->gpu,
                                      slot->scene_images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                      &copy);
        }
    for (uint32_t n = 0; n < scene->dispatch_count; ++n) {
        const mag_native_frame *f = &scene->dispatches[n];
        frontend_vk_pipeline *p = pipeline_find(c, f->pipeline);
        c->vkCmdBindPipeline(slot->command, VK_PIPELINE_BIND_POINT_COMPUTE, p->gpu);
        c->vkCmdBindDescriptorSets(slot->command, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1,
                                   &compute_sets[n], 0, NULL);
        c->vkCmdDispatch(slot->command, f->groups[0], f->groups[1], f->groups[2]);
        VkMemoryBarrier dependency = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                      .dstAccessMask =
                                          VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                                          VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT};
        c->vkCmdPipelineBarrier(slot->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                0, 1, &dependency, 0, NULL, 0, NULL);
    }
    draw_count = 0;
    for (uint32_t n = 0; n < scene->pass_count; ++n) {
        const mag_native_scene_pass *p = &scene->passes[n];
        // Each sampled image is transitioned before entering the render pass; an
        // active attachment alias was already rejected by the shared validator.
        uint32_t sampled = 0;
        for (uint32_t j = 0; j < p->draw_count; ++j)
            for (uint32_t k = 0; k < p->draws[j].texture_count; ++k) {
                uint32_t index = p->draws[j].textures[k].image;
                if (!(sampled & (1u << index))) {
                    sampled |= 1u << index;
                    frontend_vk_scene_image *im = &slot->scene_images[index];
                    scene_image_transition(c, slot, index,
                                           im->aspect == VK_IMAGE_ASPECT_DEPTH_BIT
                                               ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                           VK_ACCESS_SHADER_READ_BIT,
                                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                }
            }
        VkClearValue clear[2];
        memset(clear, 0, sizeof(clear));
        uint32_t clear_count = 0,
                 ref = p->color.image != MAG_NATIVE_SCENE_NONE ? p->color.image : p->depth.image;
        if (p->color.image != MAG_NATIVE_SCENE_NONE) {
            scene_image_transition(c, slot, p->color.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            memcpy(clear[clear_count++].color.float32, p->color.clear, sizeof(p->color.clear));
        }
        if (p->depth.image != MAG_NATIVE_SCENE_NONE) {
            scene_image_transition(
                c, slot, p->depth.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
            clear[clear_count++].depthStencil.depth = p->depth.clear[0];
        }
        const mag_native_scene_image *extent = &scene->images[ref];
        VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                      .renderPass = slot->scene_renderpasses[n],
                                      .framebuffer = slot->scene_framebuffers[n],
                                      .renderArea = {{0, 0}, {extent->width, extent->height}},
                                      .clearValueCount = clear_count,
                                      .pClearValues = clear};
        c->vkCmdBeginRenderPass(slot->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        // Vulkan1.1 negative viewport height matches Metal's authored top-left image.
        VkViewport viewport = {0, (float)extent->height, (float)extent->width, -(float)extent->height, 0, 1};
        VkRect2D scissor = {{0, 0}, {extent->width, extent->height}};
        c->vkCmdSetViewport(slot->command, 0, 1, &viewport);
        c->vkCmdSetScissor(slot->command, 0, 1, &scissor);
        for (uint32_t j = 0; j < p->draw_count; ++j) {
            const mag_native_scene_draw *d = &p->draws[j];
            frontend_vk_pipeline *pipeline = pipeline_find(c, d->pipeline);
            c->vkCmdBindPipeline(slot->command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->gpu);
            c->vkCmdBindDescriptorSets(slot->command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 0, 2,
                                       draw_sets[draw_count++], 0, NULL);
            VkBuffer vertex = buffer_find(c, d->vertices)->gpu;
            VkDeviceSize offset = d->vertex_offset;
            c->vkCmdBindVertexBuffers(slot->command, 0, 1, &vertex, &offset);
            c->vkCmdBindIndexBuffer(slot->command, buffer_find(c, d->indices)->gpu, d->index_offset,
                                    VK_INDEX_TYPE_UINT32);
            c->vkCmdDrawIndexed(slot->command, d->index_count, 1, 0, 0, 0);
        }
        c->vkCmdEndRenderPass(slot->command);
    }
    if (scene->diagnostic_readback.id) {
        const mag_native_scene_image *im = &scene->images[scene->output_image];
        scene_image_transition(c, slot, scene->output_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {im->width, im->height, 1}};
        c->vkCmdCopyImageToBuffer(slot->command, slot->scene_images[scene->output_image].image,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  buffer_find(c, scene->diagnostic_readback)->gpu, 1, &copy);
        VkMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        c->vkCmdPipelineBarrier(slot->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                1, &after, 0, NULL, 0, NULL);
    }
    if (c->vkEndCommandBuffer(slot->command) != VK_SUCCESS) {
        status = MAG_NATIVE_DEVICE_FAILED;
        goto fail;
    }
    free(draw_sets);
    draw_sets = NULL;
    slot->count = scene->resource_count;
    for (uint32_t i = 0; i < slot->count; ++i) {
        frontend_vk_buffer *b = buffer_find(c, scene->resources[i].buffer);
        slot->buffers[i] = b;
        slot->writes[i] = usage.write[i];
        if (usage.write[i]) {
            ++b->writers;
            b->gpu_written = 1;
        } else
            ++b->readers;
    }
    slot->batch_count = used_count;
    for (uint32_t i = 0; i < used_count; ++i) {
        slot->batch_pipelines[i] = used[i];
        ++used[i]->references;
    }
    slot->occupied = 1;
    slot->ticket = (mag_native_ticket){c->generation, ++c->next_submission};
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &slot->command};
    VkResult result = c->vkQueueSubmit(c->owner.queue, 1, &submit, slot->fence);
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        slot_release(c, slot);
        return MAG_NATIVE_ALLOCATION_FAILED;
    }
    *out = slot->ticket;
    if (result != VK_SUCCESS) {
        c->failed = 1;
        return MAG_NATIVE_INDETERMINATE;
    }
    return MAG_NATIVE_OK;
fail:
    free(draw_sets);
    scene_attachments_release(c, slot);
    return status;
}
static int submit_scene(void *opaque, const mag_native_scene *scene, mag_native_ticket *out) {
    frontend_vk *c = opaque;
    int status = c->owner.host.try_begin(c->owner.host.userdata, c->owner.host.lifetime);
    if (status)
        return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
    int result = submit_scene_owned(c, scene, out);
    c->owner.host.end(c->owner.host.userdata, c->owner.host.lifetime);
    return result;
}
