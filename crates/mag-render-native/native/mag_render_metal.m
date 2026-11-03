/* Extracted from ps3recomp libs/video/rsx_frontend_metal.m at 6e0fcc2e026cddc02fb94f23050ac8429a03306a.
 * Copyright (c) 2026 Ned Heller / sp00nznet. MIT; see LICENSE.ps3recomp.
 * MagRender-owned implementation; no SDK runtime/header dependency. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "mag_render_native_internal.h"
#include "mag_render_sampler_registry.h"
#include "mag_render_scene_validate.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>

@interface MagRenderBuffer : NSObject {
@public id<MTLBuffer> gpu; uint64_t identifier, version; uint32_t readers, writers; int seed_zero,gpu_written;
}
@end
@implementation MagRenderBuffer @end
@interface MagRenderImportedImage : NSObject {
@public id<MTLTexture> image;MagRenderHostImage host;mag_native_validate_planes_fn validate_planes;
}
@end
@implementation MagRenderImportedImage
- (void)dealloc { if(host.release)host.release(host.userdata,host.owner_lifetime,host.resource_id,host.resource_generation); }
@end
@interface MagRenderPipeline : NSObject {
@public id<MTLComputePipelineState> gpu; MTLSize threads;
    id<MTLRenderPipelineState> graphics;id<MTLDepthStencilState> depth;
    mag_native_binding_layout bindings[MAG_NATIVE_MAX_BINDINGS];
    uint32_t binding_count, sizes_count;
    mag_native_raster_state raster;int explicit_raster,stored_depth_aux;
    int scene;uint32_t scene_binding_model;mag_native_scene_graphics_desc scene_desc;
    mag_native_scene_buffer_layout scene_buffers[32];
    mag_native_scene_texture_layout scene_textures[32];
}
@end
@implementation MagRenderPipeline @end
@interface MagRenderUse : NSObject {
@public MagRenderBuffer *buffer; int write;
}
@end
@implementation MagRenderUse @end
@interface MagRenderSubmission : NSObject {
@public id<MTLCommandBuffer> command; MagRenderPipeline *pipeline;
    id<MTLBuffer> sizes; NSArray<MagRenderUse *> *uses; mag_native_ticket ticket;
    NSArray *retained;id<MTLBuffer> packed_source;MagRenderBuffer *packed_destination;
    uint32_t packed_width,packed_height,packed_pitch;
    /* Chunked reserved encoding: the earlier, already committed command
     * buffers of this one submission, in queue order (command is the last).
     * incomplete marks a submission whose encoding failed after an earlier
     * chunk was committed; its terminal event always reports failure. */
    NSArray<id<MTLCommandBuffer>> *chunks;int incomplete;
}
@end
@implementation MagRenderSubmission @end
/* Lifetime witness attached to every scene command buffer: counts command
 * buffers not yet deallocated (process-wide), so finished GPU work that
 * something still retains (with every resource it referenced) is visible. */
