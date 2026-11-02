#include "mag_render_batch.h"
#import <Foundation/Foundation.h>
#include <cassert>
#include <cstdio>
#include <cstring>
extern "C" int mag_render_test_owner_open(MagRenderHostOwner *);
extern "C" int mag_render_test_owner_gate(void);
extern "C" const char *mag_render_test_owner_name(void);
extern "C" void mag_render_test_owner_signal(void *);
extern "C" void mag_render_test_owner_close(int);
struct DrawProbeReport {
    uint32_t abi_version, struct_size;
    int32_t status;
    uint32_t submitted, completed, pending;
    uint64_t retained_before_terminal, retained_after_terminal;
    uint32_t center_rgba, outside_rgba;
};
extern "C" int mag_render_native_draw_probe(const MagRenderHostOwner *, const mag_native_pipeline_desc *,
                                            const mag_native_pipeline_desc *,
                                            const mag_native_graphics_desc *, uint32_t, void (*)(void *),
                                            void *, DrawProbeReport *);
#include "metal_shader.h"
static void check_rejected_batches(const MagRenderHostOwner *owner, const mag_native_pipeline_desc *source) {
    mag_native_device *device = NULL;
    assert(mag_native_create(owner, &device) == 0);
    mag_native_pipeline pipeline = {};
    assert(mag_native_pipeline_create(device, source, &pipeline) == 0);
    mag_native_buffer buffers[3] = {};
    uint8_t parameters[80] = {};
    for (unsigned i = 0; i < 3; ++i) {
        mag_native_buffer_desc desc = {sizeof(desc), 1, i == 2 ? 80u : 48u, i == 2 ? parameters : NULL};
        assert(mag_native_buffer_create(device, &desc, &buffers[i]) == 0);
    }
    mag_native_batch_resource resources[3] = {{buffers[0], MAG_NATIVE_PRODUCED, 0},
                                              {buffers[1], MAG_NATIVE_PRODUCED, 0},
                                              {buffers[2], MAG_NATIVE_UPLOAD, 0}};
    mag_native_binding bindings[3] = {
        {0, 0, buffers[0], 0, 48}, {1, 0, buffers[1], 0, 48}, {2, 0, buffers[2], 0, 80}};
    mag_native_frame frame = {sizeof(frame), 1, pipeline, bindings, 3, {1, 1, 1}};
    mag_native_batch_command commands[2] = {{MAG_NATIVE_DISPATCH, 0, &frame, NULL},
                                            {MAG_NATIVE_DISPATCH, 0, &frame, NULL}};
    mag_native_batch batch = {sizeof(batch), 1, resources, 3, 1, commands};
    auto rejected = [&](int expected) {
        mag_native_ticket ticket = {};
        assert(mag_native_submit_batch(device, &batch, &ticket) == expected);
        assert(!ticket.submission_id && !ticket.device_generation);
        mag_native_event event = {};
        uint32_t count = 0;
        assert(mag_native_poll(device, &event, 1, &count) == 0 && count == 0);
    };
    rejected(MAG_NATIVE_INVALID); // no producer for source GPU value
    resources[0].role = MAG_NATIVE_UPLOAD;
    resources[1].role = MAG_NATIVE_UPLOAD;
    rejected(MAG_NATIVE_INVALID); // immutable upload write
    resources[1].role = MAG_NATIVE_PRODUCED;
    batch.command_count = 2;
    rejected(MAG_NATIVE_INVALID); // second writer
    batch.command_count = 1;
    bindings[1].buffer = buffers[0];
    rejected(MAG_NATIVE_INVALID); // same-node read/write alias
    bindings[1].buffer = buffers[1];
    bindings[0].buffer.device_generation++;
    rejected(MAG_NATIVE_INVALID); // outside admitted resource set
    bindings[0].buffer = buffers[0];
    resources[0].buffer.version++;
    rejected(MAG_NATIVE_STALE);
    resources[0].buffer = buffers[0];
    bindings[0].offset = UINT64_MAX;
    rejected(MAG_NATIVE_INVALID);
    bindings[0].offset = 0;
    frame.pipeline.id = UINT64_MAX;
    rejected(MAG_NATIVE_STALE);
    for (auto buffer : buffers)
        assert(mag_native_buffer_release(device, buffer) == 0);
    assert(mag_native_pipeline_release(device, pipeline) == 0);
    assert(mag_native_close(device) == 0);
}
int mag_render_draw_main(const char *directory, bool diagnostic) {
    @autoreleasepool {
        NSString *root = [NSString stringWithUTF8String:directory];
        Shader first = {}, legacy = {}, prepare = {}, vertex = {}, fragment = {};
        if (!first.load(root, @"scene_transform", true) || !legacy.load(root, @"copy_transform", true) ||
            !prepare.load(root, @"scene_prepare", true) || !vertex.load(root, @"scene_vertex", false) ||
            !fragment.load(root, @"scene_fragment", false))
            return 2;
        mag_native_graphics_desc graphics = {sizeof(graphics), 1, vertex.stage(), fragment.stage(), 16, 0};
        MagRenderHostOwner owner = {};
        if (mag_render_test_owner_open(&owner) || mag_render_test_owner_gate()) {
            mag_render_test_owner_close(1);
            return 3;
        }
        check_rejected_batches(&owner, &legacy.compute);
        NSString *name = [NSString stringWithUTF8String:mag_render_test_owner_name()];
        DrawProbeReport report = {};
        report.abi_version = 1;
        report.struct_size = sizeof(report);
        int status =
            mag_render_native_draw_probe(&owner, &first.compute, &prepare.compute, &graphics,
                                         diagnostic ? 1u : 0u, mag_render_test_owner_signal, NULL, &report);
        mag_render_test_owner_signal(NULL);
        mag_render_test_owner_close(report.pending == 0);
        NSDictionary *result = @{
            @"device" : name,
            @"status" : @(status),
            @"submitted" : @(report.submitted),
            @"completed" : @(report.completed),
            @"pending" : @(report.pending),
            @"retained_before_terminal" : @(report.retained_before_terminal),
            @"retained_after_terminal" : @(report.retained_after_terminal),
            @"center_rgba" : @(report.center_rgba),
            @"outside_rgba" : @(report.outside_rgba),
            @"produced_uniform_consumer" : @YES,
            @"native_rejection_cases" : @8,
            @"diagnostic_readback" : @(diagnostic),
            @"compute_dispatches_per_batch" : @2,
            @"indexed_draws_per_batch" : @1
        };
        NSData *encoded = [NSJSONSerialization dataWithJSONObject:result
                                                          options:NSJSONWritingSortedKeys
                                                            error:NULL];
        printf("%s\n", [[[NSString alloc] initWithData:encoded encoding:NSUTF8StringEncoding] UTF8String]);
        return status == 0 && report.submitted == 2 && report.completed == 2 && report.pending == 0 &&
                       report.retained_before_terminal == 208 && report.retained_after_terminal == 0 &&
                       (!diagnostic || (report.center_rgba == 0xffff9933u && report.outside_rgba == 0))
                   ? 0
                   : 1;
    }
}
