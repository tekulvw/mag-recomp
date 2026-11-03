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
static void metal_scene_error(const char *stage,const mag_native_shader_stage *shader,NSError *error) {
    static unsigned failures=0;
    if(__atomic_fetch_add(&failures,1,__ATOMIC_RELAXED)>=8)return;
    const char *message=error.localizedDescription.UTF8String;
    fprintf(stderr,"[rsx frontend/metal scene] stage=%s status=%d entry=%.*s language=%u source_bytes=%llu error=%.*s\n",stage,MAG_NATIVE_UNSUPPORTED,
        shader?(int)(shader->entry_length<128?shader->entry_length:128):0,
        shader?(const char *)shader->entry:"",shader?shader->language:0,
        (unsigned long long)(shader?shader->source_length:0),2048,message?message:"entry or pipeline unavailable");
}
static id<MTLFunction> metal_scene_function(MagRenderMetal *c, const mag_native_shader_stage *s) {
    if (s->language != MAG_NATIVE_MSL) {
        metal_scene_error("shader-language",s,nil);
        return nil;
    }
    NSString *source =
        [[NSString alloc] initWithBytes:s->source length:s->source_length encoding:NSUTF8StringEncoding];
    NSString *entry =
        [[NSString alloc] initWithBytes:s->entry length:s->entry_length encoding:NSUTF8StringEncoding];
    if (!source || !entry) {
        metal_scene_error("shader-source-encoding",s,nil);
        return nil;
    }
    MTLCompileOptions *options = [MTLCompileOptions new];
    // Match retained guest shaders and our compute compiler: default fast math
    // may remove RSX NaN guards or reassociate depth-equality position math.
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
    if (@available(macOS 15.0, iOS 18.0, *)) {
        options.mathMode = MTLMathModeSafe;
        options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
    } else
#endif
    { [options setValue:@NO forKey:@"fastMathEnabled"]; }
    options.languageVersion = MTLLanguageVersion2_0;
    NSError *error = nil;
    id<MTLLibrary> lib = [c->device newLibraryWithSource:source options:options error:&error];
    if (!lib) {
        metal_scene_error("shader-library",s,error);
        return nil;
    }
    id<MTLFunction> function=[lib newFunctionWithName:entry];
    if(!function)metal_scene_error("shader-entry",s,nil);
    return function;
}
static int metal_scene_graphics_create_layout(void *opaque, const mag_native_scene_graphics_desc *desc,
                                       const mag_native_vertex_attribute *attributes,uint32_t attribute_count,uint32_t binding_model,const mag_native_raster_state *raster,uint32_t flags,mag_native_pipeline *out) {
    MagRenderMetal *c = context(opaque);
    if(!!flags!=!!(raster&&raster->depth_bounds_enabled)){metal_scene_error("stored-depth-flags",&desc->vertex,nil);return MAG_NATIVE_UNSUPPORTED;}
    if(desc->depth_format==MAG_NATIVE_SCENE_D24S8&&![c->device isDepth24Stencil8PixelFormatSupported]){metal_scene_error("depth24-stencil8-format",&desc->vertex,nil);return MAG_NATIVE_UNSUPPORTED;}
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
    id<MTLRenderPipelineState> graphics=nil;
    if(flags){
        if(@available(macOS 13.0,*)){
            MTLRenderPipelineReflection *reflection=nil;
            graphics=[c->device newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionBindingInfo|MTLPipelineOptionBufferTypeInfo reflection:&reflection error:&error];
            if(!graphics){metal_scene_error("render-pipeline",&desc->vertex,error);return MAG_NATIVE_UNSUPPORTED;}
            int texture=0,buffer=0;
            for(id<MTLBinding> binding in reflection.fragmentBindings){
                if(binding.type==MTLBindingTypeTexture&&binding.index==15){id<MTLTextureBinding> t=(id<MTLTextureBinding>)binding;texture=t.depthTexture&&t.textureType==MTLTextureType2D&&t.textureDataType==MTLDataTypeFloat&&t.access==MTLBindingAccessReadOnly;}
                if(binding.type==MTLBindingTypeBuffer&&binding.index==2){id<MTLBufferBinding> b=(id<MTLBufferBinding>)binding;buffer=b.bufferDataSize==16&&b.access==MTLBindingAccessReadOnly;}
            }
            if(!texture||!buffer)return MAG_NATIVE_UNSUPPORTED;
        }else{return MAG_NATIVE_UNSUPPORTED;}
    }else{graphics=[c->device newRenderPipelineStateWithDescriptor:pd error:&error];}
    if(!graphics){metal_scene_error("render-pipeline",&desc->vertex,error);return MAG_NATIVE_UNSUPPORTED;}
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
    p->scene = 1;p->scene_binding_model=binding_model;p->stored_depth_aux=!!flags;
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
    return metal_scene_graphics_create_layout(opaque,desc,attributes,1,0,NULL,0,out);
}
static int metal_scene_graphics_create_v2(void *opaque,const mag_native_scene_graphics_v2_desc *desc,mag_native_pipeline *out){
    int status=mag_native_scene_graphics_v2_validate(desc);if(status)return status;
    return metal_scene_graphics_create_layout(opaque,&desc->base,desc->attributes,desc->attribute_count,desc->binding_model,NULL,0,out);
}
static int metal_scene_graphics_create_v3(void *opaque,const mag_native_scene_graphics_v3_desc *desc,mag_native_pipeline *out){
    int status=mag_native_scene_graphics_v3_validate(desc);if(status)return status;
    mag_native_scene_graphics_desc schema;mag_native_scene_graphics_v3_schema(desc,&schema);
    return metal_scene_graphics_create_layout(opaque,&schema,desc->base.attributes,desc->base.attribute_count,desc->base.binding_model,&desc->raster,0,out);
}
static int metal_scene_graphics_create_aux(void *opaque,const mag_native_scene_graphics_v3_desc *desc,uint32_t flags,mag_native_pipeline *out){
    mag_native_scene_graphics_desc schema;mag_native_scene_graphics_v3_schema(desc,&schema);
    return metal_scene_graphics_create_layout(opaque,&schema,desc->base.attributes,desc->base.attribute_count,desc->base.binding_model,&desc->raster,flags,out);
}
static int metal_scene_raster_info(void *opaque,mag_native_pipeline ref,const mag_native_raster_state **out){
 MagRenderPipeline *p=find_pipeline(context(opaque),ref);if(!p)return MAG_NATIVE_STALE;if(!p->scene)return MAG_NATIVE_INVALID;*out=p->explicit_raster?&p->raster:NULL;return MAG_NATIVE_OK;
}
static MagRenderImportedImage *metal_imported(MagRenderMetal *c,mag_native_image ref){return ref.device_generation==c->generation&&ref.id?c->images[@(ref.id)]:nil;}
static int metal_scene_import_info(void *opaque,mag_native_image ref,uint32_t required,mag_native_import_info *out){
 MagRenderMetal *c=context(opaque);MagRenderImportedImage *image=metal_imported(c,ref);if(!image)return MAG_NATIVE_STALE;
 /* Encoding a reservation runs without the owner lease: only the exact imports
  * validated under the lease at reservation time are admitted, unchanged. */
 if(c->reserved_encoding){
  for(uint32_t k=0;k<c->reserved_count;++k){const mag_native_scene_import *r=&c->reserved_imports[k];
   if(r->ref.device_generation==ref.device_generation&&r->ref.id==ref.id&&r->required_initialized_planes==required){*out=c->reserved_info[k];return MAG_NATIVE_OK;}}
  return MAG_NATIVE_STALE;
 }
 int status=image->host.validate(image->host.userdata,image->host.owner_lifetime,image->host.resource_id,image->host.resource_generation);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
 if(required){if(!image->validate_planes)return MAG_NATIVE_UNSUPPORTED;status=image->validate_planes(image->host.userdata,image->host.owner_lifetime,image->host.resource_id,image->host.resource_generation,required);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;}
 *out=(mag_native_import_info){image->host.format,image->host.width,image->host.height,0,image->host.owner_lifetime,image->host.resource_id,image->host.resource_generation};return MAG_NATIVE_OK;
}
/* Resident uploaded images: on by default, MAG_RENDER_METAL_RESIDENT_IMAGES=0
 * restores per-submission texture creation and upload.
 * An uploaded image that no pass of this scene attaches (writes) is only
 * sampled, so its texture contents are exactly its upload bytes. Such a
 * texture is kept and reused by a later scene whose image has the same shape
 * and byte-identical upload contents (compared in full; the hash only picks a
 * bucket). Reused textures are never written again; submissions that used an
 * evicted texture keep it alive through their retained array. */