#include <objc/runtime.h>
#include <stdatomic.h>
static _Atomic uint64_t metal_live_scene_commands;
@interface MagRenderCommandWitness : NSObject @end
@implementation MagRenderCommandWitness
- (void)dealloc { atomic_fetch_sub(&metal_live_scene_commands, 1); }
@end
static const char metal_witness_key = 0;
static void metal_witness(id<MTLCommandBuffer> command) {
    if (!command || objc_getAssociatedObject(command, &metal_witness_key)) return;
    atomic_fetch_add(&metal_live_scene_commands, 1);
    objc_setAssociatedObject(command, &metal_witness_key, [MagRenderCommandWitness new], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
@interface MagRenderMetal : NSObject {
@public id<MTLDevice> device; id<MTLCommandQueue> queue;
    /* id -> version -> buffer. Small integer NSNumbers are tagged pointers,
     * so lookups allocate nothing (a formatted string key did, per lookup). */
    NSMutableDictionary<NSNumber *, NSMutableDictionary<NSNumber *, MagRenderBuffer *> *> *buffers;
    /* Default-on (MAG_RENDER_METAL_RESIDENT_IMAGES=0 disables) content-verified reuse of
     * read-only uploaded scene textures across submissions. */
    NSMutableDictionary<NSNumber *, NSMutableArray *> *resident;
    NSMutableDictionary<NSNumber *, id> *resident_by_upload; /* upload buffer id -> entry */
    uint64_t resident_bytes, resident_hits, resident_misses, resident_evictions;
    uint32_t resident_entries;int resident_enabled,resident_report;
    /* One reserved scene submission: enqueued command buffer, its ticket and
     * the imports validated under the lease when it was reserved. */
    id<MTLCommandBuffer> reserved;mag_native_ticket reserved_ticket;int reserved_encoding;
    mag_native_scene_import reserved_imports[MAG_NATIVE_SCENE_MAX_IMAGES];
    mag_native_import_info reserved_info[MAG_NATIVE_SCENE_MAX_IMAGES];uint32_t reserved_count;
    /* Opt-in (MAG_RENDER_MAIN_CHUNKED_COMMIT=1): a reservation enqueues
     * chunk_slots command buffers in order so encoding can commit earlier
     * passes while later ones are still being encoded. reserved is the first;
     * reserved_chunks are the rest. chunk_stats: chunked submissions, slots
     * and non-empty slots of the last one. */
    NSMutableArray<id<MTLCommandBuffer>> *reserved_chunks;uint32_t chunk_slots;uint64_t chunk_stats[3];
    /* GPU memory diagnostics: scene submission count and report period (0 = off). */
    uint64_t scene_submissions;uint32_t memory_report_every;
    NSMutableDictionary<NSNumber *, NSNumber *> *versions;
    NSMutableDictionary<NSNumber *, MagRenderPipeline *> *pipelines;
    NSMutableArray<MagRenderSubmission *> *pending;
    NSMutableDictionary<NSNumber*,MagRenderImportedImage*> *images;uint64_t next_image;
    uint64_t generation, next_buffer, next_pipeline, next_submission;
    int timing_enabled;uint32_t timing_samples;
    mag_native_sampler_registry samplers;
    MagRenderHostOwner host;
}
@end
@implementation MagRenderMetal @end

static MagRenderMetal *context(void *opaque) { return (__bridge MagRenderMetal *)opaque; }
static MagRenderBuffer *find_buffer(MagRenderMetal *c, mag_native_buffer ref) {
    return ref.device_generation == c->generation && ref.id && ref.version ?
        c->buffers[@(ref.id)][@(ref.version)] : nil;
}
static MagRenderPipeline *find_pipeline(MagRenderMetal *c, mag_native_pipeline ref) {
    return ref.device_generation == c->generation && ref.id ? c->pipelines[@(ref.id)] : nil;
}
static int metal_caps(void *opaque, mag_native_caps_desc *out) {
    MagRenderMetal *c = context(opaque);
    *out = (mag_native_caps_desc){sizeof(*out), MAG_NATIVE_ABI_VERSION, c->generation,
        1, 1, 2, MAG_NATIVE_MAX_BINDINGS, 30, 0};
    return MAG_NATIVE_OK;
}
static int metal_buffer_create(void *opaque, mag_native_buffer previous,
    const mag_native_buffer_desc *desc, mag_native_buffer *out) {
    MagRenderMetal *c = context(opaque);
    if (!desc->byte_length || desc->byte_length % 4) return MAG_NATIVE_INVALID;
    if (desc->byte_length > UINT32_MAX || desc->byte_length > c->device.maxBufferLength)
        return MAG_NATIVE_UNSUPPORTED;
    uint64_t buffer_id, version;
    if (previous.id) {
        if (!find_buffer(c, previous)) return MAG_NATIVE_STALE;
        buffer_id = previous.id; version = [c->versions[@(buffer_id)] unsignedLongLongValue];
        if (version == UINT64_MAX) return MAG_NATIVE_STALE;
        ++version;
    } else {
        if (c->next_buffer == UINT64_MAX) return MAG_NATIVE_ALLOCATION_FAILED;
        buffer_id = ++c->next_buffer; version = 1;
    }
    id<MTLBuffer> gpu = [c->device newBufferWithLength:(NSUInteger)desc->byte_length options:MTLResourceStorageModeShared];
    if (!gpu || !gpu.contents) return MAG_NATIVE_ALLOCATION_FAILED;
    if (desc->initial_bytes) memcpy(gpu.contents, desc->initial_bytes, (size_t)desc->byte_length);
    else memset(gpu.contents, 0, (size_t)desc->byte_length);
    MagRenderBuffer *record = [MagRenderBuffer new]; record->gpu = gpu;
    record->identifier = buffer_id; record->version = version;record->seed_zero=!desc->initial_bytes;
    NSMutableDictionary<NSNumber *, MagRenderBuffer *> *live = c->buffers[@(buffer_id)];
    if (!live) { live = [NSMutableDictionary new]; c->buffers[@(buffer_id)] = live; }
    live[@(version)] = record; c->versions[@(buffer_id)] = @(version);
    *out = (mag_native_buffer){c->generation, buffer_id, version};
    return MAG_NATIVE_OK;
}
static int metal_buffer_release(void *opaque, mag_native_buffer ref) {
    MagRenderMetal *c = context(opaque);
    if (!find_buffer(c, ref)) return MAG_NATIVE_STALE;
    NSMutableDictionary<NSNumber *, MagRenderBuffer *> *live = c->buffers[@(ref.id)];
    [live removeObjectForKey:@(ref.version)];
    /* Same rule as before: the id stays addressable while any version lives. */
    if (!live.count) { [c->buffers removeObjectForKey:@(ref.id)]; [c->versions removeObjectForKey:@(ref.id)]; }
    return MAG_NATIVE_OK;
}
static int metal_buffer_read(void *opaque, mag_native_buffer ref, uint64_t offset, void *out, uint64_t length) {
    MagRenderBuffer *record = find_buffer(context(opaque), ref);
    if (!record) return MAG_NATIVE_STALE;
    if (offset > record->gpu.length || length > record->gpu.length - offset) return MAG_NATIVE_INVALID;
    if (record->readers || record->writers) return MAG_NATIVE_BUSY;
    memcpy(out, (const char *)record->gpu.contents + offset, (size_t)length);
    return MAG_NATIVE_OK;
}
static int metal_pipeline_create(void *opaque, const mag_native_pipeline_desc *desc, mag_native_pipeline *out) {
    MagRenderMetal *c = context(opaque);
    if (desc->shader_language != MAG_NATIVE_MSL) return MAG_NATIVE_UNSUPPORTED;
    if (!desc->source_bytes || !desc->source_length || desc->source_length > 1024 * 1024 ||
        !desc->entry_point || !desc->entry_point_length || desc->entry_point_length > 128 ||
        !desc->bindings || !desc->binding_count || desc->binding_count > MAG_NATIVE_MAX_BINDINGS)
        return MAG_NATIVE_INVALID;
    uint32_t slots = 0, indices = 0, sizes_count = 0, threads = 1;
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t n = desc->threads_per_group[i];
        if (!n || n > 256 || threads > 256 / n) return MAG_NATIVE_UNSUPPORTED;
        threads *= n;
    }
    for (uint32_t i = 0; i < desc->binding_count; ++i) {
        mag_native_binding_layout b = desc->bindings[i];
        if (b.slot >= MAG_NATIVE_MAX_BINDINGS || slots & (1u << b.slot) || !b.minimum_bytes ||
            b.minimum_bytes % 4 || b.minimum_bytes > UINT32_MAX || b.binding_kind > MAG_NATIVE_UNIFORM ||
            (b.binding_kind == MAG_NATIVE_UNIFORM && (b.access != MAG_NATIVE_READ || b.runtime_array_stride)) ||
            b.alignment_bytes < 4 || (b.alignment_bytes & (b.alignment_bytes - 1)) || b.alignment_bytes > 256 ||
            (b.access != MAG_NATIVE_READ && b.access != MAG_NATIVE_READ_WRITE)) return MAG_NATIVE_INVALID;
        slots |= 1u << b.slot;
        if (b.runtime_array_stride) {
            /* The translator can describe struct headers before arrays, but
             * this first native ABI route only supports byte-zero arrays. */
            if (b.minimum_bytes != b.runtime_array_stride) return MAG_NATIVE_UNSUPPORTED;
            if (b.runtime_array_stride % 4 || b.minimum_bytes < b.runtime_array_stride ||
                b.runtime_size_index >= MAG_NATIVE_MAX_BINDINGS || indices & (1u << b.runtime_size_index))
                return MAG_NATIVE_INVALID;
            indices |= 1u << b.runtime_size_index; ++sizes_count;
        } else if (b.runtime_size_index != MAG_NATIVE_NO_SIZE_INDEX) return MAG_NATIVE_INVALID;
    }
    if (indices != (1u << sizes_count) - 1) return MAG_NATIVE_INVALID;
    NSString *source = [[NSString alloc] initWithBytes:desc->source_bytes length:(NSUInteger)desc->source_length encoding:NSUTF8StringEncoding];
    NSString *entry = [[NSString alloc] initWithBytes:desc->entry_point length:desc->entry_point_length encoding:NSUTF8StringEncoding];
    if (!source || !entry) return MAG_NATIVE_INVALID;
    MTLCompileOptions *options = [MTLCompileOptions new];
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
    if (@available(macOS 15.0, iOS 18.0, *)) {
        options.mathMode = MTLMathModeSafe;
        options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
    } else
#endif
    { [options setValue:@NO forKey:@"fastMathEnabled"]; }
    options.languageVersion = MTLLanguageVersion2_0;
    NSError *error = nil;
    id<MTLLibrary> library = [c->device newLibraryWithSource:source options:options error:&error];
    id<MTLFunction> function = [library newFunctionWithName:entry];
    id<MTLComputePipelineState> gpu = function ? [c->device newComputePipelineStateWithFunction:function error:&error] : nil;
    if (!gpu) {
        fprintf(stderr, "[rsx frontend/metal] pipeline compilation failed: %s\n", error.localizedDescription.UTF8String ?: "entry point unavailable");
        return MAG_NATIVE_UNSUPPORTED;
    }
    MTLSize limits = c->device.maxThreadsPerThreadgroup;
    if (threads > gpu.maxTotalThreadsPerThreadgroup || desc->threads_per_group[0] > limits.width ||
        desc->threads_per_group[1] > limits.height || desc->threads_per_group[2] > limits.depth)
        return MAG_NATIVE_UNSUPPORTED;
    if (c->next_pipeline == UINT64_MAX) return MAG_NATIVE_ALLOCATION_FAILED;
    MagRenderPipeline *record = [MagRenderPipeline new]; record->gpu = gpu;
    record->threads = MTLSizeMake(desc->threads_per_group[0], desc->threads_per_group[1], desc->threads_per_group[2]);
    record->binding_count = desc->binding_count; record->sizes_count = sizes_count;
    memcpy(record->bindings, desc->bindings, sizeof(*desc->bindings) * desc->binding_count);
    uint64_t id = ++c->next_pipeline; c->pipelines[@(id)] = record;
    *out = (mag_native_pipeline){c->generation, id};
    return MAG_NATIVE_OK;
}
static int metal_pipeline_release(void *opaque, mag_native_pipeline ref) {
    MagRenderMetal *c = context(opaque);
    if (!find_pipeline(c, ref)) return MAG_NATIVE_STALE;
    [c->pipelines removeObjectForKey:@(ref.id)]; return MAG_NATIVE_OK;
}
static void release_uses(MagRenderSubmission *submission) {
    for (MagRenderUse *use in submission->uses) {
        if (use->write) --use->buffer->writers; else --use->buffer->readers;
    }
}
static int metal_submit_owned(void *opaque, const mag_native_frame *frame, mag_native_ticket *out) {
    MagRenderMetal *c = context(opaque);
    if (c->pending.count >= 2) return MAG_NATIVE_BUSY;
    MagRenderPipeline *pipeline = find_pipeline(c, frame->pipeline);
    if (!pipeline) return MAG_NATIVE_STALE;
    if (!frame->bindings || frame->binding_count != pipeline->binding_count) return MAG_NATIVE_INVALID;
    for (unsigned i = 0; i < 3; ++i) if (!frame->groups[i] || frame->groups[i] > 65535) return MAG_NATIVE_INVALID;
    if (c->next_submission == UINT64_MAX) return MAG_NATIVE_DEVICE_FAILED;
    NSMutableArray<MagRenderUse *> *uses = [NSMutableArray new];
    uint32_t slots = 0, lengths[MAG_NATIVE_MAX_BINDINGS] = {0};
    for (uint32_t i = 0; i < frame->binding_count; ++i) {
        mag_native_binding b = frame->bindings[i];
        if (b.slot >= MAG_NATIVE_MAX_BINDINGS || b.reserved || slots & (1u << b.slot)) return MAG_NATIVE_INVALID;
        slots |= 1u << b.slot;
        const mag_native_binding_layout *schema = NULL;
        for (uint32_t j = 0; j < pipeline->binding_count; ++j) if (pipeline->bindings[j].slot == b.slot) schema = &pipeline->bindings[j];
        MagRenderBuffer *buffer = find_buffer(c, b.buffer);
        if (!buffer) return MAG_NATIVE_STALE;
        if (!schema || b.offset % schema->alignment_bytes || b.byte_length < schema->minimum_bytes || b.byte_length > UINT32_MAX ||
            b.offset > buffer->gpu.length || b.byte_length > buffer->gpu.length - b.offset ||
            (schema->runtime_array_stride && b.byte_length % schema->runtime_array_stride)) return MAG_NATIVE_INVALID;
        int write = schema->access == MAG_NATIVE_READ_WRITE;
        if (buffer->writers || (write && buffer->readers)) return MAG_NATIVE_BUSY;
        for (MagRenderUse *prior in uses) if (prior->buffer == buffer && (write || prior->write)) return MAG_NATIVE_INVALID;
        MagRenderUse *use = [MagRenderUse new]; use->buffer = buffer; use->write = write; [uses addObject:use];
        if (schema->runtime_array_stride) lengths[schema->runtime_size_index] = (uint32_t)b.byte_length;
    }
    MagRenderSubmission *submission = [MagRenderSubmission new]; submission->pipeline = pipeline; submission->uses = uses;
    submission->command = [c->queue commandBuffer];
    if (!submission->command) return MAG_NATIVE_DEVICE_FAILED;
    id<MTLComputeCommandEncoder> encoder = [submission->command computeCommandEncoder];
    if (!encoder) return MAG_NATIVE_DEVICE_FAILED;
    [encoder setComputePipelineState:pipeline->gpu];
    for (uint32_t i = 0; i < frame->binding_count; ++i) {
        mag_native_binding b = frame->bindings[i];
        [encoder setBuffer:((MagRenderUse *)uses[i])->buffer->gpu offset:(NSUInteger)b.offset atIndex:b.slot];
    }
    if (pipeline->sizes_count) {
        submission->sizes = [c->device newBufferWithBytes:lengths length:pipeline->sizes_count * sizeof(uint32_t) options:MTLResourceStorageModeShared];
        if (!submission->sizes) { [encoder endEncoding]; return MAG_NATIVE_ALLOCATION_FAILED; }
        [encoder setBuffer:submission->sizes offset:0 atIndex:30];
    }
    [encoder dispatchThreadgroups:MTLSizeMake(frame->groups[0], frame->groups[1], frame->groups[2]) threadsPerThreadgroup:pipeline->threads];
    [encoder endEncoding];
    submission->ticket = (mag_native_ticket){c->generation, ++c->next_submission};
    for (MagRenderUse *use in uses) { if (use->write) {++use->buffer->writers;use->buffer->gpu_written=1;} else ++use->buffer->readers; }
    [c->pending addObject:submission]; *out = submission->ticket;
    @try { [submission->command commit]; }
    @catch (NSException *error) {
        (void)error;
        if (submission->command.status >= MTLCommandBufferStatusCommitted) return MAG_NATIVE_INDETERMINATE;
        release_uses(submission); [c->pending removeObjectIdenticalTo:submission]; memset(out, 0, sizeof(*out));
        return MAG_NATIVE_DEVICE_FAILED;
    }
    return MAG_NATIVE_OK;
}
static int metal_submit(void *opaque,const mag_native_frame *frame,mag_native_ticket *out) {
    MagRenderMetal *c=context(opaque);
    int status=c->host.try_begin(c->host.userdata,c->host.lifetime);
    if (status!=MAG_NATIVE_OK) return status<0?status:MAG_NATIVE_DEVICE_FAILED;
    @try { return metal_submit_owned(opaque,frame,out); }
    @catch (NSException *error) { (void)error;return out->submission_id?MAG_NATIVE_INDETERMINATE:MAG_NATIVE_DEVICE_FAILED; }
    @finally { c->host.end(c->host.userdata,c->host.lifetime); }
}
static void metal_memory_report(MagRenderMetal *c);
#include "mag_render_metal_sampler.inc"
#include "mag_render_metal_batch.inc"
#include "mag_render_metal_scene.inc"
static int metal_poll(void *opaque, mag_native_event *events, uint32_t capacity, uint32_t *count) {
    MagRenderMetal *c = context(opaque);
    for (NSUInteger i = 0; i < c->pending.count && *count < capacity;) {
        MagRenderSubmission *submission = c->pending[i];
        MTLCommandBufferStatus status = submission->command.status;
        if (status != MTLCommandBufferStatusCompleted && status != MTLCommandBufferStatusError) { ++i; continue; }
        /* A chunked submission is terminal only when every one of its command
         * buffers is; any failed chunk (or a failed encoding) fails it. */
        int failed=status!=MTLCommandBufferStatusCompleted||submission->command.error||submission->incomplete,waiting=0;
        for(id<MTLCommandBuffer> chunk in submission->chunks){MTLCommandBufferStatus s=chunk.status;
            if(s!=MTLCommandBufferStatusCompleted&&s!=MTLCommandBufferStatusError)waiting=1;
            else if(s==MTLCommandBufferStatusError||chunk.error){failed=1;status=MTLCommandBufferStatusError;}}
        if(waiting){++i;continue;}
        if(!failed && submission->packed_source){
            for(uint32_t y=0;y<submission->packed_height;++y)
                memcpy((char*)submission->packed_destination->gpu.contents+(size_t)y*submission->packed_width*4,
                    (const char*)submission->packed_source.contents+(size_t)y*submission->packed_pitch,(size_t)submission->packed_width*4);
        }
        if(c->timing_enabled && c->timing_samples<256) {
            ++c->timing_samples;
            const double start=submission->chunks.count?submission->chunks[0].GPUStartTime:submission->command.GPUStartTime,end=submission->command.GPUEndTime;
            const int available=!failed &&
                isfinite(start) && isfinite(end) && start>0 && end>=start;
            fprintf(stderr,"[mag native gpu timing] generation=%llu ticket=%llu status=%u available=%d gpu_start_seconds=%.9f gpu_end_seconds=%.9f gpu_ms=%.6f\n",
                (unsigned long long)submission->ticket.device_generation,
                (unsigned long long)submission->ticket.submission_id,(unsigned)status,available,start,end,
                available?(end-start)*1000.0:-1.0);
        }
        events[(*count)++] = (mag_native_event){sizeof(*events), MAG_NATIVE_ABI_VERSION, submission->ticket,
            !failed, 1, (uint32_t)status, 0};
        release_uses(submission); [c->pending removeObjectAtIndex:i];
    }
    return MAG_NATIVE_OK;
}
static int metal_close(void *opaque) {
    MagRenderMetal *c=context(opaque);
    if (c->pending.count || c->reserved) return MAG_NATIVE_BUSY;
    MagRenderHostOwner host=c->host;
    int sampler_status=mag_native_sampler_registry_close(&c->samplers);if(sampler_status)return sampler_status;
    [c->buffers removeAllObjects];[c->resident removeAllObjects];[c->resident_by_upload removeAllObjects];[c->pipelines removeAllObjects];[c->versions removeAllObjects];[c->images removeAllObjects];
    c->queue=nil;c->device=nil;
    (void)CFBridgingRelease(opaque);
    host.release(host.userdata,host.lifetime);return MAG_NATIVE_OK;
}
/* In-place upload rewrite for opt-in arena reuse. Gated exactly like
 * readback: no submission that used this buffer may still be pending or
 * unpolled, so the GPU can never observe a partially rewritten buffer. */
static int metal_buffer_write(void *opaque, mag_native_buffer ref, uint64_t offset, const void *source, uint64_t length) {
    MagRenderBuffer *record = find_buffer(context(opaque), ref);
    if (!record) return MAG_NATIVE_STALE;
    if (record->readers || record->writers) return MAG_NATIVE_BUSY;
    if (record->gpu_written || offset > record->gpu.length || length > record->gpu.length - offset)
        return MAG_NATIVE_INVALID;
    memcpy((char *)record->gpu.contents + offset, source, (size_t)length);
    record->seed_zero = 0;
    return MAG_NATIVE_OK;
}
static void metal_memory_census(MagRenderMetal *c,uint64_t out[MAG_NATIVE_MEMORY_STATS]){
    uint64_t buffers=0;for(NSNumber *id in c->buffers)buffers+=c->buffers[id].count;
    uint64_t queued=0;for(MagRenderSubmission *s in c->pending)queued+=1+s->chunks.count;
    const uint64_t live=atomic_load(&metal_live_scene_commands);
    out[0]=(uint64_t)c->device.currentAllocatedSize;out[1]=buffers;out[2]=c->pipelines.count;out[3]=c->images.count;
    out[4]=c->resident_entries;out[5]=c->pending.count;out[6]=c->samplers.count;out[7]=live>queued?live-queued:0;out[8]=c->scene_submissions;
}
static int metal_memory_stats(void *opaque,uint64_t *out,uint32_t count){
    if(count!=MAG_NATIVE_MEMORY_STATS)return MAG_NATIVE_INVALID;
    metal_memory_census(context(opaque),out);return MAG_NATIVE_OK;
}
/* Periodic census line, default on (MAG_RENDER_GPU_MEMORY_REPORT=0 disables):
 * the first two scene submissions, then every 120th. */
static void metal_memory_report(MagRenderMetal *c){
    const uint64_t n=++c->scene_submissions;
    if(!c->memory_report_every||(n>2&&n%c->memory_report_every))return;
    uint64_t m[MAG_NATIVE_MEMORY_STATS];metal_memory_census(c,m);
    fprintf(stderr,"[mag native gpu memory] submission=%llu allocated_mb=%.1f buffers=%llu pipelines=%llu images=%llu resident=%llu resident_mb=%.1f pending=%llu samplers=%llu retired_alive=%llu\n",
        (unsigned long long)n,(double)m[0]/1048576.0,(unsigned long long)m[1],(unsigned long long)m[2],(unsigned long long)m[3],
        (unsigned long long)m[4],(double)c->resident_bytes/1048576.0,(unsigned long long)m[5],(unsigned long long)m[6],(unsigned long long)m[7]);
}
/* Every operation runs in its own autorelease pool. Callers are plain
 * threads (the Rust worker, FIFO or host threads) with no pool of their own,
 * so Metal's autoreleased command buffers, encoders and descriptors would
 * otherwise live until the thread exits, keeping every resource each
 * finished submission referenced. */
static int metal_pooled_caps(void *a0, mag_native_caps_desc *a1){@autoreleasepool{return metal_caps(a0,a1);}}
static int metal_pooled_buffer_create(void *a0, mag_native_buffer a1, const mag_native_buffer_desc *a2, mag_native_buffer *a3){@autoreleasepool{return metal_buffer_create(a0,a1,a2,a3);}}
static int metal_pooled_buffer_release(void *a0, mag_native_buffer a1){@autoreleasepool{return metal_buffer_release(a0,a1);}}
static int metal_pooled_buffer_read(void *a0, mag_native_buffer a1, uint64_t a2, void *a3, uint64_t a4){@autoreleasepool{return metal_buffer_read(a0,a1,a2,a3,a4);}}
static int metal_pooled_pipeline_create(void *a0, const mag_native_pipeline_desc *a1, mag_native_pipeline *a2){@autoreleasepool{return metal_pipeline_create(a0,a1,a2);}}
static int metal_pooled_pipeline_release(void *a0, mag_native_pipeline a1){@autoreleasepool{return metal_pipeline_release(a0,a1);}}
static int metal_pooled_submit(void *a0, const mag_native_frame *a1, mag_native_ticket *a2){@autoreleasepool{return metal_submit(a0,a1,a2);}}
static int metal_pooled_poll(void *a0, mag_native_event *a1, uint32_t a2, uint32_t *a3){@autoreleasepool{return metal_poll(a0,a1,a2,a3);}}
static int metal_pooled_close(void *a0){@autoreleasepool{return metal_close(a0);}}
static int metal_pooled_graphics_create(void *a0, const mag_native_graphics_desc *a1, mag_native_pipeline *a2){@autoreleasepool{return metal_graphics_create(a0,a1,a2);}}
static int metal_pooled_submit_batch(void *a0, const mag_native_batch *a1, mag_native_ticket *a2){@autoreleasepool{return metal_submit_batch(a0,a1,a2);}}
static int metal_pooled_scene_graphics_create(void *a0, const mag_native_scene_graphics_desc *a1, mag_native_pipeline *a2){@autoreleasepool{return metal_scene_graphics_create(a0,a1,a2);}}
static int metal_pooled_submit_scene(void *a0, const mag_native_scene *a1, mag_native_ticket *a2){@autoreleasepool{return metal_submit_scene(a0,a1,a2);}}
static int metal_pooled_image_import(void *a0, const MagRenderHostImage *a1, mag_native_image *a2){@autoreleasepool{return metal_image_import(a0,a1,a2);}}
static int metal_pooled_image_release(void *a0, mag_native_image a1){@autoreleasepool{return metal_image_release(a0,a1);}}
static int metal_pooled_submit_scene_target(void *a0, const mag_native_scene *a1, mag_native_image a2, mag_native_ticket *a3){@autoreleasepool{return metal_submit_scene_target(a0,a1,a2,a3);}}
static int metal_pooled_submit_scene_frame(void *a0, const mag_native_scene *a1, mag_native_image a2, const MagRenderHostFrame *a3, mag_native_ticket *a4){@autoreleasepool{return metal_submit_scene_frame(a0,a1,a2,a3,a4);}}
static int metal_pooled_scene_graphics_create_v2(void *a0, const mag_native_scene_graphics_v2_desc *a1, mag_native_pipeline *a2){@autoreleasepool{return metal_scene_graphics_create_v2(a0,a1,a2);}}
static int metal_pooled_scene_graphics_create_v3(void *a0, const mag_native_scene_graphics_v3_desc *a1, mag_native_pipeline *a2){@autoreleasepool{return metal_scene_graphics_create_v3(a0,a1,a2);}}
static int metal_pooled_sampler_create(void *a0, const mag_native_sampler_desc *a1, mag_native_sampler *a2){@autoreleasepool{return metal_sampler_create(a0,a1,a2);}}
static int metal_pooled_sampler_release(void *a0, mag_native_sampler a1){@autoreleasepool{return metal_sampler_release(a0,a1);}}
static int metal_pooled_submit_scene_v2(void *a0, const mag_native_scene_v2 *a1, const mag_native_image *a2, const MagRenderHostFrame *a3, mag_native_ticket *a4){@autoreleasepool{return metal_submit_scene_v2(a0,a1,a2,a3,a4);}}
static int metal_pooled_submit_scene_v3(void *a0, const mag_native_scene_v3 *a1, const mag_native_image *a2, const MagRenderHostFrame *a3, mag_native_ticket *a4){@autoreleasepool{return metal_submit_scene_v3(a0,a1,a2,a3,a4);}}
static int metal_pooled_submit_scene_v4(void *a0, const mag_native_scene_v4 *a1, const mag_native_image *a2, const MagRenderHostFrame *a3, mag_native_ticket *a4){@autoreleasepool{return metal_submit_scene_v4(a0,a1,a2,a3,a4);}}
static int metal_pooled_image_import_certified(void *a0, const mag_native_image_certificate *a1, mag_native_image *a2){@autoreleasepool{return metal_image_import_certified(a0,a1,a2);}}
static int metal_pooled_scene_graphics_create_aux(void *a0, const mag_native_scene_graphics_v3_desc *a1, uint32_t a2, mag_native_pipeline *a3){@autoreleasepool{return metal_scene_graphics_create_aux(a0,a1,a2,a3);}}
static int metal_pooled_submit_scene_v5(void *a0, const mag_native_scene_v5 *a1, const mag_native_image *a2, const MagRenderHostFrame *a3, mag_native_ticket *a4){@autoreleasepool{return metal_submit_scene_v5(a0,a1,a2,a3,a4);}}
static int metal_pooled_reserve_scene(void *a0, const mag_native_scene_import *a1, uint32_t a2, mag_native_ticket *a3){@autoreleasepool{return metal_reserve_scene(a0,a1,a2,a3);}}
static int metal_pooled_abandon_scene_reservation(void *a0){@autoreleasepool{return metal_abandon_scene_reservation(a0);}}
static int metal_pooled_buffer_write(void *a0, mag_native_buffer a1, uint64_t a2, const void *a3, uint64_t a4){@autoreleasepool{return metal_buffer_write(a0,a1,a2,a3,a4);}}
static int metal_pooled_scene_chunk_stats(void *a0, uint64_t *a1, uint32_t a2){@autoreleasepool{return metal_scene_chunk_stats(a0,a1,a2);}}
static int metal_pooled_memory_stats(void *a0, uint64_t *a1, uint32_t a2){@autoreleasepool{return metal_memory_stats(a0,a1,a2);}}
static const mag_native_ops metal_ops = {
    metal_pooled_caps,
    metal_pooled_buffer_create,
    metal_pooled_buffer_release,
    metal_pooled_buffer_read,
    metal_pooled_pipeline_create,
    metal_pooled_pipeline_release,
    metal_pooled_submit,
    metal_pooled_poll,
    metal_pooled_close,
    metal_pooled_graphics_create,
    metal_pooled_submit_batch,
    metal_pooled_scene_graphics_create,
    metal_pooled_submit_scene,
    metal_pooled_image_import,
    metal_pooled_image_release,
    metal_pooled_submit_scene_target,
    metal_pooled_submit_scene_frame,
    metal_pooled_scene_graphics_create_v2,
    metal_pooled_scene_graphics_create_v3,
    metal_pooled_sampler_create,
    metal_pooled_sampler_release,
    metal_pooled_submit_scene_v2,
    metal_pooled_submit_scene_v3,
    metal_pooled_submit_scene_v4,
    metal_pooled_image_import_certified,
    metal_pooled_scene_graphics_create_aux,
    metal_pooled_submit_scene_v5,
    metal_pooled_reserve_scene,
    metal_pooled_abandon_scene_reservation,
    metal_pooled_buffer_write,
    metal_pooled_scene_chunk_stats,
    metal_pooled_memory_stats};
int mag_native_metal_create(const MagRenderHostOwner *owner,mag_native_device **out) {
    if (!owner || !out || owner->backend!=MAG_RENDER_HOST_METAL) return MAG_NATIVE_INVALID;
    *out=NULL;int status=owner->retain(owner->userdata,owner->lifetime);
    if (status!=MAG_NATIVE_OK) return status<0?status:MAG_NATIVE_DEVICE_FAILED;
    // Native pointers are touched only after the host lifetime was pinned.
    id<MTLDevice> device=(__bridge id<MTLDevice>)owner->device;
    id<MTLCommandQueue> queue=(__bridge id<MTLCommandQueue>)owner->queue;
    if (!device || !queue || queue.device!=device) {owner->release(owner->userdata,owner->lifetime);return MAG_NATIVE_INVALID;}
    MagRenderMetal *c=[MagRenderMetal new];c->device=device;c->queue=queue;c->host=*owner;
    const char *timing=getenv("MAG_RENDER_NATIVE_GPU_TIMING");c->timing_enabled=timing && !strcmp(timing,"1");
    c->generation=mag_native_next_generation();
    if (!c->generation) {owner->release(owner->userdata,owner->lifetime);return MAG_NATIVE_ALLOCATION_FAILED;}
    mag_native_sampler_registry_init(&c->samplers,c->generation,NULL,metal_sampler_destroy);
    c->buffers=[NSMutableDictionary new];c->versions=[NSMutableDictionary new];c->resident=[NSMutableDictionary new];c->resident_by_upload=[NSMutableDictionary new];
    const char *resident=getenv("MAG_RENDER_METAL_RESIDENT_IMAGES");c->resident_enabled=!(resident&&!strcmp(resident,"0"));
    const char *report=getenv("MAG_RENDER_FIFO_TIMING");c->resident_report=report&&!strcmp(report,"1");
    c->reserved_chunks=[NSMutableArray new];
    const char *memory=getenv("MAG_RENDER_GPU_MEMORY_REPORT");c->memory_report_every=memory&&!strcmp(memory,"0")?0u:120u;
    const char *chunked=getenv("MAG_RENDER_MAIN_CHUNKED_COMMIT");
    if(chunked&&!strcmp(chunked,"1")){const char *n=getenv("MAG_RENDER_MAIN_CHUNKS");long slots=n?strtol(n,NULL,10):4;c->chunk_slots=(uint32_t)(slots<2?2:slots>8?8:slots);}
    c->pipelines=[NSMutableDictionary new];c->pending=[NSMutableArray new];c->images=[NSMutableDictionary new];
    void *opaque=(__bridge_retained void *)c;
    *out=mag_native_wrap(&metal_ops,opaque);
    if (!*out) {metal_close(opaque);return MAG_NATIVE_ALLOCATION_FAILED;}
    return MAG_NATIVE_OK;
}
