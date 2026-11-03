#include "mag_render_batch.h"
#include "mag_render_native_api.h"
#include "mag_render_imported_image.h"
#include MAG_RENDER_SHADER_FIXTURE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int mag_render_test_owner_open(MagRenderHostOwner *);
int mag_render_test_owner_contract(void);
int mag_render_test_owner_gate(void);
const char *mag_render_test_owner_name(void);
void mag_render_test_owner_signal(void *);
void mag_render_test_owner_close(int);
void mag_render_test_owner_require_hardware(uint32_t, uint32_t);
typedef struct DrawProbeReport {
    uint32_t abi_version, struct_size;
    int32_t status;
    uint32_t submitted, completed, pending;
    uint64_t retained_before_terminal, retained_after_terminal;
    uint32_t center_rgba, outside_rgba;
} DrawProbeReport;
int mag_render_native_draw_probe(const MagRenderHostOwner *, const mag_native_pipeline_desc *,
                                 const mag_native_pipeline_desc *, const mag_native_graphics_desc *, uint32_t,
                                 void (*)(void *), void *, DrawProbeReport *);
static mag_native_pipeline_desc compute(const mag_native_fixture_shader *source) {
    mag_native_pipeline_desc out = {.struct_size = sizeof(out),
                                    .abi_version = 1,
                                    .source_bytes = source->source,
                                    .source_length = source->length,
                                    .entry_point = source->entry,
                                    .entry_point_length = (uint32_t)strlen(source->entry),
                                    .binding_count = source->binding_count,
                                    .bindings = source->bindings,
                                    .shader_language = MAG_NATIVE_SPIRV};
    memcpy(out.threads_per_group, source->threads, sizeof(out.threads_per_group));
    return out;
}
static mag_native_shader_stage stage(const mag_native_fixture_shader *source) {
    return (mag_native_shader_stage){source->source, source->length, source->entry,
                                     (uint32_t)strlen(source->entry), MAG_NATIVE_SPIRV};
}
static void check_rejected_batches(const MagRenderHostOwner *owner, const mag_native_pipeline_desc *source) {
    mag_native_device *device = NULL;
    assert(mag_native_create(owner, &device) == 0);
    mag_native_pipeline pipeline = {0};
    assert(mag_native_pipeline_create(device, source, &pipeline) == 0);
    mag_native_buffer buffers[3] = {{0}};
    uint8_t parameters[80] = {0};
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
#define REJECT(expected)                                                                                     \
    do {                                                                                                     \
        mag_native_ticket ticket = {0};                                                                      \
        assert(mag_native_submit_batch(device, &batch, &ticket) == (expected));                              \
        assert(!ticket.submission_id && !ticket.device_generation);                                          \
        mag_native_event event = {0};                                                                        \
        uint32_t count = 0;                                                                                  \
        assert(mag_native_poll(device, &event, 1, &count) == 0 && count == 0);                               \
    } while (0)
    REJECT(MAG_NATIVE_INVALID);
    resources[0].role = MAG_NATIVE_UPLOAD;
    resources[1].role = MAG_NATIVE_UPLOAD;
    REJECT(MAG_NATIVE_INVALID);
    resources[1].role = MAG_NATIVE_PRODUCED;
    batch.command_count = 2;
    REJECT(MAG_NATIVE_INVALID);
    batch.command_count = 1;
    bindings[1].buffer = buffers[0];
    REJECT(MAG_NATIVE_INVALID);
    bindings[1].buffer = buffers[1];
    bindings[0].buffer.device_generation++;
    REJECT(MAG_NATIVE_INVALID);
    bindings[0].buffer = buffers[0];
    resources[0].buffer.version++;
    REJECT(MAG_NATIVE_STALE);
    resources[0].buffer = buffers[0];
    bindings[0].offset = UINT64_MAX;
    REJECT(MAG_NATIVE_INVALID);
    bindings[0].offset = 0;
    frame.pipeline.id = UINT64_MAX;
    REJECT(MAG_NATIVE_STALE);
#undef REJECT
    for (unsigned i = 0; i < 3; ++i)
        assert(mag_native_buffer_release(device, buffers[i]) == 0);
    assert(mag_native_pipeline_release(device, pipeline) == 0);
    assert(mag_native_close(device) == 0);
}
/* In-place upload rewrite (arena reuse) and the memory census, which this
 * backend now has; the reserved-order commit stays UNSUPPORTED (counted
 * synchronous fallback in the worker). */
