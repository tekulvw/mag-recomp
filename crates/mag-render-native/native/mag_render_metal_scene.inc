static MTLPixelFormat metal_scene_format(uint32_t f){
 const MTLPixelFormat formats[]={MTLPixelFormatInvalid,MTLPixelFormatRGBA8Unorm,MTLPixelFormatDepth32Float,MTLPixelFormatBGRA8Unorm,
 MTLPixelFormatR8Unorm,MTLPixelFormatRG8Unorm,MTLPixelFormatBC1_RGBA,MTLPixelFormatBC2_RGBA,MTLPixelFormatBC3_RGBA,
 MTLPixelFormatR16Unorm,MTLPixelFormatRG16Unorm,MTLPixelFormatRGBA16Float,MTLPixelFormatRGBA32Float,MTLPixelFormatR32Float,MTLPixelFormatRG16Float,MTLPixelFormatDepth24Unorm_Stencil8,MTLPixelFormatDepth32Float_Stencil8};
 return f<sizeof(formats)/sizeof(formats[0])?formats[f]:MTLPixelFormatInvalid;
}
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
                                       const mag_native_vertex_attribute *attributes,uint32_t attribute_count,uint32_t binding_model,const mag_native_raster_state *raster,mag_native_pipeline *out) {
    MagRenderMetal *c = context(opaque);
    if(desc->depth_format==MAG_NATIVE_SCENE_D24S8&&![c->device isDepth24Stencil8PixelFormatSupported])return MAG_NATIVE_UNSUPPORTED;
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
        pd.colorAttachments[0].pixelFormat = metal_scene_format(desc->color_format);
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
    if (raster && desc->color_format) {
        const MTLBlendFactor factors[]={MTLBlendFactorZero,MTLBlendFactorOne,MTLBlendFactorSourceColor,MTLBlendFactorOneMinusSourceColor,
            MTLBlendFactorDestinationColor,MTLBlendFactorOneMinusDestinationColor,MTLBlendFactorSourceAlpha,MTLBlendFactorOneMinusSourceAlpha,
            MTLBlendFactorDestinationAlpha,MTLBlendFactorOneMinusDestinationAlpha,MTLBlendFactorBlendColor,MTLBlendFactorOneMinusBlendColor,
            MTLBlendFactorBlendAlpha,MTLBlendFactorOneMinusBlendAlpha,MTLBlendFactorSourceAlphaSaturated};
        const MTLBlendOperation operations[]={MTLBlendOperationAdd,MTLBlendOperationSubtract,MTLBlendOperationReverseSubtract,MTLBlendOperationMin,MTLBlendOperationMax};
        MTLRenderPipelineColorAttachmentDescriptor *color=pd.colorAttachments[0];
        color.blendingEnabled=raster->blend_enabled;
        color.rgbBlendOperation=operations[raster->rgb_equation];color.alphaBlendOperation=operations[raster->alpha_equation];
        color.sourceRGBBlendFactor=factors[raster->src_rgb];color.destinationRGBBlendFactor=factors[raster->dst_rgb];
        color.sourceAlphaBlendFactor=factors[raster->src_alpha];color.destinationAlphaBlendFactor=factors[raster->dst_alpha];
        color.writeMask=((raster->color_write_mask&1)?MTLColorWriteMaskRed:0)|((raster->color_write_mask&2)?MTLColorWriteMaskGreen:0)
          |((raster->color_write_mask&4)?MTLColorWriteMaskBlue:0)|((raster->color_write_mask&8)?MTLColorWriteMaskAlpha:0);
    }
    if (desc->depth_format)
        pd.depthAttachmentPixelFormat = metal_scene_format(desc->depth_format);
    if(mag_native_image_has_stencil(desc->depth_format))pd.stencilAttachmentPixelFormat=metal_scene_format(desc->depth_format);
    NSError *error = nil;
    id<MTLRenderPipelineState> graphics = [c->device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!graphics)
        return MAG_NATIVE_UNSUPPORTED;
    MTLDepthStencilDescriptor *depth = [MTLDepthStencilDescriptor new];
    const MTLCompareFunction compares[] = {MTLCompareFunctionAlways, MTLCompareFunctionLess,
                                           MTLCompareFunctionLessEqual, MTLCompareFunctionAlways};
    depth.depthCompareFunction = compares[desc->depth_compare];
    depth.depthWriteEnabled = desc->depth_write;
    if(raster){
        const MTLCompareFunction full_compares[]={MTLCompareFunctionNever,MTLCompareFunctionLess,MTLCompareFunctionEqual,MTLCompareFunctionLessEqual,
            MTLCompareFunctionGreater,MTLCompareFunctionNotEqual,MTLCompareFunctionGreaterEqual,MTLCompareFunctionAlways};
        depth.depthCompareFunction=raster->depth_enabled&&desc->depth_format?full_compares[raster->depth_compare]:MTLCompareFunctionAlways;
        depth.depthWriteEnabled=raster->depth_enabled&&desc->depth_format&&raster->depth_write;
    }
    if(raster&&raster->stencil_enabled){
        const MTLCompareFunction compares[]={MTLCompareFunctionNever,MTLCompareFunctionLess,MTLCompareFunctionEqual,MTLCompareFunctionLessEqual,MTLCompareFunctionGreater,MTLCompareFunctionNotEqual,MTLCompareFunctionGreaterEqual,MTLCompareFunctionAlways};
        const MTLStencilOperation ops[]={MTLStencilOperationKeep,MTLStencilOperationZero,MTLStencilOperationReplace,MTLStencilOperationIncrementClamp,MTLStencilOperationDecrementClamp,MTLStencilOperationInvert,MTLStencilOperationIncrementWrap,MTLStencilOperationDecrementWrap};
        const mag_native_stencil_face *faces[]={&raster->front_stencil,&raster->back_stencil};
        for(unsigned i=0;i<2;++i){const mag_native_stencil_face *f=faces[i];MTLStencilDescriptor *stencil=[MTLStencilDescriptor new];stencil.stencilCompareFunction=compares[f->compare];stencil.stencilFailureOperation=ops[f->fail];stencil.depthFailureOperation=ops[f->depth_fail];stencil.depthStencilPassOperation=ops[f->pass];stencil.readMask=f->read_mask&255u;stencil.writeMask=f->write_mask&255u;if(i)depth.backFaceStencil=stencil;else depth.frontFaceStencil=stencil;}
    }
    id<MTLDepthStencilState> depth_gpu = [c->device newDepthStencilStateWithDescriptor:depth];
    if (!depth_gpu)
        return MAG_NATIVE_ALLOCATION_FAILED;
    MagRenderPipeline *p = [MagRenderPipeline new];
    p->scene = 1;p->scene_binding_model=binding_model;
    if(raster){p->explicit_raster=1;p->raster=*raster;}
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
    return metal_scene_graphics_create_layout(opaque,desc,attributes,1,0,NULL,out);
}
static int metal_scene_graphics_create_v2(void *opaque,const mag_native_scene_graphics_v2_desc *desc,mag_native_pipeline *out){
    int status=mag_native_scene_graphics_v2_validate(desc);if(status)return status;
    return metal_scene_graphics_create_layout(opaque,&desc->base,desc->attributes,desc->attribute_count,desc->binding_model,NULL,out);
}
static int metal_scene_graphics_create_v3(void *opaque,const mag_native_scene_graphics_v3_desc *desc,mag_native_pipeline *out){
    int status=mag_native_scene_graphics_v3_validate(desc);if(status)return status;
    mag_native_scene_graphics_desc schema;mag_native_scene_graphics_v3_schema(desc,&schema);
    return metal_scene_graphics_create_layout(opaque,&schema,desc->base.attributes,desc->base.attribute_count,desc->base.binding_model,&desc->raster,out);
}
static int metal_scene_raster_info(void *opaque,mag_native_pipeline ref,const mag_native_raster_state **out){
 MagRenderPipeline *p=find_pipeline(context(opaque),ref);if(!p)return MAG_NATIVE_STALE;if(!p->scene)return MAG_NATIVE_INVALID;*out=p->explicit_raster?&p->raster:NULL;return MAG_NATIVE_OK;
}
static int metal_scene_owned(void *opaque, const mag_native_scene *scene, mag_native_ticket *out,MagRenderImportedImage *target,const mag_native_sampler *sampler_table,uint32_t sampler_count,const mag_native_scene_v3 *v3,const mag_native_scene_v4 *v4) {
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
    mag_native_scene_registry_v2 registry_v2={&registry,sampler_table,sampler_count,metal_sampler_info};
    mag_native_scene_registry_v4 registry_v4={&registry_v2,metal_scene_raster_info};
    int status=v4?mag_native_scene_validate_v4(v4,&registry_v4,&usage):v3?mag_native_scene_validate_v3(v3,&registry_v2,&usage):mag_native_scene_validate_v2(scene,&registry_v2,&usage);
    if (status)
        return status;
    NSMutableArray *retained = [NSMutableArray new];
    for(uint32_t i=0;i<sampler_count;++i)if(usage.samplers[i]){
        MagRenderSamplerPin *pin=[MagRenderSamplerPin new];pin->registry=&c->samplers;
        status=mag_native_sampler_registry_pin(&c->samplers,sampler_table[i],&pin->entry);
        if(status)return status;
        [retained addObject:pin];
    }
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
        const mag_native_scene_image_layout *shape=v3&&v3->image_layout_count?&v3->image_layouts[i]:NULL;
        mag_native_image_format info;mag_native_image_format_info(im->format,&info);
        if(info.block_width>1&&!c->device.supportsBCTextureCompression)return MAG_NATIVE_UNSUPPORTED;
        // Source depth images are native render targets. Uploading host depth
        // requires a separate private-texture staging path, not replaceRegion.
        if((im->format==MAG_NATIVE_SCENE_D32||mag_native_image_has_stencil(im->format))&&im->upload.id)return MAG_NATIVE_UNSUPPORTED;
        MTLTextureDescriptor *d=[MTLTextureDescriptor new];
        if(im->format==MAG_NATIVE_SCENE_D24S8&&![c->device isDepth24Stencil8PixelFormatSupported])return MAG_NATIVE_UNSUPPORTED;
        d.pixelFormat=metal_scene_format(im->format);d.width=im->width;d.height=im->height;d.depth=1;
        d.textureType=shape&&shape->dimension==4?MTLTextureTypeCube:MTLTextureType2D;
        d.mipmapLevelCount=shape?shape->mip_count:1;d.arrayLength=1;d.sampleCount=1;
        d.usage=MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
        if(info.color_attachment||info.depth_attachment)d.usage|=MTLTextureUsageRenderTarget;
        d.storageMode=im->upload.id?MTLStorageModeShared:MTLStorageModePrivate;
        id<MTLTexture> texture=target&&i==scene->output_image?target->image:[c->device newTextureWithDescriptor:d];
        if(target&&i==scene->output_image)[retained addObject:target];
        if(!texture)return MAG_NATIVE_ALLOCATION_FAILED;
        if(im->upload.id){
            MagRenderBuffer *b=find_buffer(c,im->upload);
            if(shape){
                for(uint32_t n=0;n<shape->subresource_count;++n){const mag_native_image_subresource *level=&shape->subresources[n];
                    [texture replaceRegion:MTLRegionMake2D(0,0,level->width,level->height) mipmapLevel:level->mip slice:level->layer
                        withBytes:(const uint8_t*)b->gpu.contents+level->offset bytesPerRow:level->row_bytes bytesPerImage:level->length];
                }
            }else [texture replaceRegion:MTLRegionMake2D(0,0,im->width,im->height) mipmapLevel:0 withBytes:b->gpu.contents bytesPerRow:im->width*4];
        }
        [images addObject:texture];
        [retained addObject:texture];
    }
    NSMutableArray<id<MTLTexture>> *views=[NSMutableArray new];
    if(v3)for(uint32_t i=0;i<v3->view_count;++i){
        const mag_native_image_view *v=&v3->views[i];id<MTLTexture> parent=images[v->image];id<MTLTexture> view=nil;
        const MTLTextureSwizzle components[]={MTLTextureSwizzleRed,MTLTextureSwizzleGreen,MTLTextureSwizzleBlue,MTLTextureSwizzleAlpha,MTLTextureSwizzleZero,MTLTextureSwizzleOne};
        if(@available(macOS 10.15,*)){
            MTLTextureSwizzleChannels swizzle=MTLTextureSwizzleChannelsMake(components[v->remap[0]],components[v->remap[1]],components[v->remap[2]],components[v->remap[3]]);
            view=[parent newTextureViewWithPixelFormat:parent.pixelFormat textureType:v->dimension==4?MTLTextureTypeCube:MTLTextureType2D
                levels:NSMakeRange(v->base_mip,v->mip_count) slices:NSMakeRange(v->base_layer,v->layer_count) swizzle:swizzle];
        }else return MAG_NATIVE_UNSUPPORTED;
        if(!view)return MAG_NATIVE_UNSUPPORTED;[views addObject:view];[retained addObject:view];
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
                p->color.load == MAG_NATIVE_SCENE_CLEAR ? MTLLoadActionClear : p->color.load==MAG_NATIVE_SCENE_DISCARD_LOAD?MTLLoadActionDontCare:MTLLoadActionLoad;
            pd.colorAttachments[0].storeAction =
                p->color.store == MAG_NATIVE_SCENE_STORE ? MTLStoreActionStore : MTLStoreActionDontCare;
            pd.colorAttachments[0].clearColor =
                MTLClearColorMake(p->color.clear[0], p->color.clear[1], p->color.clear[2], p->color.clear[3]);
        }
        if (p->depth.image != MAG_NATIVE_SCENE_NONE) {
            pd.depthAttachment.texture = images[p->depth.image];
            pd.depthAttachment.loadAction =
                p->depth.load == MAG_NATIVE_SCENE_CLEAR ? MTLLoadActionClear : p->depth.load==MAG_NATIVE_SCENE_DISCARD_LOAD?MTLLoadActionDontCare:MTLLoadActionLoad;
            pd.depthAttachment.storeAction =
                p->depth.store == MAG_NATIVE_SCENE_STORE ? MTLStoreActionStore : MTLStoreActionDontCare;
            pd.depthAttachment.clearDepth = p->depth.clear[0];
            if(v4&&mag_native_image_has_stencil(scene->images[p->depth.image].format)){
                const mag_native_stencil_attachment *ops=&v4->stencils[n];
                pd.stencilAttachment.texture=images[p->depth.image];
                pd.stencilAttachment.loadAction=ops->load==0?MTLLoadActionClear:ops->load==1?MTLLoadActionLoad:MTLLoadActionDontCare;
                pd.stencilAttachment.storeAction=ops->store==0?MTLStoreActionStore:MTLStoreActionDontCare;pd.stencilAttachment.clearStencil=ops->clear_value;
            }
        }
        id<MTLRenderCommandEncoder> e = [submission->command renderCommandEncoderWithDescriptor:pd];
        if (!e)
            return MAG_NATIVE_DEVICE_FAILED;
        [e setCullMode:MTLCullModeNone];
        if(v3&&v3->viewport_count){const mag_native_viewport *v=&v3->viewports[n];
            [e setViewport:(MTLViewport){v->x,v->y,v->width,v->height,v->min_depth,v->max_depth}];
            if(!v->scissor_width||!v->scissor_height){[e endEncoding];continue;}
            [e setScissorRect:(MTLScissorRect){v->scissor_x,v->scissor_y,v->scissor_width,v->scissor_height}];
        }
        for (uint32_t j = 0; j < p->draw_count; ++j) {
            const mag_native_scene_draw *d = &p->draws[j];
            MagRenderPipeline *pipeline = find_pipeline(c, d->pipeline);
            [retained addObject:pipeline];
            [e setRenderPipelineState:pipeline->graphics];
            [e setDepthStencilState:pipeline->depth];
            if(pipeline->explicit_raster){
                const MTLCullMode cull[]={MTLCullModeNone,MTLCullModeFront,MTLCullModeBack};
                [e setCullMode:cull[pipeline->raster.cull_mode]];
                if(pipeline->raster.stencil_enabled)[e setStencilFrontReferenceValue:pipeline->raster.front_stencil.reference&255u backReferenceValue:pipeline->raster.back_stencil.reference&255u];
                [e setFrontFacingWinding:pipeline->raster.front_face?MTLWindingClockwise:MTLWindingCounterClockwise];
                [e setBlendColorRed:pipeline->raster.blend_constant[0] green:pipeline->raster.blend_constant[1]
                    blue:pipeline->raster.blend_constant[2] alpha:pipeline->raster.blend_constant[3]];
            }else{
                [e setCullMode:MTLCullModeNone];
                [e setFrontFacingWinding:MTLWindingClockwise];
            }
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
                id<MTLSamplerState> chosen_sampler=nil;
                if(t.sampler&MAG_NATIVE_SCENE_SAMPLER_TABLE_BIT){
                    mag_native_sampler_entry *entry=mag_native_sampler_registry_find(&c->samplers,sampler_table[MAG_NATIVE_SCENE_SAMPLER_TABLE_INDEX(t.sampler)]);
                    chosen_sampler=(__bridge id<MTLSamplerState>)entry->payload;
                    /* The submission's registry pin owns this exact native object. */
                }else chosen_sampler=samplers[t.sampler];
                if (t.stage == MAG_NATIVE_SCENE_VERTEX) {
                    [e setVertexTexture:((t.image&MAG_NATIVE_SCENE_VIEW_TABLE_BIT)?views[MAG_NATIVE_SCENE_VIEW_TABLE_INDEX(t.image)]:images[t.image]) atIndex:t.texture_slot];
                    [e setVertexSamplerState:chosen_sampler atIndex:t.sampler_slot];
                } else {
                    [e setFragmentTexture:((t.image&MAG_NATIVE_SCENE_VIEW_TABLE_BIT)?views[MAG_NATIVE_SCENE_VIEW_TABLE_INDEX(t.image)]:images[t.image]) atIndex:t.texture_slot];
                    [e setFragmentSamplerState:chosen_sampler atIndex:t.sampler_slot];
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
static int metal_submit_scene_impl(void *opaque, const mag_native_scene *scene, mag_native_ticket *out,const mag_native_sampler *sampler_table,uint32_t sampler_count,const mag_native_scene_v3 *v3,const mag_native_scene_v4 *v4) {
    MagRenderMetal *c = context(opaque);
    int status = c->host.try_begin(c->host.userdata, c->host.lifetime);
    if (status)
        return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
    @try {
        return metal_scene_owned(opaque, scene, out,nil,sampler_table,sampler_count,v3,v4);
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
static int metal_submit_scene_target_impl(void *opaque,const mag_native_scene *scene,mag_native_image ref,mag_native_ticket *out,const mag_native_sampler *sampler_table,uint32_t sampler_count,const mag_native_scene_v3 *v3,const mag_native_scene_v4 *v4){
 MagRenderMetal *c=context(opaque);if(ref.device_generation!=c->generation||!ref.id)return MAG_NATIVE_STALE;
 MagRenderImportedImage *target=c->images[@(ref.id)];if(!target)return MAG_NATIVE_STALE;
 int status=c->host.try_begin(c->host.userdata,c->host.lifetime);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
 @try{
  status=target->host.validate(target->host.userdata,target->host.owner_lifetime,target->host.resource_id,target->host.resource_generation);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
  if(!scene->images||scene->output_image>=scene->image_count)return MAG_NATIVE_INVALID;
  const mag_native_scene_image *im=&scene->images[scene->output_image];if(im->format!=target->host.format||im->width!=target->host.width||im->height!=target->host.height||im->upload.id||im->upload.device_generation||im->upload.version)return MAG_NATIVE_INVALID;
  return metal_scene_owned(opaque,scene,out,target,sampler_table,sampler_count,v3,v4);
 }@catch(NSException *e){(void)e;return out->submission_id?MAG_NATIVE_INDETERMINATE:MAG_NATIVE_DEVICE_FAILED;}@finally{c->host.end(c->host.userdata,c->host.lifetime);}
}

static int metal_submit_scene_frame_impl(void *opaque,const mag_native_scene *scene,mag_native_image ref,const MagRenderHostFrame *frame,mag_native_ticket *out,const mag_native_sampler *sampler_table,uint32_t sampler_count,const mag_native_scene_v3 *v3,const mag_native_scene_v4 *v4){
 MagRenderMetal *c=context(opaque);if(frame->owner_lifetime!=c->host.lifetime||ref.device_generation!=c->generation||!ref.id)return MAG_NATIVE_STALE;
 MagRenderImportedImage *target=c->images[@(ref.id)];if(!target)return MAG_NATIVE_STALE;
 int status=c->host.try_begin(c->host.userdata,c->host.lifetime);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
 int reserved=0;
 @try{
  status=target->host.validate(target->host.userdata,target->host.owner_lifetime,target->host.resource_id,target->host.resource_generation);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
  status=frame->preflight(frame->userdata,frame->owner_lifetime,frame->reservation,target->host.resource_id,target->host.resource_generation);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;reserved=1;
  if(!scene->images||scene->output_image>=scene->image_count){frame->cancel(frame->userdata,frame->owner_lifetime,frame->reservation);return MAG_NATIVE_INVALID;}
  const mag_native_scene_image *im=&scene->images[scene->output_image];if(im->format!=target->host.format||im->width!=target->host.width||im->height!=target->host.height||im->upload.id||im->upload.device_generation||im->upload.version){frame->cancel(frame->userdata,frame->owner_lifetime,frame->reservation);return MAG_NATIVE_INVALID;}
  status=metal_scene_owned(opaque,scene,out,target,sampler_table,sampler_count,v3,v4);
  if(status<0){frame->cancel(frame->userdata,frame->owner_lifetime,frame->reservation);return status;}
  int publication=frame->committed(frame->userdata,frame->owner_lifetime,frame->reservation,out->device_generation,out->submission_id,status?1u:0u);
  return publication?MAG_NATIVE_INDETERMINATE:status;
 }@catch(NSException *error){(void)error;
  if(out->submission_id){if(reserved)frame->committed(frame->userdata,frame->owner_lifetime,frame->reservation,out->device_generation,out->submission_id,1);return MAG_NATIVE_INDETERMINATE;}
  if(reserved)frame->cancel(frame->userdata,frame->owner_lifetime,frame->reservation);return MAG_NATIVE_DEVICE_FAILED;
 }@finally{c->host.end(c->host.userdata,c->host.lifetime);}
}

static int metal_submit_scene(void *opaque,const mag_native_scene *scene,mag_native_ticket *out){return metal_submit_scene_impl(opaque,scene,out,NULL,0,NULL,NULL);}
static int metal_submit_scene_target(void *opaque,const mag_native_scene *scene,mag_native_image target,mag_native_ticket *out){return metal_submit_scene_target_impl(opaque,scene,target,out,NULL,0,NULL,NULL);}
static int metal_submit_scene_frame(void *opaque,const mag_native_scene *scene,mag_native_image target,const MagRenderHostFrame *frame,mag_native_ticket *out){return metal_submit_scene_frame_impl(opaque,scene,target,frame,out,NULL,0,NULL,NULL);}
static int metal_submit_scene_v2(void *opaque,const mag_native_scene_v2 *desc,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
    if(frame)return metal_submit_scene_frame_impl(opaque,desc->scene,*target,frame,out,desc->samplers,desc->sampler_count,NULL,NULL);
    if(target)return metal_submit_scene_target_impl(opaque,desc->scene,*target,out,desc->samplers,desc->sampler_count,NULL,NULL);
    return metal_submit_scene_impl(opaque,desc->scene,out,desc->samplers,desc->sampler_count,NULL,NULL);
}

static int metal_submit_scene_v3(void *opaque,const mag_native_scene_v3 *v3,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
 const mag_native_scene_v2 *desc=&v3->base;
 if(frame)return metal_submit_scene_frame_impl(opaque,desc->scene,*target,frame,out,desc->samplers,desc->sampler_count,v3,NULL);
 if(target)return metal_submit_scene_target_impl(opaque,desc->scene,*target,out,desc->samplers,desc->sampler_count,v3,NULL);
 return metal_submit_scene_impl(opaque,desc->scene,out,desc->samplers,desc->sampler_count,v3,NULL);
}

static int metal_submit_scene_v4(void *opaque,const mag_native_scene_v4 *desc,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
 if(frame)return metal_submit_scene_frame_impl(opaque,desc->base.base.scene,*target,frame,out,desc->base.base.samplers,desc->base.base.sampler_count,&desc->base,desc);
 if(target)return metal_submit_scene_target_impl(opaque,desc->base.base.scene,*target,out,desc->base.base.samplers,desc->base.base.sampler_count,&desc->base,desc);
 return metal_submit_scene_impl(opaque,desc->base.base.scene,out,desc->base.base.samplers,desc->base.base.sampler_count,&desc->base,desc);
}
