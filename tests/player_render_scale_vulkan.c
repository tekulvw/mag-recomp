/* Synthetic headless GPU regression for the rendering-resolution adapter on
 * the Vulkan backend. No game programs, assets or window. Build it as
 * tests/test_player_render_scale_vulkan.py does: this file includes the SDK's
 * rsx_vulkan_backend.c with its draw engine registration renamed to the
 * adapter's (as cmake/MagRenderScale.cmake renames it), and links
 * player_render_scale.cpp built with MAG_PLAYER_RENDER_SCALE_VULKAN=1.
 * Run once per supported percentage: 100, 150, 200 and 300.
 *
 * It checks what PR #245 learned on Metal, on Vulkan: the physical target
 * sizes and added raster detail; fragment window positions in guest units;
 * guest-unit writes, scissors and colour/depth readbacks; a 1x1 target at an
 * odd physical size drawn and read whole; a depth target read as A8R8G8B8
 * words through a view on the physical grid; and the present reaching a
 * display image of the enlarged size pixel for pixel. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "rsx_vulkan_backend.c"
#include "player_render_scale.h"
#include <assert.h>
#include <math.h>

u8* vm_base;u32 ppu_vm_size,ppu_hle_inject_base;
u32 cellGcmResolveLocated(int local,u32 offset){(void)local;(void)offset;assert(!"fixture has no guest memory");return 0;}
u32 cellGcmResolveIO(u32 offset){(void)offset;assert(!"fixture has no guest IO");return 0;}
u32 gcm_io_offset_to_ea(u32 offset){(void)offset;assert(!"fixture has no guest IO");return 0;}
void vm_write32(uint32_t a,uint32_t v){(void)a;(void)v;assert(!"fixture cannot publish guest memory");}
void ps3_ms(const char* key){(void)key;}
unsigned long long ps3_ms_now(void){return 0;}
void cellGcm_replay_display_buffers_to_engine(void){}
u32 cellGcm_flip_request_count(void){return 0;}
void cellGcm_fifo_kick_notify(void){}
rsx_frontend_device* rsx_frontend_vulkan_create(const rsx_frontend_vulkan_owner* owner){(void)owner;assert(!"legacy frontend must not be created");return NULL;}
void rsx_frontend_close(rsx_frontend_device* device){assert(!device);}

static int checks, failures;
#define CHECK(value, label) do { checks++; if (!(value)) { \
    failures++; fprintf(stderr, "FAIL %s\n", label); } } while (0)

static const char* vertex_shader =
    "struct Input { float4 position : ATTR0; };\n"
    "float4 main(Input v) : SV_Position { return v.position; }\n";
static const char* white_shader =
    "float4 main() : SV_Target0 { return float4(1,1,1,1); }\n";
static const char* red_shader =
    "float4 main() : SV_Target0 { return float4(1,0,0,1); }\n";
static const char* position_shader =
    "struct PSInput { float4 position : SV_Position; };\n"
    "float4 main(PSInput input) : SV_Target0 {\n"
    "return float4(input.position.x/4.0,input.position.y/4.0,0,1); }\n";

static const float fullscreen[3][4] = {{-1,-1,0.25f,1}, {3,-1,0.25f,1}, {-1,3,0.25f,1}};

static u32 pipeline(const rsx_draw_backend* b, const char* ps, int depth_write)
{
    rsx_vertex_layout_plan layout; memset(&layout, 0, sizeof layout);
    layout.mask = 1; layout.count = 1; layout.stride = 16;
    rsx_be_render_state state; memset(&state, 0, sizeof state);
    state.color_mask = 0xffffffffu;
    state.depth_test = 1; state.depth_write = (u32)depth_write; state.depth_func = 0x207;  /* ALWAYS */
    const u32 p = b->pipeline_create(b->user, vertex_shader, ps, &state, &layout, 16, RSX_BE_FMT_R8G8B8A8, 1);
    CHECK(p != 0, "translated synthetic pipeline");
    return p;
}

