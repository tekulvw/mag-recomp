/* Frontend-owned explicit multipass scene encoder. All preflight is shared
 * with the other APIs; actual native resources/pipelines qualify every ref. */
#include "mag_render_scene_validate.h"
static int metal_scene_buffer_info(void *opaque, mag_native_buffer ref, mag_native_scene_buffer_info *out) {
    MagRenderBuffer *b = find_buffer(context(opaque), ref);
    if (!b)
        return MAG_NATIVE_STALE;
    *out =
        (mag_native_scene_buffer_info){b->gpu.length, b->readers || b->writers, b->gpu_written, b->seed_zero};
    return MAG_NATIVE_OK;
}
static int metal_scene_compute_info(void *opaque, mag_native_pipeline ref,
                                    mag_native_scene_compute_info *out) {
    MagRenderPipeline *p = find_pipeline(context(opaque), ref);
    if (!p)
        return MAG_NATIVE_STALE;
    if (!p->gpu)
        return MAG_NATIVE_INVALID;
    *out = (mag_native_scene_compute_info){
        p->bindings,
        p->binding_count,
        {(uint32_t)p->threads.width, (uint32_t)p->threads.height, (uint32_t)p->threads.depth}};
    return MAG_NATIVE_OK;
}
static int metal_scene_graphics_info(void *opaque, mag_native_pipeline ref,
                                     const mag_native_scene_graphics_desc **out,uint32_t *model) {
    MagRenderPipeline *p = find_pipeline(context(opaque), ref);
    if (!p)
        return MAG_NATIVE_STALE;
    if (!p->graphics || !p->scene)
        return MAG_NATIVE_INVALID;
    *out = &p->scene_desc;*model=p->scene_binding_model;
    return MAG_NATIVE_OK;
}
static id<MTLFunction> metal_scene_function(MagRenderMetal *c, const mag_native_shader_stage *s) {
    if (s->language != MAG_NATIVE_MSL)
        return nil;
    NSString *source =
        [[NSString alloc] initWithBytes:s->source length:s->source_length encoding:NSUTF8StringEncoding];
    NSString *entry =
        [[NSString alloc] initWithBytes:s->entry length:s->entry_length encoding:NSUTF8StringEncoding];
    if (!source || !entry)
        return nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_0;
    NSError *error = nil;
    id<MTLLibrary> lib = [c->device newLibraryWithSource:source options:options error:&error];
    if (!lib)
        return nil;
    return [lib newFunctionWithName:entry];
}
static int metal_scene_graphics_create_layout(void *opaque, const mag_native_scene_graphics_desc *desc,
                                       const mag_native_vertex_attribute *attributes,uint32_t attribute_count,uint32_t binding_model,mag_native_pipeline *out) {
    MagRenderMetal *c = context(opaque);
    if (c->next_pipeline == UINT64_MAX)
        return MAG_NATIVE_ALLOCATION_FAILED;
    id<MTLFunction> vertex = metal_scene_function(c, &desc->vertex), fragment = nil;
    if (!vertex)
        return MAG_NATIVE_UNSUPPORTED;
    if (desc->fragment.source_length) {
        fragment = metal_scene_function(c, &desc->fragment);
        if (!fragment)
            return MAG_NATIVE_UNSUPPORTED;
    }
    MTLVertexDescriptor *v = [MTLVertexDescriptor new];
    const MTLVertexFormat formats[]={MTLVertexFormatInvalid,MTLVertexFormatFloat,MTLVertexFormatFloat2,MTLVertexFormatFloat3,MTLVertexFormatFloat4,MTLVertexFormatUChar4Normalized};
    for(uint32_t i=0;i<attribute_count;++i){const mag_native_vertex_attribute *a=&attributes[i];
        v.attributes[a->location].format=formats[a->format];v.attributes[a->location].offset=a->offset;v.attributes[a->location].bufferIndex=16;}
    v.layouts[16].stride = desc->vertex_stride;
    v.layouts[16].stepFunction = MTLVertexStepFunctionPerVertex;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vertex;
    pd.fragmentFunction = fragment;
    pd.vertexDescriptor = v;
    if (desc->color_format) {
        pd.colorAttachments[0].pixelFormat = desc->color_format==MAG_NATIVE_SCENE_BGRA8?MTLPixelFormatBGRA8Unorm:MTLPixelFormatRGBA8Unorm;
        if (desc->blend == MAG_NATIVE_SCENE_ALPHA) {
            pd.colorAttachments[0].blendingEnabled = YES;
            pd.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
            pd.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
            pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
    }
    if (desc->depth_format)
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError *error = nil;
    id<MTLRenderPipelineState> graphics = [c->device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!graphics)
        return MAG_NATIVE_UNSUPPORTED;
    MTLDepthStencilDescriptor *depth = [MTLDepthStencilDescriptor new];
    const MTLCompareFunction compares[] = {MTLCompareFunctionAlways, MTLCompareFunctionLess,
                                           MTLCompareFunctionLessEqual, MTLCompareFunctionAlways};
    depth.depthCompareFunction = compares[desc->depth_compare];
    depth.depthWriteEnabled = desc->depth_write;
    id<MTLDepthStencilState> depth_gpu = [c->device newDepthStencilStateWithDescriptor:depth];
    if (!depth_gpu)
        return MAG_NATIVE_ALLOCATION_FAILED;
    MagRenderPipeline *p = [MagRenderPipeline new];
    p->scene = 1;p->scene_binding_model=binding_model;
    p->graphics = graphics;
    p->depth = depth_gpu;
    p->scene_desc = *desc;
    if (desc->buffer_count)
        memcpy(p->scene_buffers, desc->buffers, desc->buffer_count * sizeof(*desc->buffers));
    if (desc->texture_count)
        memcpy(p->scene_textures, desc->textures, desc->texture_count * sizeof(*desc->textures));
    p->scene_desc.buffers = p->scene_buffers;
    p->scene_desc.textures = p->scene_textures;
    // Source storage is caller-owned and no longer consulted after compilation.
    p->scene_desc.vertex.source = NULL;
    p->scene_desc.vertex.entry = NULL;
    p->scene_desc.fragment.source = NULL;
    p->scene_desc.fragment.entry = NULL;
    uint64_t id = ++c->next_pipeline;
    c->pipelines[@(id)] = p;
    *out = (mag_native_pipeline){c->generation, id};
    return MAG_NATIVE_OK;
}
static int metal_scene_graphics_create(void *opaque,const mag_native_scene_graphics_desc *desc,mag_native_pipeline *out){
    int status=mag_native_scene_graphics_validate(desc);if(status)return status;
    const mag_native_vertex_attribute attributes[]={{0,MAG_NATIVE_VERTEX_FLOAT4,0,0}};
    return metal_scene_graphics_create_layout(opaque,desc,attributes,1,0,out);
}
static int metal_scene_graphics_create_v2(void *opaque,const mag_native_scene_graphics_v2_desc *desc,mag_native_pipeline *out){
    int status=mag_native_scene_graphics_v2_validate(desc);if(status)return status;
    return metal_scene_graphics_create_layout(opaque,&desc->base,desc->attributes,desc->attribute_count,desc->binding_model,out);
}
static int metal_scene_owned(void *opaque, const mag_native_scene *scene, mag_native_ticket *out,MagRenderImportedImage *target) {
    MagRenderMetal *c = context(opaque);
    if (c->pending.count >= 2)
        return MAG_NATIVE_BUSY;
    if (c->next_submission == UINT64_MAX)
        return MAG_NATIVE_DEVICE_FAILED;
    mag_native_scene_registry registry = {opaque,
                                          metal_scene_buffer_info,
                                          metal_scene_compute_info,
                                          metal_scene_graphics_info,
                                          4,
                                          4,
                                          UINT32_MAX,
                                          UINT32_MAX};
    mag_native_scene_usage usage;
    int status = mag_native_scene_validate(scene, &registry, &usage);
    if (status)
        return status;
    NSMutableArray *retained = [NSMutableArray new];
    NSMutableArray<MagRenderUse *> *uses = [NSMutableArray new];
    NSMutableArray<id<MTLTexture>> *images = [NSMutableArray new];
    for (uint32_t i = 0; i < scene->resource_count; ++i) {
        MagRenderUse *u = [MagRenderUse new];
        u->buffer = find_buffer(c, scene->resources[i].buffer);
        u->write = usage.write[i];
        [uses addObject:u];
    }
    for (uint32_t i = 0; i < scene->image_count; ++i) {
        const mag_native_scene_image *im = &scene->images[i];
        MTLTextureDescriptor *d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:im->format == MAG_NATIVE_SCENE_RGBA8
                                                                         ? MTLPixelFormatRGBA8Unorm
                                                                         : im->format==MAG_NATIVE_SCENE_BGRA8?MTLPixelFormatBGRA8Unorm:MTLPixelFormatDepth32Float
                                                               width:im->width
                                                              height:im->height
                                                           mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = im->upload.id ? MTLStorageModeShared : MTLStorageModePrivate;
        id<MTLTexture> texture = target&&i==scene->output_image?target->image:[c->device newTextureWithDescriptor:d];
        if(target&&i==scene->output_image)[retained addObject:target];
        if (!texture)
            return MAG_NATIVE_ALLOCATION_FAILED;
        if (im->upload.id) {
            MagRenderBuffer *b = find_buffer(c, im->upload);
            [texture replaceRegion:MTLRegionMake2D(0, 0, im->width, im->height)
                       mipmapLevel:0
                         withBytes:b->gpu.contents
                       bytesPerRow:im->width * 4];
        }
        [images addObject:texture];
        [retained addObject:texture];
    }
    id<MTLSamplerState> samplers[3];
    for (unsigned i = 0; i < 3; ++i) {
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        d.sAddressMode = MTLSamplerAddressModeClampToEdge;
        d.tAddressMode = MTLSamplerAddressModeClampToEdge;
        d.minFilter = d.magFilter =
            i == MAG_NATIVE_SCENE_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.compareFunction =
            i == MAG_NATIVE_SCENE_COMPARE_LEQUAL ? MTLCompareFunctionLessEqual : MTLCompareFunctionAlways;
        samplers[i] = [c->device newSamplerStateWithDescriptor:d];
        if (!samplers[i])
            return MAG_NATIVE_ALLOCATION_FAILED;
        [retained addObject:samplers[i]];
    }
    MagRenderSubmission *submission = [MagRenderSubmission new];
    submission->uses = uses;
    submission->retained = retained;
    // All images, pipeline references and buffers are validated before any queue effect.
    submission->command = [c->queue commandBuffer];
    if (!submission->command)
        return MAG_NATIVE_DEVICE_FAILED;
    for (uint32_t node = 0; node < scene->dispatch_count; ++node) {
        const mag_native_frame *f = &scene->dispatches[node];
        MagRenderPipeline *p = find_pipeline(c, f->pipeline);
        [retained addObject:p];
        id<MTLComputeCommandEncoder> e = [submission->command computeCommandEncoder];
        if (!e)
            return MAG_NATIVE_DEVICE_FAILED;
        [e setComputePipelineState:p->gpu];
        uint32_t lengths[16] = {0};
        for (uint32_t j = 0; j < f->binding_count; ++j) {
            mag_native_binding b = f->bindings[j];
            MagRenderBuffer *buffer = find_buffer(c, b.buffer);
            [e setBuffer:buffer->gpu offset:b.offset atIndex:b.slot];
            for (uint32_t k = 0; k < p->binding_count; ++k)
                if (p->bindings[k].slot == b.slot && p->bindings[k].runtime_array_stride)
                    lengths[p->bindings[k].runtime_size_index] = (uint32_t)b.byte_length;
        }
        if (p->sizes_count) {
            id<MTLBuffer> sizes = [c->device newBufferWithBytes:lengths
                                                         length:p->sizes_count * 4
                                                        options:MTLResourceStorageModeShared];
            if (!sizes) {
                [e endEncoding];
                return MAG_NATIVE_ALLOCATION_FAILED;
            }
            [retained addObject:sizes];
            [e setBuffer:sizes offset:0 atIndex:30];
        }
        [e dispatchThreadgroups:MTLSizeMake(f->groups[0], f->groups[1], f->groups[2])
            threadsPerThreadgroup:p->threads];
        [e endEncoding];
    }
    for (uint32_t n = 0; n < scene->pass_count; ++n) {
        const mag_native_scene_pass *p = &scene->passes[n];
        MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
        if (p->color.image != MAG_NATIVE_SCENE_NONE) {
            pd.colorAttachments[0].texture = images[p->color.image];
            pd.colorAttachments[0].loadAction =
                p->color.load == MAG_NATIVE_SCENE_CLEAR ? MTLLoadActionClear : MTLLoadActionLoad;
            pd.colorAttachments[0].storeAction =
                p->color.store == MAG_NATIVE_SCENE_STORE ? MTLStoreActionStore : MTLStoreActionDontCare;
            pd.colorAttachments[0].clearColor =
                MTLClearColorMake(p->color.clear[0], p->color.clear[1], p->color.clear[2], p->color.clear[3]);
        }
        if (p->depth.image != MAG_NATIVE_SCENE_NONE) {
            pd.depthAttachment.texture = images[p->depth.image];
            pd.depthAttachment.loadAction =
                p->depth.load == MAG_NATIVE_SCENE_CLEAR ? MTLLoadActionClear : MTLLoadActionLoad;
            pd.depthAttachment.storeAction =
                p->depth.store == MAG_NATIVE_SCENE_STORE ? MTLStoreActionStore : MTLStoreActionDontCare;
            pd.depthAttachment.clearDepth = p->depth.clear[0];
        }
        id<MTLRenderCommandEncoder> e = [submission->command renderCommandEncoderWithDescriptor:pd];
        if (!e)
            return MAG_NATIVE_DEVICE_FAILED;
        [e setCullMode:MTLCullModeNone];
        for (uint32_t j = 0; j < p->draw_count; ++j) {
            const mag_native_scene_draw *d = &p->draws[j];
            MagRenderPipeline *pipeline = find_pipeline(c, d->pipeline);
            [retained addObject:pipeline];
            [e setRenderPipelineState:pipeline->graphics];
            [e setDepthStencilState:pipeline->depth];
            MagRenderBuffer *v = find_buffer(c, d->vertices), *i = find_buffer(c, d->indices);
            [e setVertexBuffer:v->gpu offset:d->vertex_offset atIndex:16];
            for (uint32_t k = 0; k < d->buffer_count; ++k) {
                mag_native_scene_buffer_binding b = d->buffers[k];
                MagRenderBuffer *buffer = find_buffer(c, b.binding.buffer);
                if (b.stage == MAG_NATIVE_SCENE_VERTEX)
                    [e setVertexBuffer:buffer->gpu offset:b.binding.offset atIndex:b.binding.slot];
                else
                    [e setFragmentBuffer:buffer->gpu offset:b.binding.offset atIndex:b.binding.slot];
            }
            for (uint32_t k = 0; k < d->texture_count; ++k) {
                mag_native_scene_texture_binding t = d->textures[k];
                if (t.stage == MAG_NATIVE_SCENE_VERTEX) {
                    [e setVertexTexture:images[t.image] atIndex:t.texture_slot];
                    [e setVertexSamplerState:samplers[t.sampler] atIndex:t.sampler_slot];
                } else {
                    [e setFragmentTexture:images[t.image] atIndex:t.texture_slot];
                    [e setFragmentSamplerState:samplers[t.sampler] atIndex:t.sampler_slot];
                }
            }
            [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:d->index_count
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:i->gpu
                   indexBufferOffset:d->index_offset];
        }
        [e endEncoding];
    }
    if (scene->diagnostic_readback.id) {
        const mag_native_scene_image *im = &scene->images[scene->output_image];
        uint32_t pitch = (im->width * 4 + 255) & ~255u;
        id<MTLBuffer> padded = [c->device newBufferWithLength:(NSUInteger)pitch * im->height
                                                      options:MTLResourceStorageModeShared];
        if (!padded)
            return MAG_NATIVE_ALLOCATION_FAILED;
        submission->packed_source = padded;
        submission->packed_destination = find_buffer(c, scene->diagnostic_readback);
        submission->packed_width = im->width;
        submission->packed_height = im->height;
        submission->packed_pitch = pitch;
        id<MTLBlitCommandEncoder> blit = [submission->command blitCommandEncoder];
        if (!blit)
            return MAG_NATIVE_DEVICE_FAILED;
        [blit copyFromTexture:images[scene->output_image]
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(im->width, im->height, 1)
                            toBuffer:padded
                   destinationOffset:0
              destinationBytesPerRow:pitch
            destinationBytesPerImage:(NSUInteger)pitch * im->height];
        [blit endEncoding];
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
    } @catch (NSException *e) {
        (void)e;
        if (submission->command.status >= MTLCommandBufferStatusCommitted)
            return MAG_NATIVE_INDETERMINATE;
        release_uses(submission);
        [c->pending removeObjectIdenticalTo:submission];
        memset(out, 0, sizeof(*out));
        return MAG_NATIVE_DEVICE_FAILED;
    }
    return MAG_NATIVE_OK;
}
static int metal_submit_scene(void *opaque, const mag_native_scene *scene, mag_native_ticket *out) {
    MagRenderMetal *c = context(opaque);
    int status = c->host.try_begin(c->host.userdata, c->host.lifetime);
    if (status)
        return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
    @try {
        return metal_scene_owned(opaque, scene, out,nil);
    } @catch (NSException *e) {
        (void)e;
        return out->submission_id ? MAG_NATIVE_INDETERMINATE : MAG_NATIVE_DEVICE_FAILED;
    } @finally {
        c->host.end(c->host.userdata, c->host.lifetime);
    }
}

static int metal_image_import(void *opaque,const MagRenderHostImage *host,mag_native_image *out){
 MagRenderMetal *c=context(opaque);
 if(host->backend!=MAG_RENDER_HOST_METAL||host->owner_lifetime!=c->host.lifetime)return MAG_NATIVE_STALE;
 if(c->images.count>=64||c->next_image==UINT64_MAX)return MAG_NATIVE_ALLOCATION_FAILED;
 int status=host->retain(host->userdata,host->owner_lifetime,host->resource_id,host->resource_generation);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
 id<MTLTexture> image=(__bridge id<MTLTexture>)host->image;
 if(!image||image.device!=c->device||image.width!=host->width||image.height!=host->height||!host->width||!host->height||!(image.usage&MTLTextureUsageRenderTarget)||image.textureType!=MTLTextureType2D||image.sampleCount!=1||(host->format!=MAG_NATIVE_SCENE_RGBA8&&host->format!=MAG_NATIVE_SCENE_BGRA8)||image.pixelFormat!=(host->format==MAG_NATIVE_SCENE_BGRA8?MTLPixelFormatBGRA8Unorm:MTLPixelFormatRGBA8Unorm)){host->release(host->userdata,host->owner_lifetime,host->resource_id,host->resource_generation);return MAG_NATIVE_UNSUPPORTED;}
 MagRenderImportedImage *record=[MagRenderImportedImage new];record->image=image;record->host=*host;uint64_t id=++c->next_image;c->images[@(id)]=record;*out=(mag_native_image){c->generation,id};return MAG_NATIVE_OK;
}
static int metal_image_release(void *opaque,mag_native_image ref){
 MagRenderMetal *c=context(opaque);if(ref.device_generation!=c->generation||!ref.id||!c->images[@(ref.id)])return MAG_NATIVE_STALE;[c->images removeObjectForKey:@(ref.id)];return MAG_NATIVE_OK;
}
static int metal_submit_scene_target(void *opaque,const mag_native_scene *scene,mag_native_image ref,mag_native_ticket *out){
 MagRenderMetal *c=context(opaque);if(ref.device_generation!=c->generation||!ref.id)return MAG_NATIVE_STALE;
 MagRenderImportedImage *target=c->images[@(ref.id)];if(!target)return MAG_NATIVE_STALE;
 int status=c->host.try_begin(c->host.userdata,c->host.lifetime);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
 @try{
  status=target->host.validate(target->host.userdata,target->host.owner_lifetime,target->host.resource_id,target->host.resource_generation);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
  if(!scene->images||scene->output_image>=scene->image_count)return MAG_NATIVE_INVALID;
  const mag_native_scene_image *im=&scene->images[scene->output_image];if(im->format!=target->host.format||im->width!=target->host.width||im->height!=target->host.height||im->upload.id||im->upload.device_generation||im->upload.version)return MAG_NATIVE_INVALID;
  return metal_scene_owned(opaque,scene,out,target);
 }@catch(NSException *e){(void)e;return out->submission_id?MAG_NATIVE_INDETERMINATE:MAG_NATIVE_DEVICE_FAILED;}@finally{c->host.end(c->host.userdata,c->host.lifetime);}
}

static int metal_submit_scene_frame(void *opaque,const mag_native_scene *scene,mag_native_image ref,const MagRenderHostFrame *frame,mag_native_ticket *out){
 MagRenderMetal *c=context(opaque);if(frame->owner_lifetime!=c->host.lifetime||ref.device_generation!=c->generation||!ref.id)return MAG_NATIVE_STALE;
 MagRenderImportedImage *target=c->images[@(ref.id)];if(!target)return MAG_NATIVE_STALE;
 int status=c->host.try_begin(c->host.userdata,c->host.lifetime);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
 int reserved=0;
 @try{
  status=target->host.validate(target->host.userdata,target->host.owner_lifetime,target->host.resource_id,target->host.resource_generation);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
  status=frame->preflight(frame->userdata,frame->owner_lifetime,frame->reservation,target->host.resource_id,target->host.resource_generation);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;reserved=1;
  if(!scene->images||scene->output_image>=scene->image_count){frame->cancel(frame->userdata,frame->owner_lifetime,frame->reservation);return MAG_NATIVE_INVALID;}
  const mag_native_scene_image *im=&scene->images[scene->output_image];if(im->format!=target->host.format||im->width!=target->host.width||im->height!=target->host.height||im->upload.id||im->upload.device_generation||im->upload.version){frame->cancel(frame->userdata,frame->owner_lifetime,frame->reservation);return MAG_NATIVE_INVALID;}
  status=metal_scene_owned(opaque,scene,out,target);
  if(status<0){frame->cancel(frame->userdata,frame->owner_lifetime,frame->reservation);return status;}
  int publication=frame->committed(frame->userdata,frame->owner_lifetime,frame->reservation,out->device_generation,out->submission_id,status?1u:0u);
  return publication?MAG_NATIVE_INDETERMINATE:status;
 }@catch(NSException *error){(void)error;
  if(out->submission_id){if(reserved)frame->committed(frame->userdata,frame->owner_lifetime,frame->reservation,out->device_generation,out->submission_id,1);return MAG_NATIVE_INDETERMINATE;}
  if(reserved)frame->cancel(frame->userdata,frame->owner_lifetime,frame->reservation);return MAG_NATIVE_DEVICE_FAILED;
 }@finally{c->host.end(c->host.userdata,c->host.lifetime);}
}
