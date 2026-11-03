/* Ordered frontend-owned Vulkan compute/graphics batch. No SDK API required. */
static int batch_index(const mag_native_batch *batch, mag_native_buffer ref) {
    for (uint32_t i = 0; i < batch->resource_count; ++i) {
        mag_native_buffer b = batch->resources[i].buffer;
        if (b.device_generation == ref.device_generation && b.id == ref.id && b.version == ref.version)
            return (int)i;
    }
    return -1;
}
static int batch_access(const mag_native_batch *batch, mag_native_buffer ref, int write, int *writers,
                        uint8_t *access) {
    int i = batch_index(batch, ref);
    if (i < 0)
        return -1;
    uint32_t role = batch->resources[i].role;
    if (write) {
        if (role == MAG_NATIVE_UPLOAD || writers[i] >= 0 || access[i])
            return -1;
        access[i] = 2;
    } else {
        if (role == MAG_NATIVE_READBACK || (role == MAG_NATIVE_PRODUCED && writers[i] < 0) || access[i] == 2)
            return -1;
        access[i] = 1;
    }
    return i;
}
static int buffer_range(frontend_vk_buffer *b, uint64_t offset, uint64_t length, uint64_t alignment) {
    return length && !(offset % alignment) && offset <= b->length && length <= b->length - offset;
}
static int graphics_create(void *opaque, const mag_native_graphics_desc *desc, mag_native_pipeline *out) {
    frontend_vk *c = opaque;
    if (c->failed)
        return MAG_NATIVE_DEVICE_FAILED;
    if (desc->vertex_stride != 16 || desc->reserved)
        return MAG_NATIVE_UNSUPPORTED;
    PFN_vkGetPhysicalDeviceFormatProperties format_properties =
        (PFN_vkGetPhysicalDeviceFormatProperties)c->owner.host.get_instance_proc(
            c->owner.host.instance, "vkGetPhysicalDeviceFormatProperties");
    if (!format_properties)
        return MAG_NATIVE_UNSUPPORTED;
    VkFormatProperties color_format, depth_format;
    format_properties((VkPhysicalDevice)c->owner.host.physical_device, VK_FORMAT_R8G8B8A8_UNORM,
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
    VkAttachmentDescription attachments[2] = {
        {.format = VK_FORMAT_R8G8B8A8_UNORM,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.format = VK_FORMAT_D32_SFLOAT,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                          depth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &color,
                                    .pDepthStencilAttachment = &depth};
    VkSubpassDependency dependency = {.srcSubpass = VK_SUBPASS_EXTERNAL,
                                      .dstSubpass = 0,
                                      .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    VkRenderPassCreateInfo render = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                     .attachmentCount = 2,
                                     .pAttachments = attachments,
                                     .subpassCount = 1,
                                     .pSubpasses = &subpass,
                                     .dependencyCount = 1,
                                     .pDependencies = &dependency};
    if (c->vkCreateRenderPass(c->owner.device, &render, NULL, &p->render_pass) != VK_SUCCESS)
        goto done;
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
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
    VkVertexInputBindingDescription binding = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 1,
        .pVertexAttributeDescriptions = &attribute};
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
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS};
    VkPipelineColorBlendAttachmentState blend = {.colorWriteMask =
                                                     VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend};
    VkDynamicState states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = 2,
                                                .pDynamicStates = states};
    VkGraphicsPipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                             .stageCount = 2,
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
static int batch_image(frontend_vk *c, uint32_t width, uint32_t height, VkFormat format,
                       VkImageAspectFlags aspect, VkImageUsageFlags usage, VkImage *image,
                       VkDeviceMemory *memory, VkDeviceSize *memory_bytes, VkImageView *view) {
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {width, height, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = usage,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (c->vkCreateImage(c->owner.device, &info, NULL, image) != VK_SUCCESS)
        return MAG_NATIVE_ALLOCATION_FAILED;
    VkMemoryRequirements required;
    c->vkGetImageMemoryRequirements(c->owner.device, *image, &required);
    uint32_t type;
    for (type = 0; type < c->owner.memory.memoryTypeCount; ++type)
        if ((required.memoryTypeBits & (1u << type)) &&
            (c->owner.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            break;
    if (type == c->owner.memory.memoryTypeCount)
        return MAG_NATIVE_UNSUPPORTED;
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = required.size,
                                       .memoryTypeIndex = type};
    if (native_memory_allocate(c, &allocation, memory, memory_bytes) != VK_SUCCESS ||
        c->vkBindImageMemory(c->owner.device, *image, *memory, 0) != VK_SUCCESS)
        return MAG_NATIVE_ALLOCATION_FAILED;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = *image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .subresourceRange = {aspect, 0, 1, 0, 1}};
    return c->vkCreateImageView(c->owner.device, &vi, NULL, view) == VK_SUCCESS
               ? MAG_NATIVE_OK
               : MAG_NATIVE_ALLOCATION_FAILED;
}
static int batch_validate(frontend_vk *c, const mag_native_batch *batch, frontend_vk_buffer **buffers,
                          uint32_t *writes, frontend_vk_pipeline **pipelines) {
    if (!batch->resources || !batch->resource_count || batch->resource_count > 64 || !batch->commands ||
        !batch->command_count || batch->command_count > 32)
        return MAG_NATIVE_INVALID;
    int writers[64];
    memset(writers, -1, sizeof(writers));
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < batch->resource_count; ++i) {
        mag_native_batch_resource r = batch->resources[i];
        if (r.reserved || r.role > MAG_NATIVE_READBACK)
            return MAG_NATIVE_INVALID;
        frontend_vk_buffer *b = buffer_find(c, r.buffer);
        if (!b)
            return MAG_NATIVE_STALE;
        if (b->readers || b->writers)
            return MAG_NATIVE_BUSY;
        if ((r.role == MAG_NATIVE_UPLOAD && b->gpu_written) ||
            (r.role != MAG_NATIVE_UPLOAD && (!b->seed_zero || b->gpu_written)))
            return MAG_NATIVE_INVALID;
        for (uint32_t j = 0; j < i; ++j)
            if (buffers[j] == b)
                return MAG_NATIVE_INVALID;
        buffers[i] = b;
        bytes += b->length;
        if (bytes > MAG_NATIVE_BATCH_MAX_BYTES)
            return MAG_NATIVE_ALLOCATION_FAILED;
    }
    for (uint32_t node = 0; node < batch->command_count; ++node) {
        mag_native_batch_command command = batch->commands[node];
        uint8_t access[64] = {0};
        if (command.reserved)
            return MAG_NATIVE_INVALID;
        if (command.kind == MAG_NATIVE_DISPATCH) {
            const mag_native_frame *f = command.dispatch;
            if (!f || command.draw || f->struct_size != sizeof(*f) || f->abi_version != 1)
                return MAG_NATIVE_INVALID;
            frontend_vk_pipeline *p = pipeline_find(c, f->pipeline);
            if (!p)
                return MAG_NATIVE_STALE;
            if (p->graphics || !f->bindings || f->binding_count != p->count)
                return MAG_NATIVE_INVALID;
            pipelines[node] = p;
            for (unsigned k = 0; k < 3; ++k)
                if (!f->groups[k] || f->groups[k] > c->owner.properties.limits.maxComputeWorkGroupCount[k])
                    return MAG_NATIVE_INVALID;
            uint32_t slots = 0;
            for (uint32_t j = 0; j < f->binding_count; ++j) {
                mag_native_binding b = f->bindings[j];
                if (b.reserved || b.slot >= 16 || (slots & (1u << b.slot)))
                    return MAG_NATIVE_INVALID;
                slots |= 1u << b.slot;
                const mag_native_binding_layout *schema = NULL;
                for (uint32_t k = 0; k < p->count; ++k)
                    if (p->bindings[k].slot == b.slot)
                        schema = &p->bindings[k];
                if (!schema)
                    return MAG_NATIVE_INVALID;
                int i =
                    batch_access(batch, b.buffer, schema->access == MAG_NATIVE_READ_WRITE, writers, access);
                if (i < 0)
                    return MAG_NATIVE_INVALID;
                VkDeviceSize alignment = schema->binding_kind == MAG_NATIVE_UNIFORM
                                             ? c->owner.properties.limits.minUniformBufferOffsetAlignment
                                             : c->owner.properties.limits.minStorageBufferOffsetAlignment;
                uint64_t limit = schema->binding_kind == MAG_NATIVE_UNIFORM
                                     ? c->owner.properties.limits.maxUniformBufferRange
                                     : c->owner.properties.limits.maxStorageBufferRange;
                if (!buffer_range(buffers[i], b.offset, b.byte_length, schema->alignment_bytes) ||
                    (alignment && b.offset % alignment) || b.byte_length < schema->minimum_bytes ||
                    b.byte_length > limit ||
                    (schema->runtime_array_stride && b.byte_length % schema->runtime_array_stride))
                    return MAG_NATIVE_INVALID;
            }
        } else if (command.kind == MAG_NATIVE_INDEXED_DRAW) {
            const mag_native_indexed_draw *d = command.draw;
            if (!d || command.dispatch || node + 1 != batch->command_count)
                return MAG_NATIVE_INVALID;
            frontend_vk_pipeline *p = pipeline_find(c, d->pipeline);
            if (!p)
                return MAG_NATIVE_STALE;
            if (!p->graphics)
                return MAG_NATIVE_INVALID;
            pipelines[node] = p;
            if (d->reserved || d->reserved1 || !d->width || !d->height || d->width > 8192 ||
                d->height > 8192 || d->width > c->owner.properties.limits.maxFramebufferWidth ||
                d->height > c->owner.properties.limits.maxFramebufferHeight || !d->index_count ||
                d->index_count % 3)
                return MAG_NATIVE_INVALID;
            for (unsigned j = 0; j < 4; ++j)
                if (!isfinite(d->clear_color[j]) || d->clear_color[j] < 0 || d->clear_color[j] > 1)
                    return MAG_NATIVE_INVALID;
            if (!isfinite(d->clear_depth) || d->clear_depth < 0 || d->clear_depth > 1)
                return MAG_NATIVE_INVALID;
            int v = batch_access(batch, d->vertices, 0, writers, access),
                i = batch_access(batch, d->indices, 0, writers, access);
            if (v < 0 || i < 0 || v == i || batch->resources[v].role != MAG_NATIVE_PRODUCED ||
                batch->resources[i].role != MAG_NATIVE_PRODUCED ||
                !buffer_range(buffers[v], d->vertex_offset, d->vertex_length, 16) || d->vertex_length % 16 ||
                !buffer_range(buffers[i], d->index_offset, d->index_length, 4) ||
                d->index_length < (uint64_t)d->index_count * 4)
                return MAG_NATIVE_INVALID;
            if (d->color_readback.id) {
                int r = batch_access(batch, d->color_readback, 1, writers, access);
                if (r < 0 || batch->resources[r].role != MAG_NATIVE_READBACK ||
                    buffers[r]->length != (uint64_t)d->width * d->height * 4)
                    return MAG_NATIVE_INVALID;
            } else if (d->color_readback.device_generation || d->color_readback.version)
                return MAG_NATIVE_INVALID;
            bytes += (uint64_t)d->width * d->height * 8;
            if (bytes > MAG_NATIVE_BATCH_MAX_BYTES)
                return MAG_NATIVE_ALLOCATION_FAILED;
        } else
            return MAG_NATIVE_INVALID;
        for (uint32_t i = 0; i < batch->resource_count; ++i)
            if (access[i] == 2) {
                writers[i] = (int)node;
                writes[i] = 1;
            }
    }
    return MAG_NATIVE_OK;
}
static int submit_batch_owned(frontend_vk *c, const mag_native_batch *batch, mag_native_ticket *out) {
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
    frontend_vk_buffer *buffers[64] = {0};
    frontend_vk_pipeline *pipelines[32] = {0};
    uint32_t writes[64] = {0};
    int status = batch_validate(c, batch, buffers, writes, pipelines);
    if (status)
        return status;
    if (c->vkResetCommandPool(c->owner.device, slot->pool, 0) != VK_SUCCESS ||
        c->vkResetDescriptorPool(c->owner.device, slot->descriptors, 0) != VK_SUCCESS ||
        c->vkResetFences(c->owner.device, 1, &slot->fence) != VK_SUCCESS)
        return MAG_NATIVE_DEVICE_FAILED;
    VkDescriptorSet sets[32] = {0};
    for (uint32_t node = 0; node < batch->command_count; ++node) {
        const mag_native_frame *f = batch->commands[node].dispatch;
        frontend_vk_pipeline *p = pipelines[node];
        if (!f)
            continue;
        VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                .descriptorPool = slot->descriptors,
                                                .descriptorSetCount = 1,
                                                .pSetLayouts = &p->descriptors};
        if (c->vkAllocateDescriptorSets(c->owner.device, &allocate, &sets[node]) != VK_SUCCESS)
            return MAG_NATIVE_ALLOCATION_FAILED;
        VkDescriptorBufferInfo infos[16] = {{0}};
        VkWriteDescriptorSet descriptors[16] = {{0}};
        for (uint32_t j = 0; j < f->binding_count; ++j) {
            mag_native_binding b = f->bindings[j];
            const mag_native_binding_layout *schema = NULL;
            for (uint32_t k = 0; k < p->count; ++k)
                if (p->bindings[k].slot == b.slot)
                    schema = &p->bindings[k];
            infos[j] =
                (VkDescriptorBufferInfo){buffers[batch_index(batch, b.buffer)]->gpu, b.offset, b.byte_length};
            descriptors[j] =
                (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = sets[node],
                                       .dstBinding = b.slot,
                                       .descriptorCount = 1,
                                       .descriptorType = schema->binding_kind == MAG_NATIVE_UNIFORM
                                                             ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                             : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &infos[j]};
        }
        c->vkUpdateDescriptorSets(c->owner.device, p->count, descriptors, 0, NULL);
    }
    const mag_native_indexed_draw *draw = batch->commands[batch->command_count - 1].draw;
    if (draw) {
        status =
            batch_image(c, draw->width, draw->height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &slot->color,
                        &slot->color_memory, &slot->color_bytes, &slot->color_view);
        if (status)
            goto fail;
        status = batch_image(c, draw->width, draw->height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &slot->depth, &slot->depth_memory,
                             &slot->depth_bytes, &slot->depth_view);
        if (status)
            goto fail;
        VkImageView views[2] = {slot->color_view, slot->depth_view};
        VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                      .renderPass = pipelines[batch->command_count - 1]->render_pass,
                                      .attachmentCount = 2,
                                      .pAttachments = views,
                                      .width = draw->width,
                                      .height = draw->height,
                                      .layers = 1};
        if (c->vkCreateFramebuffer(c->owner.device, &fb, NULL, &slot->framebuffer) != VK_SUCCESS) {
            status = MAG_NATIVE_ALLOCATION_FAILED;
            goto fail;
        }
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
                                             VK_ACCESS_SHADER_WRITE_BIT};
    c->vkCmdPipelineBarrier(slot->command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            0, 1, &host, 0, NULL, 0, NULL);
    for (uint32_t node = 0; node < batch->command_count; ++node) {
        mag_native_batch_command command = batch->commands[node];
        frontend_vk_pipeline *p = pipelines[node];
        if (command.kind == MAG_NATIVE_DISPATCH) {
            const mag_native_frame *f = command.dispatch;
            c->vkCmdBindPipeline(slot->command, VK_PIPELINE_BIND_POINT_COMPUTE, p->gpu);
            c->vkCmdBindDescriptorSets(slot->command, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1,
                                       &sets[node], 0, NULL);
            c->vkCmdDispatch(slot->command, f->groups[0], f->groups[1], f->groups[2]);
            VkMemoryBarrier dependency = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                          .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                          .dstAccessMask =
                                              VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                                              VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT};
            c->vkCmdPipelineBarrier(slot->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                    0, 1, &dependency, 0, NULL, 0, NULL);
        } else {
            const mag_native_indexed_draw *d = command.draw;
            VkClearValue clear[2] = {0};
            memcpy(clear[0].color.float32, d->clear_color, sizeof(d->clear_color));
            clear[1].depthStencil.depth = d->clear_depth;
            VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                          .renderPass = p->render_pass,
                                          .framebuffer = slot->framebuffer,
                                          .renderArea = {{0, 0}, {d->width, d->height}},
                                          .clearValueCount = 2,
                                          .pClearValues = clear};
            c->vkCmdBeginRenderPass(slot->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
            c->vkCmdBindPipeline(slot->command, VK_PIPELINE_BIND_POINT_GRAPHICS, p->gpu);
            VkViewport viewport = {0, 0, (float)d->width, (float)d->height, 0, 1};
            VkRect2D scissor = {{0, 0}, {d->width, d->height}};
            c->vkCmdSetViewport(slot->command, 0, 1, &viewport);
            c->vkCmdSetScissor(slot->command, 0, 1, &scissor);
            VkBuffer vertex = buffers[batch_index(batch, d->vertices)]->gpu;
            VkDeviceSize offset = d->vertex_offset;
            c->vkCmdBindVertexBuffers(slot->command, 0, 1, &vertex, &offset);
            c->vkCmdBindIndexBuffer(slot->command, buffers[batch_index(batch, d->indices)]->gpu,
                                    d->index_offset, VK_INDEX_TYPE_UINT32);
            c->vkCmdDrawIndexed(slot->command, d->index_count, 1, 0, 0, 0);
            c->vkCmdEndRenderPass(slot->command);
            if (d->color_readback.id) {
                VkImageMemoryBarrier image = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                              .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                              .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                              .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                              .image = slot->color,
                                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
                c->vkCmdPipelineBarrier(slot->command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &image);
                VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                          .imageExtent = {d->width, d->height, 1}};
                c->vkCmdCopyImageToBuffer(slot->command, slot->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          buffers[batch_index(batch, d->color_readback)]->gpu, 1, &copy);
            }
        }
    }
    VkMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                             .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    c->vkCmdPipelineBarrier(slot->command,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, NULL, 0, NULL);
    if (c->vkEndCommandBuffer(slot->command) != VK_SUCCESS) {
        status = MAG_NATIVE_DEVICE_FAILED;
        goto fail;
    }
    slot->count = batch->resource_count;
    for (uint32_t i = 0; i < slot->count; ++i) {
        slot->buffers[i] = buffers[i];
        slot->writes[i] = writes[i];
        if (writes[i]) {
            ++buffers[i]->writers;
            buffers[i]->gpu_written = 1;
        } else
            ++buffers[i]->readers;
    }
    slot->batch_count = batch->command_count;
    for (uint32_t i = 0; i < slot->batch_count; ++i) {
        slot->batch_pipelines[i] = pipelines[i];
        ++pipelines[i]->references;
    }
    slot->occupied = 1;
    slot->ticket = (mag_native_ticket){c->generation, ++c->next_submission};
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &slot->command};
    VkResult result = c->vkQueueSubmit(c->owner.queue, 1, &submit_info, slot->fence);
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
    batch_attachments_release(c, slot);
    return status;
}
static int submit_batch(void *opaque, const mag_native_batch *batch, mag_native_ticket *out) {
    frontend_vk *c = opaque;
    int status = c->owner.host.try_begin(c->owner.host.userdata, c->owner.host.lifetime);
    if (status)
        return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
    int result = submit_batch_owned(c, batch, out);
    c->owner.host.end(c->owner.host.userdata, c->owner.host.lifetime);
    return result;
}
