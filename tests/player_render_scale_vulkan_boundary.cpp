// The adapter as the Vulkan backend registers it (player_render_scale.cpp built with
// MAG_PLAYER_RENDER_SCALE_VULKAN=1): the registration asks the backend for the enlarged present
// before it hands the engine the wrapped table, never at 100% or on unregistration, and a refused
// present stops visibly. The fake backend has the Vulkan engine table's shape (no partial clears,
// no depth_as_color; depth_z24_view, pipeline_create_host, the packed-depth shortcut), and the
// scaling decisions PR #245 made on Metal hold for it: viewport and scissor by the bound target's
// own ratio (a 1x1 exposure target at 150%), depth-only passes on their depth target's grid, and
// depth views on the physical grid. Argument: 100, 150, 200, 300 or "refused".
#include "libs/video/rsx_draw_engine.h"
#include "player_render_scale.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const rsx_draw_backend *registered = nullptr;
static int registrations = 0, scale_requests = 0, scale_before_registration = -1;
static u32 requested_percent = 0;
static bool refuse = false;
extern "C" void rsx_draw_engine_set_backend(const rsx_draw_backend *backend) { registered = backend; registrations++; }
extern "C" int rsx_vulkan_backend_set_present_scale(u32 percent) {
    scale_requests++;
    requested_percent = percent;
    scale_before_registration = registrations;
    return refuse ? -1 : 0;
}

static u32 next_handle = 1, last_w, last_h;
static float viewport[4];
static u32 rect[4];
static u32 color(void *, rsx_be_format, u32 w, u32 h, const void *, u32) { last_w = w; last_h = h; return next_handle++; }
static u32 depth(void *, u32 w, u32 h) { last_w = w; last_h = h; return next_handle++; }
static void release(void *, u32) {}
static void target_write(void *, u32, u32 x, u32 y, u32 w, u32 h, const void *, u32) { rect[0] = x; rect[1] = y; rect[2] = w; rect[3] = h; }
static u32 view(void *, u32, u32, u32) { return next_handle++; }
static u32 snap(void *, u32) { return next_handle++; }
static u32 dsnap(void *, u32, u32 w, u32 h) { last_w = w; last_h = h; return next_handle++; }
static u32 pipeline(void *, const char *, const char *, const rsx_be_render_state *, const rsx_vertex_layout_plan *, u32, rsx_be_format, u32) { return 1; }
static u32 host_pipeline(void *, const char *, const char *, const rsx_be_render_state *, const rsx_vertex_layout_plan *, const u8 *, rsx_be_format, u32) { return 1; }
static void bind(void *, const u32 *, u32, u32) {}
static void vp(void *, float x, float y, float w, float h) { viewport[0] = x; viewport[1] = y; viewport[2] = w; viewport[3] = h; }
static void sc(void *, u32 x, u32 y, u32 w, u32 h) { rect[0] = x; rect[1] = y; rect[2] = w; rect[3] = h; }
static void target_read(void *, u32, u32, u32, u32, u32, void *, u32) {}
static int dread(void *, u32, u32, u32, u32, u32, float *, u32) { return 0; }
static int store(void *, u32, u32, u32, u8 *, u32) { return 0; }
static u32 ztex(void *, u32, u32, u32, u32) { return next_handle++; }
static u32 zview(void *, u32, u32 w, u32 h, u32, u32) { last_w = w; last_h = h; return next_handle++; }

