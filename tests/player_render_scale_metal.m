/* Synthetic headless GPU regression. No game programs, assets or window.
 * Compile with the SDK include and libs/video directories on the include path,
 * plus player_render_scale.cpp and the SDK's shader translation dependencies.
 * Run once per supported percentage: 100, 150, 200 and 300.
 * Removing physical target/viewport scaling must fail the dimension and
 * diagonal-detail checks; breaking logical coordinates must fail ROI checks. */
#include "rsx_metal_backend.m"
#include "player_render_scale.h"
#include <math.h>

u8* vm_base;
u32 ppu_vm_size;
int g_resv_store_active;
/* Standalone linkage only: this fixture has no guest CPU or guest writes. */
void ppu_resv_break_store(uint64_t address) { (void)address; abort(); }
void ps3_ww_report_inline(uint32_t address, uint64_t value, int width)
{ (void)address; (void)value; (void)width; abort(); }
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; return offset; }
static int checks, failures;
#define CHECK(value, label) do { checks++; if (!(value)) { \
    failures++; fprintf(stderr, "FAIL %s\n", label); } } while (0)

static const char* vertex_shader =
    "struct Input { float4 position : ATTR0; };"
    "float4 main(Input v) : SV_Position { return v.position; }";
static const char* fragment_shader =
    "float4 main() : SV_Target0 { return float4(1,1,1,1); }";
static const char* position_fragment_shader =
    "struct PSInput { float4 position : SV_Position; };"
    "float4 main(PSInput input) : SV_Target0 {"
    "return float4(input.position.x/4.0,input.position.y/4.0,0,1); }";

