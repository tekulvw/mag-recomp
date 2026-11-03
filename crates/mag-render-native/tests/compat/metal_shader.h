#pragma once
#include "mag_render_batch.h"
#import <Foundation/Foundation.h>
#include <cstring>
struct Shader {
    NSData *source;
    NSString *entry;
    NSDictionary *manifest;
    mag_native_binding_layout layouts[16];
    mag_native_pipeline_desc compute;
    bool load(NSString *root, NSString *name, bool is_compute) {
        root = [root stringByAppendingPathComponent:name];
        source = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"shader.metal"]];
        NSData *metadata =
            [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"manifest.json"]];
        if (!source || !metadata)
            return false;
        manifest = [NSJSONSerialization JSONObjectWithData:metadata options:0 error:NULL];
        if ([manifest[@"schema_version"] unsignedIntValue] != 1)
            return false;
        entry = manifest[@"targets"][@"msl"][@"entry_point"];
        if (!entry)
            return false;
        if (!is_compute)
            return true;
        NSArray *buffers = manifest[@"buffers"];
        if (!buffers.count || buffers.count > 16)
            return false;
        for (NSUInteger i = 0; i < buffers.count; ++i) {
            NSDictionary *b = buffers[i];
            uint32_t index = MAG_NATIVE_NO_SIZE_INDEX;
            for (NSDictionary *size in manifest[@"metal_sizes_buffer"][@"entries"])
                if ([size[@"binding"] isEqual:b[@"binding"]])
                    index = [size[@"offset_bytes"] unsignedIntValue] / 4;
            layouts[i] = {
                [b[@"binding"] unsignedIntValue],
                [b[@"access"] isEqual:@"read"] ? MAG_NATIVE_READ : MAG_NATIVE_READ_WRITE,
                [b[@"minimum_binding_bytes"] unsignedLongLongValue],
                index == MAG_NATIVE_NO_SIZE_INDEX ? 0 : [b[@"layout"][@"stride_bytes"] unsignedIntValue],
                index,
                [b[@"layout"][@"alignment_bytes"] unsignedIntValue],
                [b[@"address_space"] isEqual:@"uniform"] ? MAG_NATIVE_UNIFORM : MAG_NATIVE_STORAGE};
        }
        compute = {};
        compute.struct_size = sizeof(compute);
        compute.abi_version = 1;
        compute.source_bytes = source.bytes;
        compute.source_length = source.length;
        compute.entry_point = entry.UTF8String;
        compute.entry_point_length = uint32_t(strlen(entry.UTF8String));
        compute.binding_count = uint32_t(buffers.count);
        compute.bindings = layouts;
        compute.shader_language = MAG_NATIVE_MSL;
        for (unsigned i = 0; i < 3; ++i)
            compute.threads_per_group[i] = [manifest[@"workgroup_size"][i] unsignedIntValue];
        return true;
    }
    mag_native_shader_stage stage() const {
        return {source.bytes, source.length, entry.UTF8String, uint32_t(strlen(entry.UTF8String)),
                MAG_NATIVE_MSL};
    }
};