static void check_buffer_write_and_census(const MagRenderHostOwner *owner) {
    mag_native_device *device = NULL;
    assert(mag_native_create(owner, &device) == 0);
    uint64_t census[MAG_NATIVE_MEMORY_STATS] = {0};
    assert(mag_native_memory_stats(device, census, MAG_NATIVE_MEMORY_STATS) == 0 && census[0] == 0 && census[1] == 0);
    uint8_t seed[64], patch[16], back[64];
    for (unsigned i = 0; i < sizeof seed; ++i) seed[i] = (uint8_t)i;
    for (unsigned i = 0; i < sizeof patch; ++i) patch[i] = (uint8_t)(0xA0 + i);
    mag_native_buffer_desc desc = {sizeof(desc), 1, sizeof seed, seed};
    mag_native_buffer buffer = {0};
    assert(mag_native_buffer_create(device, &desc, &buffer) == 0);
    assert(mag_native_memory_stats(device, census, MAG_NATIVE_MEMORY_STATS) == 0 && census[0] >= sizeof seed && census[1] == 1);
    assert(mag_native_buffer_write(device, buffer, 8, patch, sizeof patch) == 0);
    assert(mag_native_buffer_read(device, buffer, 0, back, sizeof back) == 0);
    for (unsigned i = 0; i < sizeof back; ++i) assert(back[i] == (i >= 8 && i < 24 ? patch[i - 8] : seed[i]));
    assert(mag_native_buffer_write(device, buffer, 56, patch, sizeof patch) == MAG_NATIVE_INVALID);
    assert(mag_native_buffer_write(device, buffer, UINT64_MAX, patch, 1) == MAG_NATIVE_INVALID);
    mag_native_buffer stale = buffer; ++stale.version;
    assert(mag_native_buffer_write(device, stale, 0, patch, 4) == MAG_NATIVE_STALE);
    assert(mag_native_memory_stats(device, census, 3) == MAG_NATIVE_INVALID);
    mag_native_ticket ticket = {0};
    mag_native_scene_import import = {0};
    assert(mag_native_reserve_scene_v5(device, &import, 1, &ticket) == MAG_NATIVE_UNSUPPORTED && !ticket.submission_id);
    assert(mag_native_buffer_release(device, buffer) == 0);
    assert(mag_native_memory_stats(device, census, MAG_NATIVE_MEMORY_STATS) == 0 && census[0] == 0 && census[1] == 0);
    assert(mag_native_close(device) == 0);
}
int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: native_regression owner|compute|draw|draw-no-readback VENDOR DEVICE\n");
        return 2;
    }
    uint32_t vendor = (uint32_t)strtoul(argv[2], NULL, 0), device = (uint32_t)strtoul(argv[3], NULL, 0);
    mag_render_test_owner_require_hardware(vendor, device);
    if (!strcmp(argv[1], "owner"))
        return mag_render_test_owner_contract();
    mag_native_pipeline_desc legacy = compute(&copy_transform_fixture),
                             first = compute(&scene_transform_fixture),
                             second = compute(&scene_prepare_fixture);
    MagRenderHostOwner owner = {0};
    if (mag_render_test_owner_open(&owner) || mag_render_test_owner_gate()) {
        mag_render_test_owner_close(1);
        return 3;
    }
    char name[256];
    snprintf(name, sizeof(name), "%s", mag_render_test_owner_name());
    int result = 1;
    if (!strcmp(argv[1], "compute")) {
        MagRenderNativeProbeReport report = {.abi_version = 1, .struct_size = sizeof(report)};
        int status =
            mag_render_native_probe_owner(&owner, &legacy, mag_render_test_owner_signal, NULL, &report);
        mag_render_test_owner_signal(NULL);
        mag_render_test_owner_close(report.pending == 0);
        printf("{\"device\":\"%s\",\"vendor_id\":%u,\"device_id\":%u,\"fixture_timeline_enabled\":true,"
               "\"status\":%d,\"submitted\":%u,\"completed\":%u,\"pending\":%u,\"retained_before_terminal\":%"
               "llu,\"retained_after_terminal\":%llu,\"upload_bytes\":%llu}\n",
               name, vendor, device, status, report.submitted, report.completed, report.pending,
               (unsigned long long)report.retained_before_terminal,
               (unsigned long long)report.retained_after_terminal, (unsigned long long)report.upload_bytes);
        result = status == 0 && report.submitted == 3 && report.completed == 3 && report.pending == 0 &&
                         report.retained_before_terminal == 352 && report.retained_after_terminal == 0
                     ? 0
                     : 1;
    } else {
        int diagnostic = !strcmp(argv[1], "draw");
        if (!diagnostic && strcmp(argv[1], "draw-no-readback")) {
            mag_render_test_owner_close(1);
            return 2;
        }
        check_rejected_batches(&owner, &legacy);
        check_buffer_write_and_census(&owner);
        mag_native_graphics_desc graphics = {
            sizeof(graphics), 1, stage(&scene_vertex_fixture), stage(&scene_fragment_fixture), 16, 0};
        DrawProbeReport report = {.abi_version = 1, .struct_size = sizeof(report)};
        int status = mag_render_native_draw_probe(&owner, &first, &second, &graphics, (uint32_t)diagnostic,
                                                  mag_render_test_owner_signal, NULL, &report);
        mag_render_test_owner_signal(NULL);
        mag_render_test_owner_close(report.pending == 0);
        printf("{\"device\":\"%s\",\"vendor_id\":%u,\"device_id\":%u,\"fixture_timeline_enabled\":true,"
               "\"status\":%d,\"submitted\":%u,\"completed\":%u,\"pending\":%u,\"retained_before_terminal\":%"
               "llu,\"retained_after_terminal\":%llu,\"center_rgba\":%u,\"outside_rgba\":%u,\"diagnostic_"
               "readback\":%s,\"produced_uniform_consumer\":true,\"native_rejection_cases\":8,\"compute_"
               "dispatches_per_batch\":2,\"indexed_"
               "draws_per_batch\":1}\n",
               name, vendor, device, status, report.submitted, report.completed, report.pending,
               (unsigned long long)report.retained_before_terminal,
               (unsigned long long)report.retained_after_terminal, report.center_rgba, report.outside_rgba,
               diagnostic ? "true" : "false");
        result = status == 0 && report.submitted == 2 && report.completed == 2 && report.pending == 0 &&
                         report.retained_before_terminal == 208 && report.retained_after_terminal == 0 &&
                         (!diagnostic || (report.center_rgba == 0xffff9933u && report.outside_rgba == 0))
                     ? 0
                     : 1;
    }
    return result;
}
