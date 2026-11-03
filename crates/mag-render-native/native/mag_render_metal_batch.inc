/* Frontend-owned ordered batch path. Included after common Metal registry
 * helpers so single-dispatch and batch share the same native lifetime owner. */
static int metal_graphics_create(void *opaque, const mag_native_graphics_desc *desc,
                                 mag_native_pipeline *out) {
    MagRenderMetal *c = context(opaque);
    if (desc->vertex_stride != 16 || desc->reserved)
        return MAG_NATIVE_UNSUPPORTED;
    id<MTLFunction> stages[2] = {nil, nil};
    const mag_native_shader_stage *sources[2] = {&desc->vertex, &desc->fragment};
    for (unsigned i = 0; i < 2; ++i) {
        const mag_native_shader_stage *s = sources[i];
        if (s->language != MAG_NATIVE_MSL)
            return MAG_NATIVE_UNSUPPORTED;
        if (!s->source || !s->source_length || s->source_length > 1024 * 1024 || !s->entry ||
            !s->entry_length || s->entry_length > 128)
            return MAG_NATIVE_INVALID;
        NSString *source =
            [[NSString alloc] initWithBytes:s->source length:s->source_length encoding:NSUTF8StringEncoding];
        NSString *entry =
            [[NSString alloc] initWithBytes:s->entry length:s->entry_length encoding:NSUTF8StringEncoding];
        if (!source || !entry)
            return MAG_NATIVE_INVALID;
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_0;
        NSError *error = nil;
        id<MTLLibrary> library = [c->device newLibraryWithSource:source options:options error:&error];
        stages[i] = [library newFunctionWithName:entry];
        if (!stages[i])
            return MAG_NATIVE_UNSUPPORTED;
    }
    MTLVertexDescriptor *vertex = [MTLVertexDescriptor new];
    vertex.attributes[0].format = MTLVertexFormatFloat4;
    vertex.attributes[0].offset = 0;
    vertex.attributes[0].bufferIndex = 0;
    vertex.layouts[0].stride = 16;
    vertex.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
    pipeline.vertexFunction = stages[0];
    pipeline.fragmentFunction = stages[1];
    pipeline.vertexDescriptor = vertex;
    pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    pipeline.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError *error = nil;
    id<MTLRenderPipelineState> gpu = [c->device newRenderPipelineStateWithDescriptor:pipeline error:&error];
    if (!gpu)
        return MAG_NATIVE_UNSUPPORTED;
    MTLDepthStencilDescriptor *depth = [MTLDepthStencilDescriptor new];
    depth.depthCompareFunction = MTLCompareFunctionLess;
    depth.depthWriteEnabled = YES;
    id<MTLDepthStencilState> depth_gpu = [c->device newDepthStencilStateWithDescriptor:depth];
    if (!depth_gpu)
        return MAG_NATIVE_ALLOCATION_FAILED;
    if (c->next_pipeline == UINT64_MAX)
        return MAG_NATIVE_ALLOCATION_FAILED;
    MagRenderPipeline *record = [MagRenderPipeline new];
    record->graphics = gpu;
    record->depth = depth_gpu;
    uint64_t id = ++c->next_pipeline;
    c->pipelines[@(id)] = record;
    *out = (mag_native_pipeline){c->generation, id};
    return MAG_NATIVE_OK;
}
static int batch_resource_index(const mag_native_batch *batch, mag_native_buffer ref) {
    for (uint32_t i = 0; i < batch->resource_count; ++i) {
        mag_native_buffer b = batch->resources[i].buffer;
        if (b.device_generation == ref.device_generation && b.id == ref.id && b.version == ref.version)
            return (int)i;
    }
    return -1;
}
static int batch_range(MagRenderBuffer *b, uint64_t offset, uint64_t length, uint64_t alignment) {
    return length && !(offset % alignment) && offset <= b->gpu.length && length <= b->gpu.length - offset;
}
/* Returns a resource index only after proving role, publication and same-node
 * alias. Defining one write does not publish it to another node until this
 * command's complete schema has been validated. */