@interface MagRenderResidentImage : NSObject {
@public id<MTLTexture> texture; NSData *shape; NSData *bytes; uint64_t used;
    uint64_t upload_id,upload_version; /* the immutable upload buffer it was made from */
}
@end
@implementation MagRenderResidentImage @end
static const uint64_t metal_resident_max_bytes=256ull*1024*1024;
static const uint32_t metal_resident_max_entries=4096;
static int metal_scene_image_attached(const mag_native_scene *scene,uint32_t image){
    if(image==scene->output_image)return 1;
    for(uint32_t n=0;n<scene->pass_count;++n){const mag_native_scene_pass *p=&scene->passes[n];
        if(p->color.image==image||p->depth.image==image)return 1;}
    return 0;
}
static NSData *metal_resident_shape(const mag_native_scene_image *im,const mag_native_scene_image_layout *shape,uint64_t length){
    NSMutableData *d=[NSMutableData data];
    uint32_t head[4]={im->format,im->width,im->height,shape?1u:0u};[d appendBytes:head length:sizeof head];
    [d appendBytes:&length length:sizeof length];
    if(shape){uint32_t s[6]={shape->dimension,shape->depth,shape->layers,shape->mip_count,shape->samples,shape->subresource_count};
        [d appendBytes:s length:sizeof s];
        if(shape->subresource_count)[d appendBytes:shape->subresources length:shape->subresource_count*sizeof(*shape->subresources)];}
    return d;
}
static uint64_t metal_resident_hash(NSData *shape,const uint8_t *bytes,uint64_t length){
    uint64_t h=1469598103934665603ull;const uint8_t *k=shape.bytes;
    for(NSUInteger i=0;i<shape.length;++i){h^=k[i];h*=1099511628211ull;}
    /* Bounded sample of the content; equality is always a full compare. */
    uint64_t words=length/8,step=words>512?words/512:1;
    for(uint64_t w=0;w<words;w+=step){uint64_t v;memcpy(&v,bytes+w*8,8);h^=v;h*=1099511628211ull;}
    for(uint64_t i=words*8;i<length;++i){h^=bytes[i];h*=1099511628211ull;}
    return h;
}
/* An upload buffer (id, version) never changes contents unless the GPU wrote
 * it, so the same buffer and shape name the same texture without hashing or
 * comparing bytes. Reached when upload buffers persist across submissions. */
