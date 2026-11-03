#include "mag_render_batch.h"
#include "mag_render_native_api.h"
#include "mag_render_imported_image.h"
#include "mag_render_scene.h"
#include "mag_render_stencil.h"
#include <pthread.h>
#include <time.h>
#include <unistd.h>
int mag_render_test_owner_image(MagRenderHostImage *);
uint32_t mag_render_test_owner_pixel(void);
void mag_render_test_owner_clear(const float rgba[4]);
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
/* In-place upload rewrite (arena reuse) and the memory census. */
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
    /* With the host's queue order a reservation validates its imports. */
    assert(mag_native_reserve_scene_v5(device, &import, 1, &ticket) == MAG_NATIVE_STALE && !ticket.submission_id);
    assert(mag_native_buffer_release(device, buffer) == 0);
    assert(mag_native_memory_stats(device, census, MAG_NATIVE_MEMORY_STATS) == 0 && census[0] == 0 && census[1] == 0);
    assert(mag_native_close(device) == 0);
}
/* Reserved submission order on the real queue (as the Metal fixture checks):
 * reservation validates the import under the owner lease, a second
 * reservation or close is BUSY, an abandoned reservation releases its
 * position, and an imported clear-only scene encodes into its reservation,
 * from another thread, with the reserved ticket. Original work recorded
 * after the reservation -- a blue clear of the same target -- must land
 * after the native clear although that is submitted ~100 ms later. */
typedef struct { mag_native_device *device; const mag_native_scene_v5 *v5; mag_native_ticket ticket; int status; } reserved_job;
static void *reserved_encode(void *arg) {
    reserved_job *j = arg;
    struct timespec d = {0, 100 * 1000 * 1000};
    nanosleep(&d, NULL);
    j->status = mag_native_submit_scene_v5(j->device, j->v5, NULL, NULL, &j->ticket);
    return NULL;
}
static void check_reserved_submission(const MagRenderHostOwner *owner, const MagRenderHostImage *target) {
    mag_native_device *device = NULL;
    assert(mag_native_create(owner, &device) == 0);
    mag_native_image_certificate certificate = {*target, NULL};
    mag_native_image ref = {0};
    assert(mag_native_image_import_certified(device, &certificate, &ref) == 0);
    mag_native_scene_import import = {0, 0, ref};
    mag_native_ticket first = {0}, busy = {0}, second = {0};
    assert(mag_native_reserve_scene_v5(device, &import, 1, &first) == 0 && first.device_generation && first.submission_id);
    assert(mag_native_reserve_scene_v5(device, &import, 1, &busy) == MAG_NATIVE_BUSY && !busy.submission_id);
    assert(mag_native_close(device) == MAG_NATIVE_BUSY);
    assert(mag_native_abandon_scene_reservation(device) == 0);
    assert(mag_native_abandon_scene_reservation(device) == MAG_NATIVE_STALE);
    assert(mag_native_reserve_scene_v5(device, &import, 1, &second) == 0 && second.submission_id > first.submission_id);
    mag_native_scene_image image = {MAG_NATIVE_SCENE_RGBA8, target->width, target->height, 0, {0}};
    mag_native_scene_pass pass = {0};
    pass.color.image = 0; pass.color.load = MAG_NATIVE_SCENE_CLEAR; pass.color.store = 1;
    pass.color.clear[0] = 0.25f; pass.color.clear[3] = 1; pass.depth.image = MAG_NATIVE_SCENE_NONE;
    mag_native_scene scene = {0};
    scene.struct_size = sizeof(scene); scene.abi_version = 1;
    scene.images = &image; scene.image_count = 1; scene.passes = &pass; scene.pass_count = 1;
    scene.output_image = MAG_NATIVE_SCENE_NONE;
    mag_native_stencil_attachment stencil = {MAG_NATIVE_SCENE_NONE, 0, 0, 0, 0, 0};
    mag_native_scene_v5 v5 = {0};
    v5.struct_size = sizeof(v5); v5.abi_version = 5;
    v5.base.struct_size = sizeof(v5.base); v5.base.abi_version = 4; v5.base.stencils = &stencil; v5.base.stencil_count = 1;
    v5.base.base.struct_size = sizeof(v5.base.base); v5.base.base.abi_version = 3;
    v5.base.base.base.struct_size = sizeof(v5.base.base.base); v5.base.base.base.abi_version = 2;
    v5.base.base.base.scene = &scene;
    v5.imports = &import; v5.import_count = 1;
    reserved_job job = {device, &v5, {0}, -99};
    pthread_t worker;
    assert(pthread_create(&worker, NULL, reserved_encode, &job) == 0);
    const float blue[4] = {0, 0, 1, 1};
    mag_render_test_owner_clear(blue);                 /* original work after the reservation */
    const uint32_t pixel = mag_render_test_owner_pixel();
    pthread_join(worker, NULL);
    if (job.status) fprintf(stderr, "[reserved submission] encode status=%d\n", job.status);
    assert(job.status == 0);
    assert(job.ticket.device_generation == second.device_generation && job.ticket.submission_id == second.submission_id);
    fprintf(stderr, "[reserved submission] original clear after the reservation reads %08x (bytes R,G,B,A; blue = ffff0000)\n", pixel);
    assert(pixel == 0xffff0000u);                     /* blue: the native 0.25 red clear ran first */
    assert(mag_native_abandon_scene_reservation(device) == MAG_NATIVE_STALE);
    int completed = 0;
    for (int i = 0; i < 2000 && !completed; ++i) {
        mag_native_event event = {0};
        uint32_t count = 0;
        assert(mag_native_poll(device, &event, 1, &count) == 0);
        if (count) { assert(event.ticket.submission_id == second.submission_id && event.succeeded); completed = 1; }
        else usleep(1000);
    }
    assert(completed);
    assert(mag_native_image_release(device, ref) == 0);
    assert(mag_native_close(device) == 0);
    fprintf(stderr, "[reserved submission] reserve=%llu abandoned, busy/close refused, encoded=%llu from another thread, completed\n",
            (unsigned long long)first.submission_id, (unsigned long long)second.submission_id);
}
int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: native_regression owner|compute|draw|draw-no-readback VENDOR DEVICE\n");
        return 2;
    }
    uint32_t vendor = (uint32_t)strtoul(argv[2], NULL, 0), device = (uint32_t)strtoul(argv[3], NULL, 0);
    setenv("MAG_RENDER_VULKAN_RESERVED_COMMIT", "1", 1);   /* opt-in; checked by check_reserved_submission */
    mag_render_test_owner_require_hardware(vendor, device);
    if (!strcmp(argv[1], "owner"))
        return mag_render_test_owner_contract();
    mag_native_pipeline_desc legacy = compute(&copy_transform_fixture),
                             first = compute(&scene_transform_fixture),
                             second = compute(&scene_prepare_fixture);
    MagRenderHostOwner owner = {0};
    if (mag_render_test_owner_open(&owner)) {
        mag_render_test_owner_close(1);
        return 3;
    }
    /* Before the deliberate queue gate below: this check needs completion. */
    if (!strcmp(argv[1], "draw")) {
        MagRenderHostImage target = {.abi_version = 1, .struct_size = sizeof(target)};
        assert(mag_render_test_owner_image(&target) == 0);
        check_reserved_submission(&owner, &target);
    }
    if (mag_render_test_owner_gate()) {
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