int main(int argc, char **argv) {
    assert(argc == 2);
    refuse = !std::strcmp(argv[1], "refused");
    setenv("MAG_PLAYER_RENDER_SCALE", refuse ? "200" : argv[1], 1);
    const u32 percent = (u32)std::atoi(refuse ? "200" : argv[1]);
    // The callbacks the Vulkan engine table sets among those the adapter can see.
    rsx_draw_backend base = {};
    base.color_target_create = color; base.color_target_release = release; base.color_target_write = target_write;
    base.depth_target_create = depth; base.depth_target_release = release; base.texture_release = release;
    base.surface_view = view; base.color_snapshot = snap; base.depth_snapshot = dsnap;
    base.pipeline_create = pipeline; base.pipeline_create_host = host_pipeline; base.bind_targets = bind;
    base.set_viewport = vp; base.set_scissor = sc; base.readback = target_read; base.depth_readback = dread;
    base.depth_store_z24 = store; base.depth_z24_texture = ztex; base.depth_z24_view = zview;

    mag_player_set_render_backend(&base);
    assert(registrations == 1);
    if (percent == 100) {
        assert(registered == &base && scale_requests == 0);
        mag_player_set_render_backend(nullptr);
        assert(registered == nullptr && scale_requests == 0);
        std::puts("native Vulkan registration keeps the backend and asks for no present scale");
        return 0;
    }
    // A refused present stops before registration (the process exits in fail()).
    assert(!refuse);
    assert(scale_requests == 1 && requested_percent == percent && scale_before_registration == 0);
    const rsx_draw_backend *b = registered;
    assert(b && b != &base);
    assert(!b->depth_store_z24 && !b->depth_z24_texture);
    assert(!b->clear_color_rect && !b->clear_depth_stencil_rect && !b->depth_as_color);
    assert(b->depth_z24_view != base.depth_z24_view && b->pipeline_create_host != base.pipeline_create_host);
    assert(b->color_target_write != base.color_target_write && b->surface_view != base.surface_view);

    const u32 c = b->color_target_create(nullptr, RSX_BE_FMT_R8G8B8A8, 1280, 720, nullptr, 0);
    const u32 cw = last_w, ch = last_h;
    assert(cw == 1280u * percent / 100 && ch == 720u * percent / 100);
    const u32 d = b->depth_target_create(nullptr, 1280, 720);
    assert(last_w == cw && last_h == ch);
    const u32 targets[1] = {c};
    b->bind_targets(nullptr, targets, 1, d);
    b->set_viewport(nullptr, 0, 0, 1280, 720);
    assert(viewport[2] == float(cw) && viewport[3] == float(ch));
    // A 1x1 exposure target: at 150% it is 2x2, and the draw covers all of it.
    const u32 one = b->color_target_create(nullptr, RSX_BE_FMT_R8G8B8A8, 1, 1, nullptr, 0);
    const u32 side = last_w;
    assert(side == (percent + 50) / 100 && last_h == side);
    const u32 exposure[1] = {one};
    b->bind_targets(nullptr, exposure, 1, 0);
    b->set_viewport(nullptr, 0, 0, 1, 1);
    assert(viewport[0] == 0 && viewport[2] == float(side) && viewport[3] == float(side));
    b->set_scissor(nullptr, 0, 0, 1, 1);
    assert(rect[0] == 0 && rect[2] == side && rect[3] == side);
    // A guest 2D transfer into it is written over its whole physical image.
    const u8 texel[4] = {1, 2, 3, 4};
    b->color_target_write(nullptr, one, 0, 0, 1, 1, texel, 4);
    assert(rect[2] == side && rect[3] == side);
    // A shadow atlas drawn depth-only takes its own grid.
    const u32 atlas = b->depth_target_create(nullptr, 2048, 1024);
    const u32 aw = last_w, ah = last_h;
    b->bind_targets(nullptr, nullptr, 0, atlas);
    b->set_viewport(nullptr, 0, 0, 2048, 1024);
    assert(viewport[2] == float(aw) && viewport[3] == float(ah));
    // The scene depth read as A8R8G8B8 words: the view is built on the physical grid.
    b->depth_z24_view(nullptr, d, 1280, 720, 0xE4, 0x85);
    assert(last_w == cw && last_h == ch);
    b->depth_z24_view(nullptr, atlas, 1024, 1024, 0xE4, 0x85);
    assert(last_w == aw / 2 && last_h == ah);
    b->depth_snapshot(nullptr, d, 1280, 720);
    assert(last_w == cw && last_h == ch);
    // Unregistration at shutdown passes through and asks for nothing more.
    mag_player_set_render_backend(nullptr);
    assert(registered == nullptr && scale_requests == 1);
    std::printf("Vulkan registration at %u%%: present scale requested first; targets %ux%u, exposure %ux%u\n",
                percent, cw, ch, side, side);
    return 0;
}