static id<MTLTexture> metal_resident_find_upload(MagRenderMetal *c,MagRenderBuffer *upload,NSData *shape){
    if(upload->gpu_written)return nil;
    MagRenderResidentImage *e=c->resident_by_upload[@(upload->identifier)];
    if(e&&e->upload_id==upload->identifier&&e->upload_version==upload->version&&[e->shape isEqualToData:shape]){
        e->used=c->next_submission;return e->texture;}
    return nil;
}
static id<MTLTexture> metal_resident_find(MagRenderMetal *c,uint64_t hash,NSData *shape,const uint8_t *bytes,uint64_t length,MagRenderBuffer *upload){
    for(MagRenderResidentImage *e in c->resident[@(hash)])
        if(e->bytes.length==length&&[e->shape isEqualToData:shape]&&!memcmp(e->bytes.bytes,bytes,(size_t)length)){
            e->used=c->next_submission;
            /* Content proven equal: later submissions of this same immutable
             * upload buffer may take the identity path. */
            if(!upload->gpu_written){
                if(e->upload_id&&c->resident_by_upload[@(e->upload_id)]==e)[c->resident_by_upload removeObjectForKey:@(e->upload_id)];
                e->upload_id=upload->identifier;e->upload_version=upload->version;c->resident_by_upload[@(e->upload_id)]=e;
            }
            return e->texture;}
    return nil;
}
static void metal_resident_insert(MagRenderMetal *c,uint64_t hash,NSData *shape,const uint8_t *bytes,uint64_t length,id<MTLTexture> texture,MagRenderBuffer *upload){
    if(length>metal_resident_max_bytes/4)return;
    while(c->resident_entries&&(c->resident_entries>=metal_resident_max_entries||c->resident_bytes+length>metal_resident_max_bytes)){
        NSNumber *oldest_key=nil;NSUInteger oldest_index=0;uint64_t oldest=UINT64_MAX;
        for(NSNumber *key in c->resident){NSMutableArray *bucket=c->resident[key];
            for(NSUInteger i=0;i<bucket.count;++i){MagRenderResidentImage *e=bucket[i];if(e->used<oldest){oldest=e->used;oldest_key=key;oldest_index=i;}}}
        if(!oldest_key)break;
        NSMutableArray *bucket=c->resident[oldest_key];MagRenderResidentImage *e=bucket[oldest_index];
        c->resident_bytes-=e->bytes.length;--c->resident_entries;++c->resident_evictions;
        if(e->upload_id&&c->resident_by_upload[@(e->upload_id)]==e)[c->resident_by_upload removeObjectForKey:@(e->upload_id)];
        [bucket removeObjectAtIndex:oldest_index];if(!bucket.count)[c->resident removeObjectForKey:oldest_key];
    }
    MagRenderResidentImage *e=[MagRenderResidentImage new];e->texture=texture;e->shape=shape;
    e->upload_id=upload->gpu_written?0:upload->identifier;e->upload_version=upload->version;
    e->bytes=[NSData dataWithBytes:bytes length:(NSUInteger)length];e->used=c->next_submission;
    if(!e->bytes)return;
    NSMutableArray *bucket=c->resident[@(hash)];if(!bucket){bucket=[NSMutableArray new];c->resident[@(hash)]=bucket;}
    [bucket addObject:e];c->resident_bytes+=length;++c->resident_entries;
    if(e->upload_id)c->resident_by_upload[@(e->upload_id)]=e;
}
/* Commit a reservation that will not be (or could not be) encoded so its
 * queue position never blocks later work. Any encoders were already ended. */