static void draw(const rsx_draw_backend* b, u32 pipe, u32 color, u32 depth, const float (*vertices)[4],
                 u32 vx, u32 vy, u32 vw, u32 vh, u32 sx, u32 sy, u32 sw, u32 sh)
{
    b->bind_targets(b->user, &color, 1, depth);
    b->bind_pipeline(b->user, pipe);
    b->set_viewport(b->user, (float)vx, (float)vy, (float)vw, (float)vh);
    b->set_scissor(b->user, sx, sy, sw, sh);
    b->draw(b->user, RSX_TOPOLOGY_TRIANGLES, vertices, 3, 16, NULL, 0);
    b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* The physical pixels of an engine image, after everything submitted. */
static void physical_rgba(u32 handle, u8* out)
{
    const vk_eng_obj* o = &s_eobj[handle];
    if (s_bat_on) (void)vk_bat_flush(1);
    vk_device_idle();
    vk_buffer st = {0};
    assert(vk_create_host_buffer(&st, (VkDeviceSize)o->w * o->h * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0);
    assert(vk_begin() == 0);
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { o->w, o->h, 1 } };
    pvkCmdCopyImageToBuffer(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &region);
    VkBufferMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &hb, 0, NULL);
    assert(vk_sync() == 0);
    vk_invalidate(&st);
    memcpy(out, st.ptr, (size_t)o->w * o->h * 4u);
    vk_destroy_buffer_now(&st);
}

/* The display image the window is blitted from, after the presents so far. */
static void display_rgba(u8* out)
{
    vk_present_wait_gpu();
    vk_device_idle();
    vk_buffer st = {0};
    assert(vk_create_host_buffer(&st, (VkDeviceSize)s_vk.out_w * s_vk.out_h * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0);
    assert(vk_begin() == 0);
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { s_vk.out_w, s_vk.out_h, 1 } };
    pvkCmdCopyImageToBuffer(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &region);
    VkBufferMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &hb, 0, NULL);
    assert(vk_sync() == 0);
    vk_invalidate(&st);
    memcpy(out, st.ptr, (size_t)s_vk.out_w * s_vk.out_h * 4u);
    vk_destroy_buffer_now(&st);
}