static int render_triangle(const rsx_draw_backend* backend, u32 color, u32 depth)
{
    rsx_vertex_layout_plan layout = {0};
    layout.mask = 1; layout.count = 1; layout.stride = 16;
    rsx_be_render_state state = {0};
    state.color_mask = 0xffffffffu;
    state.depth_test = 1; state.depth_write = 1; state.depth_func = 0x207;
    u32 pipeline = backend->pipeline_create(backend->user, vertex_shader,
        fragment_shader, &state, &layout, 16, RSX_BE_FMT_R8G8B8A8, 1);
    CHECK(pipeline != 0, "real translated synthetic triangle pipeline");
    if (!pipeline) return 0;
    const float black[4] = {0, 0, 0, 1};
    /* The sloping edge crosses subpixels hidden by a nearest-scaled 4x4. */
    const float triangle[3][4] = {
        {-0.91f, -0.87f, 0.25f, 1}, {0.79f, -0.87f, 0.25f, 1},
        {-0.91f, 0.83f, 0.25f, 1}
    };
    backend->clear_color(backend->user, color, black);
    backend->clear_depth_stencil(backend->user, depth, 3, 1, 0);
    backend->bind_targets(backend->user, &color, 1, depth);
    backend->bind_pipeline(backend->user, pipeline);
    backend->set_viewport(backend->user, 0, 0, 4, 4);
    backend->set_scissor(backend->user, 0, 0, 4, 4);
    backend->draw(backend->user, RSX_TOPOLOGY_TRIANGLES, triangle, 3, 16, NULL, 0);
    backend->submit_and_wait(backend->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    CHECK(s_eng_checked_completed && !atomic_load(&s_eng_checked_failed),
          "GPU triangle submission completed successfully");
    backend->pipeline_release(backend->user, pipeline);
    return 1;
}

static void copy_physical_rgba(u32 handle, u8* pixels, u32 extent)
{
    id<MTLTexture> texture = eng_obj(handle);
    CHECK(eng_prepare_cpu_read(texture, 1), "physical GPU texture ready for reading");
    [texture getBytes:pixels bytesPerRow:extent * 4
          fromRegion:MTLRegionMake2D(0, 0, extent, extent) mipmapLevel:0];
}

static void fragment_coordinates(const rsx_draw_backend* backend, u32 color,
                                 u32 depth, u32 extent)
{
    rsx_vertex_layout_plan layout = {0};
    layout.mask = 1; layout.count = 1; layout.stride = 16;
    rsx_be_render_state state = {0};
    state.color_mask = 0xffffffffu;
    u32 pipeline = backend->pipeline_create(backend->user, vertex_shader,
        position_fragment_shader, &state, &layout, 16, RSX_BE_FMT_R8G8B8A8, 1);
    CHECK(pipeline != 0, "fragment-window-coordinate pipeline");
    if (!pipeline) return;
    const float fullscreen[3][4] = {{-1,-1,0,1}, {3,-1,0,1}, {-1,3,0,1}};
    backend->bind_targets(backend->user, &color, 1, depth);
    backend->bind_pipeline(backend->user, pipeline);
    backend->set_viewport(backend->user, 0, 0, 4, 4);
    backend->set_scissor(backend->user, 0, 0, 4, 4);
    backend->draw(backend->user, RSX_TOPOLOGY_TRIANGLES, fullscreen, 3, 16, NULL, 0);
    backend->submit_and_wait(backend->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    u8 pixels[12 * 12 * 4]; copy_physical_rgba(color, pixels, extent);
    int good = 1;
    for (u32 y = 0; y < extent; y++) for (u32 x = 0; x < extent; x++) {
        const u8* pixel = pixels + (y * extent + x) * 4;
        /* SV_Position remains in logical guest coordinates at each added
         * sample, rather than reporting the larger physical target size. */
        good &= fabsf(pixel[0] - (x + .5f) * 255 / extent) <= 1;
        good &= fabsf(pixel[1] - (y + .5f) * 255 / extent) <= 1;
    }
    CHECK(good, "fragment window coordinates preserve logical fractional sample positions");
    backend->pipeline_release(backend->user, pipeline);
}

static void logical_readback(const rsx_draw_backend* backend, u32 color, u32 depth)
{
    const float blue[4] = {0, 0, 1, 1}, red[4] = {1, 0, 0, 1};
    backend->clear_color(backend->user, color, blue);
    backend->clear_color_rect(backend->user, color, red, 1, 1, 1, 2);
    backend->clear_depth_stencil(backend->user, depth, 3, .75f, 0);
    backend->clear_depth_stencil_rect(backend->user, depth, RSX_BE_CLEAR_DEPTH,
                                     .25f, 0, 1, 1, 1, 2);
    backend->submit_and_wait(backend->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    /* Two active RGBA pixels and one untouched padding pixel per row. */
    u8 rows[24]; memset(rows, 0x39, sizeof rows);
    backend->readback(backend->user, color, 1, 1, 2, 2, rows, 12);
    int good = 1;
    for (u32 row = 0; row < 2; row++) {
        const u8 expected[8] = {255, 0, 0, 255, 0, 0, 255, 255};
        good &= memcmp(rows + row * 12, expected, 8) == 0;
        for (u32 pad = 8; pad < 12; pad++) good &= rows[row * 12 + pad] == 0x39;
    }
    CHECK(good, "logical offset color ROI and padded output rows");
    float z[6] = {-9, -9, -9, -9, -9, -9};
    CHECK(backend->depth_readback(backend->user, depth, 1, 1, 2, 2, z, 12) == 0,
          "logical depth ROI succeeds");
    CHECK(fabsf(z[0] - .25f) < 1e-6 && fabsf(z[1] - .75f) < 1e-6 &&
          fabsf(z[3] - .25f) < 1e-6 && fabsf(z[4] - .75f) < 1e-6 &&
          z[2] == -9 && z[5] == -9, "logical depth ROI preserves values and padding");
}

int main(int argc, char** argv)
{
    @autoreleasepool {
        if (argc != 2) { fprintf(stderr, "usage: test percentage\n"); return 2; }
        const unsigned percent = (unsigned)strtoul(argv[1], NULL, 10);
        const u32 extent = percent == 100 ? 4 : percent == 150 ? 6 :
                           percent == 200 ? 8 : percent == 300 ? 12 : 0;
        if (!extent) return 2;
        s_dev = MTLCreateSystemDefaultDevice();
        CHECK(s_dev != nil, "Metal device available"); if (!s_dev) return 1;
        s_queue = [s_dev newCommandQueue]; s_guest_shaders = 1;
        s_headless = 1;
        CHECK(s_engine_backend.init(NULL, 4, 4) == 0, "native backend initializes");
        u32 native_color = s_engine_backend.color_target_create(NULL,
            RSX_BE_FMT_R8G8B8A8, 4, 4, NULL, 0);
        u32 native_depth = s_engine_backend.depth_target_create(NULL, 4, 4);
        if (!render_triangle(&s_engine_backend, native_color, native_depth)) return 1;
        u8 native_pixels[64]; copy_physical_rgba(native_color, native_pixels, 4);
        s_engine_backend.color_target_release(NULL, native_color);
        s_engine_backend.depth_target_release(NULL, native_depth);
        s_engine_backend.shutdown(NULL);

        setenv("MAG_PLAYER_RENDER_SCALE", argv[1], 1);
        const rsx_draw_backend* backend = mag_player_render_backend(&s_engine_backend);
        CHECK(backend != NULL, "player backend available"); if (!backend) return 1;
        CHECK((backend == &s_engine_backend) == (percent == 100), "native scale preserves identity backend");
        CHECK(backend->init(backend->user, 4, 4) == 0, "player backend initializes");
        u32 color = backend->color_target_create(backend->user,
            RSX_BE_FMT_R8G8B8A8, 4, 4, NULL, 0);
        u32 depth = backend->depth_target_create(backend->user, 4, 4);
        const int physical_size = [eng_obj(color) width] == extent &&
            [eng_obj(color) height] == extent && [eng_obj(depth) width] == extent &&
            [eng_obj(depth) height] == extent;
        CHECK(physical_size, "actual color and depth GPU target dimensions");
        if (!physical_size) return 1;
        if (!render_triangle(backend, color, depth)) return 1;
        u8 pixels[12 * 12 * 4]; copy_physical_rgba(color, pixels, extent);
        unsigned lit = 0, detail = 0;
        for (u32 y = 0; y < extent; y++) for (u32 x = 0; x < extent; x++) {
            lit += pixels[(y * extent + x) * 4] > 127;
            u32 source = ((y * 4 / extent) * 4 + x * 4 / extent) * 4;
            detail += memcmp(pixels + (y * extent + x) * 4, native_pixels + source, 4) != 0;
        }
        CHECK(lit > 0 && lit < extent * extent, "triangle covers a proper subset of target");
        CHECK(percent == 100 ? detail == 0 : detail > 0,
              "higher resolution rasterizes diagonal detail absent from nearest-scaled native image");
        fragment_coordinates(backend, color, depth, extent);
        logical_readback(backend, color, depth);
        backend->color_target_release(backend->user, color);
        backend->depth_target_release(backend->user, depth);
        backend->shutdown(backend->user);
        printf("render scale %u%%: physical %ux%u, lit %u, new-detail %u; %d checks, %d failures\n",
               percent, extent, extent, lit, detail, checks, failures);
        return failures ? 1 : 0;
    }
}
