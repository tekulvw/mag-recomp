#include "mag_render_scene.h"
#include "metal_shader.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
extern "C" int mag_render_test_owner_open(MagRenderHostOwner *);
extern "C" int mag_render_test_owner_gate(void);
extern "C" const char *mag_render_test_owner_name(void);
extern "C" void mag_render_test_owner_signal(void *);
extern "C" void mag_render_test_owner_close(int);
struct SceneProbeReport {
    uint32_t abi_version, struct_size;
    int32_t status;
    uint32_t submitted, completed, pending;
    uint64_t retained_before_terminal, retained_after_terminal;
    uint32_t center_rgba, ui_rgba, shadow_rgba, outside_rgba, pass_count, draw_count;
};
extern "C" int mag_render_native_scene_probe(const MagRenderHostOwner *, const mag_native_pipeline_desc *,
                                             const mag_native_pipeline_desc *,
                                             const mag_native_scene_graphics_desc *, uint32_t,
                                             void (*)(void *), void *, SceneProbeReport *);
static std::vector<mag_native_scene_buffer_layout> buffers(Shader &shader, uint32_t stage) {
    std::vector<mag_native_scene_buffer_layout> out;
    for (NSDictionary *b in shader.manifest[@"buffers"]) {
        if ([b[@"group"] unsignedIntValue] != stage || ![b[@"access"] isEqual:@"read"])
            return {};
        out.push_back({stage,
                       0,
                       {[b[@"binding"] unsignedIntValue],
                        MAG_NATIVE_READ,
                        [b[@"minimum_binding_bytes"] unsignedLongLongValue],
                        0,
                        MAG_NATIVE_NO_SIZE_INDEX,
                        [b[@"layout"][@"alignment_bytes"] unsignedIntValue],
                        [b[@"address_space"] isEqual:@"uniform"] ? MAG_NATIVE_UNIFORM : MAG_NATIVE_STORAGE}});
    }
    return out;
}
static void check_rejected_scenes(const MagRenderHostOwner *owner) {
    mag_native_device *device = NULL;
    assert(mag_native_create(owner, &device) == 0);
    mag_native_scene_image image = {MAG_NATIVE_SCENE_D32, 8, 8, 0, {}};
    mag_native_scene_pass pass = {};
    pass.color.image = MAG_NATIVE_SCENE_NONE;
    pass.depth.image = 0;
    pass.depth.load = MAG_NATIVE_SCENE_LOAD;
    pass.depth.clear[0] = 1;
    mag_native_scene scene = {};
    scene.struct_size = sizeof(scene);
    scene.abi_version = 1;
    scene.images = &image;
    scene.image_count = 1;
    scene.passes = &pass;
    scene.pass_count = 1;
    scene.output_image = MAG_NATIVE_SCENE_NONE;
    auto rejected = [&] {
        mag_native_ticket ticket = {};
        assert(mag_native_submit_scene(device, &scene, &ticket) == MAG_NATIVE_INVALID);
        assert(!ticket.device_generation && !ticket.submission_id);
        mag_native_event event = {};
        uint32_t count = 0;
        assert(mag_native_poll(device, &event, 1, &count) == 0 && !count);
    };
    rejected(); // Load before any clear/store.
    pass.depth.load = MAG_NATIVE_SCENE_CLEAR;
    pass.draw_count = 1;
    rejected(); // Missing draw list.
    pass.draw_count = 0;
    image.width = UINT32_MAX;
    rejected();
    image.width = 8;
    scene.output_image = 0;
    rejected(); // Depth cannot be a color output.
    scene.output_image = MAG_NATIVE_SCENE_NONE;
    pass.depth.clear[0] = NAN;
    rejected();
    pass.depth.clear[0] = 1;
    scene.pass_count = 0;
    rejected();
    scene.pass_count = 1;
    image.format = MAG_NATIVE_SCENE_RGBA8;
    rejected(); // Explicit attachment format.
    assert(mag_native_close(device) == 0);
}
int mag_render_scene_main(const char *directory, bool diagnostic) {
    @autoreleasepool {
        NSString *root = [NSString stringWithUTF8String:directory];
        Shader first = {}, second = {}, vertex = {}, fragment = {}, cutout = {};
        if (!first.load(root, @"scene_transform", true) || !second.load(root, @"scene_prepare", true) ||
            !vertex.load(root, @"scene_lit_vertex", false) ||
            !fragment.load(root, @"scene_lit_fragment", false) ||
            !cutout.load(root, @"scene_cutout_fragment", false))
            return 2;
        auto vertex_buffers = buffers(vertex, 0);
        auto fragment_buffers = buffers(fragment, 1);
        if (vertex_buffers.size() != 1 || fragment_buffers.size() != 1)
            return 2;
        if ([vertex.manifest[@"varyings"][0][@"format"] isEqual:@"float32x2"] != YES ||
            [fragment.manifest[@"images"] count] != 3 || [fragment.manifest[@"samplers"] count] != 2)
            return 2;
        std::vector<mag_native_scene_buffer_layout> both = vertex_buffers;
        both.insert(both.end(), fragment_buffers.begin(), fragment_buffers.end());
        std::vector<mag_native_scene_texture_layout> textures;
        for (NSDictionary *t in fragment.manifest[@"images"]) {
            uint32_t slot = [t[@"binding"] unsignedIntValue];
            bool depth = [t[@"sample_kind"] isEqual:@"depth"];
            if ([t[@"group"] unsignedIntValue] != 1 || (!depth && slot != 1) ||
                (depth && slot != 3 && slot != 4))
                return 2;
            textures.push_back({1, slot, depth ? 5u : 2u,
                                depth ? MAG_NATIVE_SCENE_D32 : MAG_NATIVE_SCENE_RGBA8, depth ? 1u : 0u, 0});
        }
        for (NSDictionary *s in fragment.manifest[@"samplers"]) {
            uint32_t slot = [s[@"binding"] unsignedIntValue];
            if ([s[@"group"] unsignedIntValue] != 1 || (slot != 2 && slot != 5) ||
                [s[@"comparison"] boolValue] != (slot == 5))
                return 2;
        }
        mag_native_scene_graphics_desc graphics[5] = {};
        for (auto &g : graphics) {
            g.struct_size = sizeof(g);
            g.abi_version = 1;
            g.vertex = vertex.stage();
            g.fragment = fragment.stage();
            g.vertex_stride = 16;
            g.color_format = MAG_NATIVE_SCENE_RGBA8;
            g.depth_format = MAG_NATIVE_SCENE_D32;
            g.depth_compare = MAG_NATIVE_SCENE_DEPTH_LESS;
            g.depth_write = 1;
            g.buffers = both.data();
            g.buffer_count = uint32_t(both.size());
            g.textures = textures.data();
            g.texture_count = uint32_t(textures.size());
        }
        graphics[0].color_format = 0;
        graphics[0].fragment = {};
        graphics[0].buffers = vertex_buffers.data();
        graphics[0].buffer_count = uint32_t(vertex_buffers.size());
        graphics[0].textures = NULL;
        graphics[0].texture_count = 0;
        graphics[2].depth_write = 0;
        graphics[2].blend = MAG_NATIVE_SCENE_ALPHA;
        graphics[3].depth_compare = MAG_NATIVE_SCENE_DEPTH_DISABLED;
        graphics[3].depth_write = 0;
        graphics[3].blend = MAG_NATIVE_SCENE_ALPHA;
        mag_native_scene_texture_layout cutout_texture = {1, 1, 2, MAG_NATIVE_SCENE_RGBA8, 0, 0};
        graphics[4] = graphics[0];
        graphics[4].fragment = cutout.stage();
        graphics[4].textures = &cutout_texture;
        graphics[4].texture_count = 1;
        MagRenderHostOwner owner = {};
        if (mag_render_test_owner_open(&owner) || mag_render_test_owner_gate()) {
            mag_render_test_owner_close(1);
            return 3;
        }
        check_rejected_scenes(&owner);
        NSString *name = [NSString stringWithUTF8String:mag_render_test_owner_name()];
        SceneProbeReport report = {};
        report.abi_version = 1;
        report.struct_size = sizeof(report);
        int status =
            mag_render_native_scene_probe(&owner, &first.compute, &second.compute, graphics,
                                          diagnostic ? 1 : 0, mag_render_test_owner_signal, NULL, &report);
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
            @"ui_rgba" : @(report.ui_rgba),
            @"shadow_rgba" : @(report.shadow_rgba),
            @"outside_rgba" : @(report.outside_rgba),
            @"passes" : @(report.pass_count),
            @"draws" : @(report.draw_count),
            @"diagnostic_readback" : @(diagnostic),
            @"physical_presentation" : @NO,
            @"native_rejection_cases" : @7
        };
        NSData *encoded = [NSJSONSerialization dataWithJSONObject:result
                                                          options:NSJSONWritingSortedKeys
                                                            error:NULL];
        printf("%s\n", [[[NSString alloc] initWithData:encoded encoding:NSUTF8StringEncoding] UTF8String]);
        return status == 0 && report.submitted == 2 && report.completed == 2 && !report.pending &&
                       report.retained_before_terminal == 608 && !report.retained_after_terminal
                   ? 0
                   : 1;
    }
}