static int render_triangle(const rsx_draw_backend* b, u32 color, u32 depth, u8* pixels)
{
    const u32 p = pipeline(b, white_shader, 1);
    if (!p) return 0;
    /* The sloping edge crosses subpixels hidden by a nearest-scaled 4x4. */
    const float triangle[3][4] = {{-0.91f,-0.87f,0.25f,1}, {0.79f,-0.87f,0.25f,1}, {-0.91f,0.83f,0.25f,1}};
    const float black[4] = {0, 0, 0, 1};
    b->clear_color(b->user, color, black);
    b->clear_depth_stencil(b->user, depth, 3, 1, 0);
    draw(b, p, color, depth, triangle, 0, 0, 4, 4, 0, 0, 4, 4);
    physical_rgba(color, pixels);
    b->pipeline_release(b->user, p);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc != 2) { fprintf(stderr, "usage: test percentage\n"); return 2; }
    const unsigned percent = (unsigned)strtoul(argv[1], NULL, 10);
    const u32 extent = percent == 100 ? 4 : percent == 150 ? 6 : percent == 200 ? 8 : percent == 300 ? 12 : 0;
    if (!extent) return 2;
    setenv("MAG_PLAYER_RENDER_SCALE", argv[1], 1);
    setenv("PS3RECOMP_VK_WINDOW", "0", 1);
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS", "1", 1);
    setenv("PS3RECOMP_RSX_ENGINE", "dispatch", 1);
    /* A 4x4 display: the backend registers its engine table through the
     * adapter during this init, which also asks for the enlarged present. */
    if (rsx_vulkan_backend_init(4, 4, "render scale synthetic")) { fprintf(stderr, "no Vulkan device\n"); return 1; }
    CHECK(s_vk.eng_active && s_g.on, "register-file draw engine with translated programs");
    CHECK(s_vk.out_w == extent && s_vk.out_h == extent && s_vk.color.w == extent && s_vk.color.h == extent,
          "the display image takes the rendering resolution");

    /* Native reference, through the backend's own table. */
    const rsx_draw_backend* native = &s_vk_engine_backend;
    u32 native_color = native->color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 4, 4, NULL, 0);
    u32 native_depth = native->depth_target_create(NULL, 4, 4);
    u8 native_pixels[64];
    if (!render_triangle(native, native_color, native_depth, native_pixels)) return 1;
    native->color_target_release(NULL, native_color);
    native->depth_target_release(NULL, native_depth);

    const rsx_draw_backend* b = mag_player_render_backend(&s_vk_engine_backend);
    CHECK(b != NULL && (b == &s_vk_engine_backend) == (percent == 100), "native scale keeps the backend itself");
    if (!b) return 1;
    CHECK(percent == 100 || (!b->depth_store_z24 && !b->depth_z24_texture),
          "the guest-memory packed-depth shortcut is off while scaled");
    u32 color = b->color_target_create(b->user, RSX_BE_FMT_R8G8B8A8, 4, 4, NULL, 0);
    u32 depth = b->depth_target_create(b->user, 4, 4);
    const int physical = s_eobj[color].w == extent && s_eobj[color].h == extent &&
                         s_eobj[depth].w == extent && s_eobj[depth].h == extent;
    CHECK(physical, "color and depth GPU targets at the physical size");
    if (!physical) return 1;

    u8 pixels[12 * 12 * 4];
    if (!render_triangle(b, color, depth, pixels)) return 1;
    unsigned lit = 0, detail = 0;
    for (u32 y = 0; y < extent; y++) for (u32 x = 0; x < extent; x++) {
        lit += pixels[(y * extent + x) * 4] > 127;
        const u32 source = ((y * 4 / extent) * 4 + x * 4 / extent) * 4;
        detail += memcmp(pixels + (y * extent + x) * 4, native_pixels + source, 4) != 0;
    }
    CHECK(lit > 0 && lit < extent * extent, "triangle covers a proper subset of the target");
    CHECK(percent == 100 ? detail == 0 : detail > 0,
          "higher resolution rasterizes diagonal detail absent from the nearest-scaled native image");

    /* The present: the enlarged target reaches the display image as it is. */
    b->present(b->user, color);
    u8 shown[12 * 12 * 4];
    display_rgba(shown);
    CHECK(!memcmp(shown, pixels, (size_t)extent * extent * 4u), "the present carries every physical pixel to the display image");

    /* Fragment window positions stay in guest units at each added sample. */
    {
        const u32 p = pipeline(b, position_shader, 0);
        if (p) {
            draw(b, p, color, depth, fullscreen, 0, 0, 4, 4, 0, 0, 4, 4);
            physical_rgba(color, pixels);
            int good = 1;
            for (u32 y = 0; y < extent; y++) for (u32 x = 0; x < extent; x++) {
                const u8* px = pixels + (y * extent + x) * 4;
                good &= fabsf(px[0] - (x + .5f) * 255 / extent) <= 1.5f;
                good &= fabsf(px[1] - (y + .5f) * 255 / extent) <= 1.5f;
            }
            CHECK(good, "fragment window coordinates keep logical fractional sample positions");
            b->pipeline_release(b->user, p);
        }
    }

    /* Guest-unit writes, scissors and readbacks, colour and depth. */
    {
        const float blue[4] = {0, 0, 1, 1};
        b->clear_color(b->user, color, blue);
        b->clear_depth_stencil(b->user, depth, 3, .75f, 0);
        const u8 red_rows[2][4] = {{255, 0, 0, 255}, {255, 0, 0, 255}};
        b->color_target_write(b->user, color, 1, 1, 1, 2, red_rows, 4);
        const u32 p = pipeline(b, red_shader, 1);
        if (p) draw(b, p, color, depth, fullscreen, 0, 0, 4, 4, 1, 1, 1, 2);   /* depth .25 under the write */
        u8 rows[24]; memset(rows, 0x39, sizeof rows);
        b->readback(b->user, color, 1, 1, 2, 2, rows, 12);
        int good = 1;
        for (u32 row = 0; row < 2; row++) {
            const u8 expected[8] = {255, 0, 0, 255, 0, 0, 255, 255};
            good &= memcmp(rows + row * 12, expected, 8) == 0;
            for (u32 pad = 8; pad < 12; pad++) good &= rows[row * 12 + pad] == 0x39;
        }
        CHECK(good, "logical colour write and ROI readback with padded rows");
        float z[6] = {-9, -9, -9, -9, -9, -9};
        CHECK(b->depth_readback(b->user, depth, 1, 1, 2, 2, z, 12) == 0, "logical depth ROI succeeds");
        CHECK(fabsf(z[0] - .25f) < 1e-6 && fabsf(z[1] - .75f) < 1e-6 && fabsf(z[3] - .25f) < 1e-6 &&
              fabsf(z[4] - .75f) < 1e-6 && z[2] == -9 && z[5] == -9,
              "the scissored depth write lands in its guest rectangle; padding untouched");
        if (p) b->pipeline_release(b->user, p);
    }

    /* A 1x1 target (an exposure chain's last step) at an odd physical size:
     * the whole physical image is drawn and a guest read finds it. */
    {
        u32 one = b->color_target_create(b->user, RSX_BE_FMT_R8G8B8A8, 1, 1, NULL, 0);
        u32 one_depth = b->depth_target_create(b->user, 1, 1);
        const u32 side = (percent + 50) / 100;
        CHECK(s_eobj[one].w == side && s_eobj[one].h == side, "a 1x1 target rounds to whole physical pixels");
        const float black[4] = {0, 0, 0, 1};
        b->clear_color(b->user, one, black);
        const u32 p = pipeline(b, white_shader, 0);
        if (p) {
            draw(b, p, one, one_depth, fullscreen, 0, 0, 1, 1, 0, 0, 1, 1);
            u8 texels[3 * 3 * 4];
            physical_rgba(one, texels);
            int all = 1;
            for (u32 i = 0; i < side * side; i++) all &= texels[i * 4] == 255;
            CHECK(all, "every physical texel of the 1x1 target is drawn");
            u8 texel[4] = {0};
            b->readback(b->user, one, 0, 0, 1, 1, texel, 4);
            CHECK(texel[0] == 255 && texel[1] == 255, "the guest reads the 1x1 target's drawn value");
            b->pipeline_release(b->user, p);
        }
        b->color_target_release(b->user, one);
        b->depth_target_release(b->user, one_depth);
    }

    /* A depth target read as A8R8G8B8 words (a shadow map sampled as
     * colour): the view of its top-left guest rectangle is on the physical grid. */
    if (b->depth_z24_view) {
        const u32 view = b->depth_z24_view(b->user, depth, 4, 4, 0xE4, 0x85);
        CHECK(view != 0 && s_eobj[view].w == extent && s_eobj[view].h == extent,
              "the depth-as-colour view covers the physical image");
        if (view) { vk_device_idle(); b->texture_release(b->user, view); }
        const u32 part = b->depth_z24_view(b->user, depth, 2, 3, 0xE4, 0x85);
        CHECK(part != 0 && s_eobj[part].w == (2u * percent + 50) / 100 && s_eobj[part].h == (3u * percent + 50) / 100,
              "a partial depth-as-colour view keeps its guest rectangle on the physical grid");
        if (part) { vk_device_idle(); b->texture_release(b->user, part); }
    }

    b->color_target_release(b->user, color);
    b->depth_target_release(b->user, depth);
    rsx_vulkan_backend_shutdown();
    printf("render scale %u%%: physical %ux%u, lit %u, new-detail %u; %d checks, %d failures\n",
           percent, extent, extent, lit, detail, checks, failures);
    return failures ? 1 : 0;
}