static int batch_access(const mag_native_batch *batch, mag_native_buffer ref, int write, int node,
                        int writers[MAG_NATIVE_BATCH_MAX_RESOURCES],
                        uint8_t access[MAG_NATIVE_BATCH_MAX_RESOURCES]) {
    int i = batch_resource_index(batch, ref);
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
    (void)node;
    return i;
}
static int metal_batch_owned(void *opaque, const mag_native_batch *batch, mag_native_ticket *out) {
    MagRenderMetal *c = context(opaque);
    if (c->pending.count >= 2)
        return MAG_NATIVE_BUSY;
    if (!batch->resources || !batch->resource_count ||
        batch->resource_count > MAG_NATIVE_BATCH_MAX_RESOURCES || !batch->commands || !batch->command_count ||
        batch->command_count > MAG_NATIVE_BATCH_MAX_COMMANDS)
        return MAG_NATIVE_INVALID;
    if (c->next_submission == UINT64_MAX)
        return MAG_NATIVE_DEVICE_FAILED;
    NSMutableArray<MagRenderBuffer *> *buffers = [NSMutableArray new];
    NSMutableArray<MagRenderUse *> *uses = [NSMutableArray new];
    NSMutableArray *retained = [NSMutableArray new];
    int writers[MAG_NATIVE_BATCH_MAX_RESOURCES];
    memset(writers, -1, sizeof(writers));
    uint8_t writes[MAG_NATIVE_BATCH_MAX_RESOURCES] = {0};
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < batch->resource_count; ++i) {
        mag_native_batch_resource r = batch->resources[i];
        if (r.reserved || r.role > MAG_NATIVE_READBACK)
            return MAG_NATIVE_INVALID;
        MagRenderBuffer *b = find_buffer(c, r.buffer);
        if (!b)
            return MAG_NATIVE_STALE;
        if ([buffers containsObject:b])
            return MAG_NATIVE_INVALID;
        if (b->readers || b->writers)
            return MAG_NATIVE_BUSY;
        if (r.role == MAG_NATIVE_UPLOAD && b->gpu_written)
            return MAG_NATIVE_INVALID;
        if (r.role != MAG_NATIVE_UPLOAD && (!b->seed_zero || b->gpu_written))
            return MAG_NATIVE_INVALID;
        bytes += b->gpu.length;
        if (bytes > MAG_NATIVE_BATCH_MAX_BYTES)
            return MAG_NATIVE_ALLOCATION_FAILED;
        [buffers addObject:b];
    }
    /* Entire command sequence is checked before creating/encoding any CB. */
    for (uint32_t node = 0; node < batch->command_count; ++node) {
        mag_native_batch_command command = batch->commands[node];
        uint8_t access[MAG_NATIVE_BATCH_MAX_RESOURCES] = {0};
        if (command.reserved)
            return MAG_NATIVE_INVALID;
        if (command.kind == MAG_NATIVE_DISPATCH) {
            if (!command.dispatch || command.draw)
                return MAG_NATIVE_INVALID;
            const mag_native_frame *f = command.dispatch;
            if (f->struct_size != sizeof(*f) || f->abi_version != 1)
                return MAG_NATIVE_INVALID;
            MagRenderPipeline *p = find_pipeline(c, f->pipeline);
            if (!p)
                return MAG_NATIVE_STALE;
            if (!p->gpu || !f->bindings || f->binding_count != p->binding_count)
                return MAG_NATIVE_INVALID;
            uint32_t slots = 0;
            for (unsigned k = 0; k < 3; ++k)
                if (!f->groups[k] || f->groups[k] > 65535)
                    return MAG_NATIVE_INVALID;
            for (uint32_t j = 0; j < f->binding_count; ++j) {
                mag_native_binding b = f->bindings[j];
                const mag_native_binding_layout *schema = NULL;
                if (b.reserved || b.slot >= 16 || (slots & (1u << b.slot)))
                    return MAG_NATIVE_INVALID;
                slots |= 1u << b.slot;
                for (uint32_t k = 0; k < p->binding_count; ++k)
                    if (p->bindings[k].slot == b.slot)
                        schema = &p->bindings[k];
                if (!schema)
                    return MAG_NATIVE_INVALID;
                int i = batch_access(batch, b.buffer, schema->access == MAG_NATIVE_READ_WRITE, (int)node,
                                     writers, access);
                if (i < 0 || !batch_range(buffers[i], b.offset, b.byte_length, schema->alignment_bytes) ||
                    b.byte_length < schema->minimum_bytes || b.byte_length > UINT32_MAX ||
                    (schema->runtime_array_stride && b.byte_length % schema->runtime_array_stride))
                    return MAG_NATIVE_INVALID;
            }
            [retained addObject:p];
        } else if (command.kind == MAG_NATIVE_INDEXED_DRAW) {
            if (command.dispatch || !command.draw || node + 1 != batch->command_count)
                return MAG_NATIVE_INVALID;
            const mag_native_indexed_draw *d = command.draw;
            MagRenderPipeline *p = find_pipeline(c, d->pipeline);
            if (!p)
                return MAG_NATIVE_STALE;
            if (!p->graphics || d->reserved || d->reserved1 || !d->width || !d->height || d->width > 8192 ||
                d->height > 8192 || !d->index_count || d->index_count % 3)
                return MAG_NATIVE_INVALID;
            for (unsigned j = 0; j < 4; ++j)
                if (!isfinite(d->clear_color[j]) || d->clear_color[j] < 0 || d->clear_color[j] > 1)
                    return MAG_NATIVE_INVALID;
            if (!isfinite(d->clear_depth) || d->clear_depth < 0 || d->clear_depth > 1)
                return MAG_NATIVE_INVALID;
            int readback = d->color_readback.id != 0;
            if (!readback && (d->color_readback.device_generation || d->color_readback.version))
                return MAG_NATIVE_INVALID;
            int v = batch_access(batch, d->vertices, 0, (int)node, writers, access),
                i = batch_access(batch, d->indices, 0, (int)node, writers, access);
            int r = readback ? batch_access(batch, d->color_readback, 1, (int)node, writers, access) : -1;
            if (v < 0 || i < 0 || v == i || batch->resources[v].role != MAG_NATIVE_PRODUCED ||
                batch->resources[i].role != MAG_NATIVE_PRODUCED ||
                !batch_range(buffers[v], d->vertex_offset, d->vertex_length, 16) || d->vertex_length % 16 ||
                !batch_range(buffers[i], d->index_offset, d->index_length, 4) ||
                d->index_length < (uint64_t)d->index_count * 4)
                return MAG_NATIVE_INVALID;
            if (readback && (r < 0 || batch->resources[r].role != MAG_NATIVE_READBACK ||
                             buffers[r]->gpu.length != (uint64_t)d->width * d->height * 4))
                return MAG_NATIVE_INVALID;
            bytes += (uint64_t)d->width * d->height * 8 +
                     (readback ? (uint64_t)(((d->width * 4 + 255) / 256) * 256) * d->height : 0);
            if (bytes > MAG_NATIVE_BATCH_MAX_BYTES)
                return MAG_NATIVE_ALLOCATION_FAILED;
            [retained addObject:p];
        } else
            return MAG_NATIVE_INVALID;
        for (uint32_t i = 0; i < batch->resource_count; ++i)
            if (access[i] == 2) {
                writers[i] = (int)node;
                writes[i] = 1;
            }
    }
    MagRenderSubmission *submission = [MagRenderSubmission new];
    submission->retained = retained;
    for (uint32_t i = 0; i < batch->resource_count; ++i) {
        MagRenderUse *use = [MagRenderUse new];
        use->buffer = buffers[i];
        use->write = writes[i];
        [uses addObject:use];
    }
    submission->uses = uses;
    submission->command = [c->queue commandBuffer];
    if (!submission->command)
        return MAG_NATIVE_DEVICE_FAILED;
    for (uint32_t node = 0; node < batch->command_count; ++node) {
        mag_native_batch_command command = batch->commands[node];
        if (command.kind == MAG_NATIVE_DISPATCH) {
            const mag_native_frame *f = command.dispatch;
            MagRenderPipeline *p = find_pipeline(c, f->pipeline);
            id<MTLComputeCommandEncoder> encoder = [submission->command computeCommandEncoder];
            if (!encoder)
                return MAG_NATIVE_DEVICE_FAILED;
            [encoder setComputePipelineState:p->gpu];
            uint32_t lengths[16] = {0};
            for (uint32_t j = 0; j < f->binding_count; ++j) {
                mag_native_binding b = f->bindings[j];
                MagRenderBuffer *buffer = buffers[batch_resource_index(batch, b.buffer)];
                [encoder setBuffer:buffer->gpu offset:b.offset atIndex:b.slot];
                for (uint32_t k = 0; k < p->binding_count; ++k)
                    if (p->bindings[k].slot == b.slot && p->bindings[k].runtime_array_stride)
                        lengths[p->bindings[k].runtime_size_index] = (uint32_t)b.byte_length;
            }
            if (p->sizes_count) {
                id<MTLBuffer> sizes = [c->device newBufferWithBytes:lengths
                                                             length:p->sizes_count * 4
                                                            options:MTLResourceStorageModeShared];
                if (!sizes) {
                    [encoder endEncoding];
                    return MAG_NATIVE_ALLOCATION_FAILED;
                }
                [retained addObject:sizes];
                [encoder setBuffer:sizes offset:0 atIndex:30];
            }
            [encoder dispatchThreadgroups:MTLSizeMake(f->groups[0], f->groups[1], f->groups[2])
                    threadsPerThreadgroup:p->threads];
            /* Tracked MTLBuffers plus serial encoders establish storage writes
             * before the following compute or render vertex/index reads. */
            [encoder endEncoding];
        } else {
            const mag_native_indexed_draw *d = command.draw;
            MagRenderPipeline *p = find_pipeline(c, d->pipeline);
            MTLTextureDescriptor *color_desc =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                   width:d->width
                                                                  height:d->height
                                                               mipmapped:NO];
            color_desc.usage = MTLTextureUsageRenderTarget;
            color_desc.storageMode = MTLStorageModePrivate;
            MTLTextureDescriptor *depth_desc =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                   width:d->width
                                                                  height:d->height
                                                               mipmapped:NO];
            depth_desc.usage = MTLTextureUsageRenderTarget;
            depth_desc.storageMode = MTLStorageModePrivate;
            id<MTLTexture> color = [c->device newTextureWithDescriptor:color_desc],
                           depth = [c->device newTextureWithDescriptor:depth_desc];
            if (!color || !depth)
                return MAG_NATIVE_ALLOCATION_FAILED;
            [retained addObject:color];
            [retained addObject:depth];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = color;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor =
                MTLClearColorMake(d->clear_color[0], d->clear_color[1], d->clear_color[2], d->clear_color[3]);
            pass.depthAttachment.texture = depth;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.storeAction = MTLStoreActionDontCare;
            pass.depthAttachment.clearDepth = d->clear_depth;
            id<MTLRenderCommandEncoder> encoder =
                [submission->command renderCommandEncoderWithDescriptor:pass];
            if (!encoder)
                return MAG_NATIVE_DEVICE_FAILED;
            [encoder setRenderPipelineState:p->graphics];
            [encoder setDepthStencilState:p->depth];
            [encoder setCullMode:MTLCullModeNone];
            MagRenderBuffer *v = buffers[batch_resource_index(batch, d->vertices)],
                            *i = buffers[batch_resource_index(batch, d->indices)];
            [encoder setVertexBuffer:v->gpu offset:d->vertex_offset atIndex:0];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:d->index_count
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:i->gpu
                         indexBufferOffset:d->index_offset];
            [encoder endEncoding];
            if (!d->color_readback.id)
                continue;
            id<MTLBlitCommandEncoder> blit = [submission->command blitCommandEncoder];
            if (!blit)
                return MAG_NATIVE_DEVICE_FAILED;
            MagRenderBuffer *readback = buffers[batch_resource_index(batch, d->color_readback)];
            uint32_t pitch = ((d->width * 4 + 255) / 256) * 256;
            id<MTLBuffer> padded = [c->device newBufferWithLength:(NSUInteger)pitch * d->height
                                                          options:MTLResourceStorageModeShared];
            if (!padded) {
                [blit endEncoding];
                return MAG_NATIVE_ALLOCATION_FAILED;
            }
            submission->packed_source = padded;
            submission->packed_destination = readback;
            submission->packed_width = d->width;
            submission->packed_height = d->height;
            submission->packed_pitch = pitch;
            [blit copyFromTexture:color
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(d->width, d->height, 1)
                                toBuffer:padded
                       destinationOffset:0
                  destinationBytesPerRow:pitch
                destinationBytesPerImage:(NSUInteger)pitch * d->height];
            [blit endEncoding];
        }
    }
    submission->ticket = (mag_native_ticket){c->generation, ++c->next_submission};
    for (MagRenderUse *use in uses) {
        if (use->write) {
            ++use->buffer->writers;
            use->buffer->gpu_written = 1;
        } else
            ++use->buffer->readers;
    }
    [c->pending addObject:submission];
    *out = submission->ticket;
    @try {
        [submission->command commit];
    } @catch (NSException *error) {
        (void)error;
        if (submission->command.status >= MTLCommandBufferStatusCommitted)
            return MAG_NATIVE_INDETERMINATE;
        release_uses(submission);
        [c->pending removeObjectIdenticalTo:submission];
        memset(out, 0, sizeof(*out));
        return MAG_NATIVE_DEVICE_FAILED;
    }
    return MAG_NATIVE_OK;
}
static int metal_submit_batch(void *opaque, const mag_native_batch *batch, mag_native_ticket *out) {
    MagRenderMetal *c = context(opaque);
    int status = c->host.try_begin(c->host.userdata, c->host.lifetime);
    if (status)
        return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
    @try {
        return metal_batch_owned(opaque, batch, out);
    } @catch (NSException *error) {
        (void)error;
        return out->submission_id ? MAG_NATIVE_INDETERMINATE : MAG_NATIVE_DEVICE_FAILED;
    } @finally {
        c->host.end(c->host.userdata, c->host.lifetime);
    }
}