static void metal_reservation_release(MagRenderMetal *c){
    if(!c->reserved)return;
    if(c->reserved.status<MTLCommandBufferStatusCommitted)[c->reserved commit];
    c->reserved=nil;c->reserved_count=0;c->reserved_ticket=(mag_native_ticket){0};
}
static int metal_reserve_scene(void *opaque,const mag_native_scene_import *imports,uint32_t count,mag_native_ticket *out){
    MagRenderMetal *c=context(opaque);
    if(c->reserved||c->pending.count>=2)return MAG_NATIVE_BUSY;
    if(c->next_submission==UINT64_MAX)return MAG_NATIVE_DEVICE_FAILED;
    int status=c->host.try_begin(c->host.userdata,c->host.lifetime);
    if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
    @try{
        mag_native_import_info info[MAG_NATIVE_SCENE_MAX_IMAGES];
        for(uint32_t i=0;i<count;++i){
            for(uint32_t j=0;j<i;++j)if(imports[j].image==imports[i].image)return MAG_NATIVE_INVALID;
            status=metal_scene_import_info(opaque,imports[i].ref,imports[i].required_initialized_planes,&info[i]);
            if(status)return status;
        }
        id<MTLCommandBuffer> command=[c->queue commandBuffer];
        if(!command)return MAG_NATIVE_DEVICE_FAILED;
        /* Fixes this submission's place in queue order now, at the genuine
         * native boundary; later queue work executes after it. */
        [command enqueue];
        c->reserved=command;c->reserved_count=count;
        memcpy(c->reserved_imports,imports,count*sizeof(*imports));memcpy(c->reserved_info,info,count*sizeof(*info));
        c->reserved_ticket=(mag_native_ticket){c->generation,++c->next_submission};
        *out=c->reserved_ticket;
        return MAG_NATIVE_OK;
    }@catch(NSException *e){(void)e;metal_reservation_release(c);return MAG_NATIVE_DEVICE_FAILED;}
    @finally{c->host.end(c->host.userdata,c->host.lifetime);}
}
static int metal_abandon_scene_reservation(void *opaque){
    MagRenderMetal *c=context(opaque);if(!c->reserved)return MAG_NATIVE_STALE;
    metal_reservation_release(c);return MAG_NATIVE_OK;
}
static int metal_scene_owned(void *opaque, const mag_native_scene *scene, mag_native_ticket *out,MagRenderImportedImage *target,const mag_native_sampler *sampler_table,uint32_t sampler_count,const mag_native_scene_v3 *v3,const mag_native_scene_v4 *v4,const mag_native_scene_v5 *v5) {
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
    mag_native_scene_registry_v5 registry_v5={&registry_v4,metal_scene_import_info};
    int status=v5?mag_native_scene_validate_v5(v5,&registry_v5,&usage):v4?mag_native_scene_validate_v4(v4,&registry_v4,&usage):v3?mag_native_scene_validate_v3(v3,&registry_v2,&usage):mag_native_scene_validate_v2(scene,&registry_v2,&usage);
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
        MagRenderImportedImage *borrowed=nil;
        if(v5)for(uint32_t k=0;k<v5->import_count;++k)if(v5->imports[k].image==i)borrowed=metal_imported(c,v5->imports[k].ref);
        if(borrowed){
            if(target&&i==scene->output_image&&target->image!=borrowed->image)return MAG_NATIVE_INVALID;
            if(target&&i!=scene->output_image&&target->image==borrowed->image)return MAG_NATIVE_INVALID;
            for(uint32_t pass=0;pass<scene->pass_count;++pass){
                const mag_native_scene_pass *p=&scene->passes[pass];
                if((p->color.image==i||p->depth.image==i)&&!(borrowed->image.usage&MTLTextureUsageRenderTarget))return MAG_NATIVE_UNSUPPORTED;
                for(uint32_t d=0;d<p->draw_count;++d)for(uint32_t t=0;t<p->draws[d].texture_count;++t)if(mag_native_scene_image_index(v3,p->draws[d].textures[t].image)==i&&!(borrowed->image.usage&MTLTextureUsageShaderRead))return MAG_NATIVE_UNSUPPORTED;
            }
            [images addObject:borrowed->image];[retained addObject:borrowed];continue;
        }
        const mag_native_scene_image_layout *shape=v3&&v3->image_layout_count?&v3->image_layouts[i]:NULL;
        mag_native_image_format info;mag_native_image_format_info(im->format,&info);
        if(info.block_width>1&&!c->device.supportsBCTextureCompression)return MAG_NATIVE_UNSUPPORTED;
        // Source depth images are native render targets. Uploading host depth
        // requires a separate private-texture staging path, not replaceRegion.
        if((im->format==MAG_NATIVE_SCENE_D32||mag_native_image_has_stencil(im->format))&&im->upload.id)return MAG_NATIVE_UNSUPPORTED;
        NSData *resident_shape=nil;uint64_t resident_hash=0;MagRenderBuffer *resident_upload=nil;
        if(c->resident_enabled&&im->upload.id&&!metal_scene_image_attached(scene,i)){
            resident_upload=find_buffer(c,im->upload);
            if(resident_upload){
                resident_shape=metal_resident_shape(im,shape,resident_upload->gpu.length);
                id<MTLTexture> hit=metal_resident_find_upload(c,resident_upload,resident_shape);
                if(!hit){
                    resident_hash=metal_resident_hash(resident_shape,resident_upload->gpu.contents,resident_upload->gpu.length);
                    hit=metal_resident_find(c,resident_hash,resident_shape,resident_upload->gpu.contents,resident_upload->gpu.length,resident_upload);
                }
                if(hit){++c->resident_hits;[images addObject:hit];[retained addObject:hit];continue;}
                ++c->resident_misses;
            }
        }
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
            if(resident_shape)metal_resident_insert(c,resident_hash,resident_shape,resident_upload->gpu.contents,resident_upload->gpu.length,texture,resident_upload);
        }
        [images addObject:texture];
        [retained addObject:texture];
    }
    NSMutableArray<id<MTLTexture>> *views=[NSMutableArray new];
    if(v3)for(uint32_t i=0;i<v3->view_count;++i){
        const mag_native_image_view *v=&v3->views[i];id<MTLTexture> parent=images[v->image];id<MTLTexture> view=nil;
        const MTLTextureSwizzle components[]={MTLTextureSwizzleRed,MTLTextureSwizzleGreen,MTLTextureSwizzleBlue,MTLTextureSwizzleAlpha,MTLTextureSwizzleZero,MTLTextureSwizzleOne};
        int identity=v->dimension==2&&v->base_mip==0&&v->mip_count==parent.mipmapLevelCount&&v->base_layer==0&&v->layer_count==1;
        for(unsigned component=0;component<4;++component)identity&=v->remap[component]==component;
        if(identity)view=parent;
        else if(!(parent.usage&MTLTextureUsagePixelFormatView))return MAG_NATIVE_UNSUPPORTED;
        else if(@available(macOS 10.15,*)){
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
    NSMutableArray<id> *snapshots=[NSMutableArray new];
    uint32_t flattened=0;
    for(uint32_t n=0;n<scene->pass_count;++n){
        const mag_native_scene_pass *p=&scene->passes[n];int needed=0;
        for(uint32_t j=0;j<p->draw_count;++j){MagRenderPipeline *pipeline=find_pipeline(c,p->draws[j].pipeline);
            int enabled=v5&&v5->bounds_count&&v5->bounds[flattened+j].enabled;
            if(enabled!=pipeline->stored_depth_aux)return MAG_NATIVE_INVALID;
            if(enabled){needed=1;if(pipeline->raster.stencil_enabled&&((pipeline->raster.front_stencil.write_mask|pipeline->raster.back_stencil.write_mask)&255u))return MAG_NATIVE_UNSUPPORTED;}
        }
        flattened+=p->draw_count;
        if(!needed){[snapshots addObject:[NSNull null]];continue;}
        if(p->depth.image==MAG_NATIVE_SCENE_NONE||p->depth.load!=MAG_NATIVE_SCENE_LOAD)return MAG_NATIVE_UNSUPPORTED;
        for(uint32_t j=0;j<p->draw_count;++j){MagRenderPipeline *pipeline=find_pipeline(c,p->draws[j].pipeline);if(pipeline->explicit_raster?(pipeline->raster.depth_enabled&&pipeline->raster.depth_write):pipeline->scene_desc.depth_write)return MAG_NATIVE_UNSUPPORTED;}
        id<MTLTexture> source=images[p->depth.image];
        MTLTextureDescriptor *shape=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat width:source.width height:source.height mipmapped:NO];shape.storageMode=MTLStorageModePrivate;shape.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> snapshot=[c->device newTextureWithDescriptor:shape];if(!snapshot)return MAG_NATIVE_ALLOCATION_FAILED;
        [snapshots addObject:snapshot];[retained addObject:snapshot];
    }
    MagRenderSubmission *submission = [MagRenderSubmission new];
    submission->uses = uses;
    submission->retained = retained;
    // All images, pipeline references and buffers are validated before any queue effect.
    submission->command = c->reserved_encoding ? c->reserved : [c->queue commandBuffer];
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
    flattened=0;
    for (uint32_t n = 0; n < scene->pass_count; ++n) {
        const mag_native_scene_pass *p = &scene->passes[n];
        id<MTLTexture> snapshot=snapshots[n]==[NSNull null]?nil:snapshots[n];
        if(snapshot){
            id<MTLTexture> source=images[p->depth.image];id<MTLBlitCommandEncoder> blit=[submission->command blitCommandEncoder];if(!blit)return MAG_NATIVE_DEVICE_FAILED;
            [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(source.width,source.height,1) toTexture:snapshot destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[blit endEncoding];
        }
        MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
        // Explicit render area, bounded by each attachment in common admission.
        // Physical depth can be larger; no cropped view or identity is invented.
        uint32_t area=p->color.image!=MAG_NATIVE_SCENE_NONE?p->color.image:p->depth.image;
        pd.renderTargetWidth=scene->images[area].width;pd.renderTargetHeight=scene->images[area].height;
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
            if(!v->scissor_width||!v->scissor_height){[e endEncoding];flattened+=p->draw_count;continue;}
            [e setScissorRect:(MTLScissorRect){v->scissor_x,v->scissor_y,v->scissor_width,v->scissor_height}];
        }
        for (uint32_t j = 0; j < p->draw_count; ++j) {
            const mag_native_scene_draw *d = &p->draws[j];
            MagRenderPipeline *pipeline = find_pipeline(c, d->pipeline);
            [retained addObject:pipeline];
            [e setRenderPipelineState:pipeline->graphics];
            [e setDepthStencilState:pipeline->depth];
            if(pipeline->stored_depth_aux){
                const mag_native_depth_bounds *bounds=&v5->bounds[flattened+j];float range[4]={bounds->minimum,bounds->maximum,0,0};
                [e setFragmentTexture:snapshot atIndex:15];[e setFragmentBytes:range length:sizeof(range) atIndex:2];
            }
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
        flattened+=p->draw_count;
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
    if(c->reserved_encoding){submission->ticket=c->reserved_ticket;c->reserved=nil;c->reserved_count=0;}
    else submission->ticket = (mag_native_ticket){c->generation, ++c->next_submission};
    if(c->resident_enabled&&c->resident_report&&(c->next_submission<=4||c->next_submission%120==0))
        fprintf(stderr,"[mag native resident images] submission=%llu hits=%llu misses=%llu entries=%u bytes=%llu evictions=%llu\n",
            (unsigned long long)c->next_submission,(unsigned long long)c->resident_hits,(unsigned long long)c->resident_misses,
            c->resident_entries,(unsigned long long)c->resident_bytes,(unsigned long long)c->resident_evictions);
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
static int metal_submit_scene_impl(void *opaque, const mag_native_scene *scene, mag_native_ticket *out,const mag_native_sampler *sampler_table,uint32_t sampler_count,const mag_native_scene_v3 *v3,const mag_native_scene_v4 *v4,const mag_native_scene_v5 *v5) {
    MagRenderMetal *c = context(opaque);
    if (c->reserved) {
        /* Encode the reservation: queue order, ticket and import validation
         * were fixed under the lease when it was reserved. */
        int result;
        c->reserved_encoding = 1;
        @try {
            result = v5 ? metal_scene_owned(opaque, scene, out, nil, sampler_table, sampler_count, v3, v4, v5) : MAG_NATIVE_INVALID;
        } @catch (NSException *e) {
            (void)e;
            result = out->submission_id ? MAG_NATIVE_INDETERMINATE : MAG_NATIVE_DEVICE_FAILED;
        }
        c->reserved_encoding = 0;
        if (c->reserved) metal_reservation_release(c);
        return result;
    }
    int status = c->host.try_begin(c->host.userdata, c->host.lifetime);
    if (status)
        return status < 0 ? status : MAG_NATIVE_DEVICE_FAILED;
    @try {
        return metal_scene_owned(opaque, scene, out,nil,sampler_table,sampler_count,v3,v4,v5);
    } @catch (NSException *e) {
        (void)e;
        return out->submission_id ? MAG_NATIVE_INDETERMINATE : MAG_NATIVE_DEVICE_FAILED;
    } @finally {
        c->host.end(c->host.userdata, c->host.lifetime);
    }
}

static int metal_import_image(void *opaque,const MagRenderHostImage *host,mag_native_validate_planes_fn planes,int certified,mag_native_image *out){
 MagRenderMetal *c=context(opaque);
 if(host->backend!=MAG_RENDER_HOST_METAL||host->owner_lifetime!=c->host.lifetime)return MAG_NATIVE_STALE;
 if(c->images.count>=MAG_NATIVE_SCENE_MAX_IMAGES+2||c->next_image==UINT64_MAX)return MAG_NATIVE_ALLOCATION_FAILED;
 int status=host->retain(host->userdata,host->owner_lifetime,host->resource_id,host->resource_generation);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
 id<MTLTexture> image=(__bridge id<MTLTexture>)host->image;
 MTLPixelFormat format=metal_scene_format(host->format);
 if(!image||image.device!=c->device||image.width!=host->width||image.height!=host->height||!host->width||!host->height||image.textureType!=MTLTextureType2D||image.sampleCount!=1||image.mipmapLevelCount!=1||format==MTLPixelFormatInvalid||image.pixelFormat!=format||
   (certified?!(image.usage&(MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead)):!(image.usage&MTLTextureUsageRenderTarget))){host->release(host->userdata,host->owner_lifetime,host->resource_id,host->resource_generation);return MAG_NATIVE_UNSUPPORTED;}
 MagRenderImportedImage *record=[MagRenderImportedImage new];record->image=image;record->host=*host;record->validate_planes=planes;uint64_t id=++c->next_image;c->images[@(id)]=record;*out=(mag_native_image){c->generation,id};return MAG_NATIVE_OK;
}
static int metal_image_import(void *opaque,const MagRenderHostImage *host,mag_native_image *out){return metal_import_image(opaque,host,NULL,0,out);}
static int metal_image_import_certified(void *opaque,const mag_native_image_certificate *cert,mag_native_image *out){return metal_import_image(opaque,&cert->host,cert->validate_planes,1,out);}
static int metal_image_release(void *opaque,mag_native_image ref){
 MagRenderMetal *c=context(opaque);if(ref.device_generation!=c->generation||!ref.id||!c->images[@(ref.id)])return MAG_NATIVE_STALE;[c->images removeObjectForKey:@(ref.id)];return MAG_NATIVE_OK;
}
static int metal_submit_scene_target_impl(void *opaque,const mag_native_scene *scene,mag_native_image ref,mag_native_ticket *out,const mag_native_sampler *sampler_table,uint32_t sampler_count,const mag_native_scene_v3 *v3,const mag_native_scene_v4 *v4,const mag_native_scene_v5 *v5){
 MagRenderMetal *c=context(opaque);if(c->reserved)return MAG_NATIVE_BUSY;if(ref.device_generation!=c->generation||!ref.id)return MAG_NATIVE_STALE;
 MagRenderImportedImage *target=c->images[@(ref.id)];if(!target)return MAG_NATIVE_STALE;
 int status=c->host.try_begin(c->host.userdata,c->host.lifetime);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
 @try{
  status=target->host.validate(target->host.userdata,target->host.owner_lifetime,target->host.resource_id,target->host.resource_generation);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
  if(!scene->images||scene->output_image>=scene->image_count)return MAG_NATIVE_INVALID;
  const mag_native_scene_image *im=&scene->images[scene->output_image];if(im->format!=target->host.format||im->width!=target->host.width||im->height!=target->host.height||im->upload.id||im->upload.device_generation||im->upload.version)return MAG_NATIVE_INVALID;
  return metal_scene_owned(opaque,scene,out,target,sampler_table,sampler_count,v3,v4,v5);
 }@catch(NSException *e){(void)e;return out->submission_id?MAG_NATIVE_INDETERMINATE:MAG_NATIVE_DEVICE_FAILED;}@finally{c->host.end(c->host.userdata,c->host.lifetime);}
}

static int metal_submit_scene_frame_impl(void *opaque,const mag_native_scene *scene,mag_native_image ref,const MagRenderHostFrame *frame,mag_native_ticket *out,const mag_native_sampler *sampler_table,uint32_t sampler_count,const mag_native_scene_v3 *v3,const mag_native_scene_v4 *v4,const mag_native_scene_v5 *v5){
 MagRenderMetal *c=context(opaque);if(c->reserved)return MAG_NATIVE_BUSY;if(frame->owner_lifetime!=c->host.lifetime||ref.device_generation!=c->generation||!ref.id)return MAG_NATIVE_STALE;
 MagRenderImportedImage *target=c->images[@(ref.id)];if(!target)return MAG_NATIVE_STALE;
 int status=c->host.try_begin(c->host.userdata,c->host.lifetime);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
 int reserved=0;
 @try{
  status=target->host.validate(target->host.userdata,target->host.owner_lifetime,target->host.resource_id,target->host.resource_generation);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;
  status=frame->preflight(frame->userdata,frame->owner_lifetime,frame->reservation,target->host.resource_id,target->host.resource_generation);if(status)return status<0?status:MAG_NATIVE_DEVICE_FAILED;reserved=1;
  if(!scene->images||scene->output_image>=scene->image_count){frame->cancel(frame->userdata,frame->owner_lifetime,frame->reservation);return MAG_NATIVE_INVALID;}
  const mag_native_scene_image *im=&scene->images[scene->output_image];if(im->format!=target->host.format||im->width!=target->host.width||im->height!=target->host.height||im->upload.id||im->upload.device_generation||im->upload.version){frame->cancel(frame->userdata,frame->owner_lifetime,frame->reservation);return MAG_NATIVE_INVALID;}
  status=metal_scene_owned(opaque,scene,out,target,sampler_table,sampler_count,v3,v4,v5);
  if(status<0){frame->cancel(frame->userdata,frame->owner_lifetime,frame->reservation);return status;}
  int publication=frame->committed(frame->userdata,frame->owner_lifetime,frame->reservation,out->device_generation,out->submission_id,status?1u:0u);
  return publication?MAG_NATIVE_INDETERMINATE:status;
 }@catch(NSException *error){(void)error;
  if(out->submission_id){if(reserved)frame->committed(frame->userdata,frame->owner_lifetime,frame->reservation,out->device_generation,out->submission_id,1);return MAG_NATIVE_INDETERMINATE;}
  if(reserved)frame->cancel(frame->userdata,frame->owner_lifetime,frame->reservation);return MAG_NATIVE_DEVICE_FAILED;
 }@finally{c->host.end(c->host.userdata,c->host.lifetime);}
}

static int metal_submit_scene(void *opaque,const mag_native_scene *scene,mag_native_ticket *out){return metal_submit_scene_impl(opaque,scene,out,NULL,0,NULL,NULL,NULL);}
static int metal_submit_scene_target(void *opaque,const mag_native_scene *scene,mag_native_image target,mag_native_ticket *out){return metal_submit_scene_target_impl(opaque,scene,target,out,NULL,0,NULL,NULL,NULL);}
static int metal_submit_scene_frame(void *opaque,const mag_native_scene *scene,mag_native_image target,const MagRenderHostFrame *frame,mag_native_ticket *out){return metal_submit_scene_frame_impl(opaque,scene,target,frame,out,NULL,0,NULL,NULL,NULL);}
static int metal_submit_scene_v2(void *opaque,const mag_native_scene_v2 *desc,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
    if(frame)return metal_submit_scene_frame_impl(opaque,desc->scene,*target,frame,out,desc->samplers,desc->sampler_count,NULL,NULL,NULL);
    if(target)return metal_submit_scene_target_impl(opaque,desc->scene,*target,out,desc->samplers,desc->sampler_count,NULL,NULL,NULL);
    return metal_submit_scene_impl(opaque,desc->scene,out,desc->samplers,desc->sampler_count,NULL,NULL,NULL);
}

static int metal_submit_scene_v3(void *opaque,const mag_native_scene_v3 *v3,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
 const mag_native_scene_v2 *desc=&v3->base;
 if(frame)return metal_submit_scene_frame_impl(opaque,desc->scene,*target,frame,out,desc->samplers,desc->sampler_count,v3,NULL,NULL);
 if(target)return metal_submit_scene_target_impl(opaque,desc->scene,*target,out,desc->samplers,desc->sampler_count,v3,NULL,NULL);
 return metal_submit_scene_impl(opaque,desc->scene,out,desc->samplers,desc->sampler_count,v3,NULL,NULL);
}

static int metal_submit_scene_v4(void *opaque,const mag_native_scene_v4 *desc,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
 if(frame)return metal_submit_scene_frame_impl(opaque,desc->base.base.scene,*target,frame,out,desc->base.base.samplers,desc->base.base.sampler_count,&desc->base,desc,NULL);
 if(target)return metal_submit_scene_target_impl(opaque,desc->base.base.scene,*target,out,desc->base.base.samplers,desc->base.base.sampler_count,&desc->base,desc,NULL);
 return metal_submit_scene_impl(opaque,desc->base.base.scene,out,desc->base.base.samplers,desc->base.base.sampler_count,&desc->base,desc,NULL);
}

static int metal_submit_scene_v5(void *opaque,const mag_native_scene_v5 *v5,const mag_native_image *target,const MagRenderHostFrame *frame,mag_native_ticket *out){
 const mag_native_scene_v4 *v4=&v5->base;const mag_native_scene_v3 *v3=&v4->base;const mag_native_scene_v2 *desc=&v3->base;
 if(frame)return metal_submit_scene_frame_impl(opaque,desc->scene,*target,frame,out,desc->samplers,desc->sampler_count,v3,v4,v5);
 if(target)return metal_submit_scene_target_impl(opaque,desc->scene,*target,out,desc->samplers,desc->sampler_count,v3,v4,v5);
 return metal_submit_scene_impl(opaque,desc->scene,out,desc->samplers,desc->sampler_count,v3,v4,v5);
}
