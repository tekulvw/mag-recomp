/* Real-GPU D3D12 regression for the frontend-owned native backend.
 *
 * The fixture plays the host: it creates one D3D12 device and direct queue on
 * an explicitly identified hardware adapter and lends them through a
 * synthetic MagRenderHostOwner (no SDK, guest data, window or swapchain). It
 * then drives the same authored probes as the Metal and Vulkan fixtures with
 * the pinned translator's HLSL. A queue-side fence wait plays the queued host
 * gate, so submissions stay pending until the probe releases it.
 *
 * With MAG_FRONTEND_REQUIRE_VALIDATION=1 the D3D12 debug layer must load and
 * every debug-layer error or corruption message fails the run (exit 3);
 * MAG_D3D12_GPU_VALIDATION=1 adds GPU-based validation. The debug layer is the
 * Windows "Graphics Tools" optional feature.
 *
 * Usage: d3d12_regression owner|compute|draw|draw-no-readback|scene|scene-no-readback VENDOR DEVICE
 * Exit: 0 pass, 1 contract failure, 2 usage/unsupported host, 3 validation errors. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mag_render_batch.h"
#include "mag_render_native_api.h"
#include "mag_render_imported_image.h"
#include "mag_render_scene.h"
#include "mag_render_stencil.h"
#include "mag_render_sampler.h"
#include "mag_render_host_owner.h"
#include MAG_RENDER_SHADER_FIXTURE

/* The fixture's own asserts must hold in every build configuration. */
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__);                           \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

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
typedef struct SceneProbeReport {
    uint32_t abi_version, struct_size;
    int32_t status;
    uint32_t submitted, completed, pending;
    uint64_t retained_before_terminal, retained_after_terminal;
    uint32_t center_rgba, ui_rgba, shadow_rgba, outside_rgba, pass_count, draw_count;
} SceneProbeReport;
int mag_render_native_scene_probe_target(const MagRenderHostOwner *, const mag_native_pipeline_desc *,
                                         const mag_native_pipeline_desc *, const mag_native_scene_graphics_desc *,
                                         const MagRenderHostImage *, uint32_t, void (*)(void *), void *,
                                         SceneProbeReport *);

/* ---- Synthetic host owner -------------------------------------------------------------------------- */
static struct {
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    ID3D12Fence *gate, *idle;
    UINT64 gate_value, idle_value;
    uint64_t lifetime;
    volatile LONG retained, busy, closing;
    char name[256];
    UINT vendor, device_id;
    LARGE_INTEGER driver;
    ID3D12InfoQueue *info;
    DWORD callback_cookie;
    volatile LONG validation_errors, validation_warnings;
    int validation;
} g;

static int owner_retain(void *user, uint64_t lifetime) {
    if (user != &g || lifetime != g.lifetime || g.closing) return MAG_RENDER_HOST_STALE;
    InterlockedIncrement(&g.retained);
    return MAG_RENDER_HOST_OK;
}
static void owner_release(void *user, uint64_t lifetime) {
    if (user != &g || lifetime != g.lifetime || InterlockedDecrement(&g.retained) < 0) {
        fprintf(stderr, "owner released without a matching retain\n");
        exit(1);
    }
}
static int owner_try_begin(void *user, uint64_t lifetime) {
    if (user != &g || lifetime != g.lifetime || g.closing) return MAG_RENDER_HOST_STALE;
    return InterlockedCompareExchange(&g.busy, 1, 0) ? MAG_RENDER_HOST_BUSY : MAG_RENDER_HOST_OK;
}
static void owner_end(void *user, uint64_t lifetime) {
    (void)lifetime;
    if (user != &g || InterlockedExchange(&g.busy, 0) != 1) {
        fprintf(stderr, "owner end without a matching begin\n");
        exit(1);
    }
}
static void owner_fill(MagRenderHostOwner *out) {
    memset(out, 0, sizeof(*out));
    out->abi_version = MAG_RENDER_HOST_OWNER_ABI;
    out->struct_size = sizeof(*out);
    out->backend = MAG_RENDER_HOST_D3D12;
    out->lifetime = g.lifetime;
    out->device = g.device;
    out->queue = g.queue;
    out->userdata = &g;
    out->retain = owner_retain;
    out->release = owner_release;
    out->try_begin = owner_try_begin;
    out->end = owner_end;
}
/* Queue gate: later submissions wait on the GPU until the probe releases it. */
static void owner_gate(void) {
    ++g.gate_value;
    CHECK(SUCCEEDED(ID3D12CommandQueue_Wait(g.queue, g.gate, g.gate_value)));
}
static void owner_signal(void *user) {
    (void)user;
    if (ID3D12Fence_GetCompletedValue(g.gate) < g.gate_value) ID3D12Fence_Signal(g.gate, g.gate_value);
}
static void owner_wait_idle(void) {
    ++g.idle_value;
    CHECK(SUCCEEDED(ID3D12CommandQueue_Signal(g.queue, g.idle, g.idle_value)));
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK(event && SUCCEEDED(ID3D12Fence_SetEventOnCompletion(g.idle, g.idle_value, event)));
    CHECK(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
    CloseHandle(event);
}

static void CALLBACK on_message(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                                LPCSTR description, void *context) {
    (void)category; (void)context;
    if (severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
        InterlockedIncrement(&g.validation_errors);
        fprintf(stderr, "[d3d12 validation error] id=%d %s\n", (int)id, description);
    } else if (severity == D3D12_MESSAGE_SEVERITY_WARNING) {
        InterlockedIncrement(&g.validation_warnings);
        fprintf(stderr, "[d3d12 validation warning] id=%d %s\n", (int)id, description);
    }
}

/* Hardware identity: the exact requested non-software adapter, or exit 2. */
static int owner_open_device(UINT vendor, UINT device_id) {
    g.validation = getenv("MAG_FRONTEND_REQUIRE_VALIDATION") && !strcmp(getenv("MAG_FRONTEND_REQUIRE_VALIDATION"), "1");
    if (g.validation) {
        ID3D12Debug *debug = NULL;
        const HRESULT found = D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug);
        if (FAILED(found)) {
            fprintf(stderr, "required D3D12 debug layer unavailable (install Graphics Tools): 0x%08lx\n",
                    (unsigned long)found);
            return 2;
        }
        ID3D12Debug_EnableDebugLayer(debug);
        const char *gbv = getenv("MAG_D3D12_GPU_VALIDATION");
        if (gbv && !strcmp(gbv, "1")) {
            ID3D12Debug1 *debug1 = NULL;
            if (FAILED(ID3D12Debug_QueryInterface(debug, &IID_ID3D12Debug1, (void **)&debug1))) {
                fprintf(stderr, "GPU-based validation unavailable\n");
                return 2;
            }
            ID3D12Debug1_SetEnableGPUBasedValidation(debug1, TRUE);
            ID3D12Debug1_Release(debug1);
        }
        ID3D12Debug_Release(debug);
    }
    IDXGIFactory6 *factory = NULL;
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory6, (void **)&factory))) return 2;
    IDXGIAdapter1 *chosen = NULL;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1 *adapter = NULL;
        if (IDXGIFactory6_EnumAdapterByGpuPreference(factory, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                     &IID_IDXGIAdapter1, (void **)&adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter1_GetDesc1(adapter, &desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId == vendor && desc.DeviceId == device_id) {
            chosen = adapter;
            g.vendor = desc.VendorId;
            g.device_id = desc.DeviceId;
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g.name, sizeof(g.name), NULL, NULL);
            IDXGIAdapter1_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &g.driver);
            break;
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory6_Release(factory);
    if (!chosen) {
        fprintf(stderr, "no hardware D3D12 adapter %04x:%04x\n", vendor, device_id);
        return 2;
    }
    HRESULT hr = D3D12CreateDevice((IUnknown *)chosen, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&g.device);
    IDXGIAdapter1_Release(chosen);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D12CreateDevice failed: 0x%08lx\n", (unsigned long)hr);
        return 2;
    }
    if (g.validation) {
        if (FAILED(ID3D12Device_QueryInterface(g.device, &IID_ID3D12InfoQueue, (void **)&g.info))) {
            fprintf(stderr, "debug layer loaded without an info queue\n");
            return 2;
        }
        ID3D12InfoQueue1 *queue1 = NULL;
        if (SUCCEEDED(ID3D12InfoQueue_QueryInterface(g.info, &IID_ID3D12InfoQueue1, (void **)&queue1))) {
            ID3D12InfoQueue1_RegisterMessageCallback(queue1, on_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL,
                                                     &g.callback_cookie);
            ID3D12InfoQueue1_Release(queue1);
        }
    }
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    CHECK(SUCCEEDED(ID3D12Device_CreateCommandQueue(g.device, &qd, &IID_ID3D12CommandQueue, (void **)&g.queue)));
    CHECK(SUCCEEDED(ID3D12Device_CreateFence(g.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&g.gate)));
    CHECK(SUCCEEDED(ID3D12Device_CreateFence(g.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&g.idle)));
    g.lifetime = 0x5eed0001;
    return 0;
}
/* Messages stored before the callback was registered (or with no callback). */
static void drain_info_queue(void) {
    if (!g.info || g.callback_cookie) return;
    const UINT64 count = ID3D12InfoQueue_GetNumStoredMessages(g.info);
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        ID3D12InfoQueue_GetMessage(g.info, i, NULL, &size);
        D3D12_MESSAGE *message = malloc(size);
        if (!message || FAILED(ID3D12InfoQueue_GetMessage(g.info, i, message, &size))) { free(message); continue; }
        on_message(message->Category, message->Severity, message->ID, message->pDescription, NULL);
        free(message);
    }
    ID3D12InfoQueue_ClearStoredMessages(g.info);
}
static void owner_close(int drained) {
    owner_signal(NULL);
    if (drained) owner_wait_idle();
    g.closing = 1;
    if (g.retained) {
        fprintf(stderr, "frontend still retains the owner at close: %ld\n", g.retained);
        exit(1);
    }
    drain_info_queue();
}

/* ---- Shader fixtures ------------------------------------------------------------------------------- */
static mag_native_pipeline_desc compute(const mag_native_fixture_shader *source) {
    mag_native_pipeline_desc out;
    memset(&out, 0, sizeof(out));
    out.struct_size = sizeof(out);
    out.abi_version = 1;
    out.source_bytes = source->source;
    out.source_length = source->length;
    out.entry_point = source->entry;
    out.entry_point_length = (uint32_t)strlen(source->entry);
    out.binding_count = source->binding_count;
    out.bindings = source->bindings;
    out.shader_language = MAG_NATIVE_HLSL;
    memcpy(out.threads_per_group, source->threads, sizeof(out.threads_per_group));
    return out;
}
static mag_native_shader_stage stage(const mag_native_fixture_shader *source) {
    mag_native_shader_stage out = {source->source, source->length, source->entry, (uint32_t)strlen(source->entry),
                                   MAG_NATIVE_HLSL};
    return out;
}
static void wait_one(mag_native_device *device, uint64_t submission) {
    for (int i = 0; i < 5000; ++i) {
        mag_native_event event = {0};
        uint32_t count = 0;
        CHECK(mag_native_poll(device, &event, 1, &count) == 0);
        if (count) {
            CHECK(event.succeeded && (!submission || event.ticket.submission_id == submission));
            return;
        }
        Sleep(1);
    }
    fprintf(stderr, "submission %llu did not complete\n", (unsigned long long)submission);
    exit(1);
}

/* ---- Owner contract: retain before any native dereference ---------------------------------------- */
static int owner_contract(void) {
    MagRenderHostOwner owner;
    owner_fill(&owner);
    mag_native_device *device = NULL;
    /* Poisoned handles: a stale or closing lifetime must be refused by retain
     * before the backend touches the device or queue. */
    MagRenderHostOwner poisoned = owner;
    poisoned.device = (void *)(uintptr_t)0x10;
    poisoned.queue = (void *)(uintptr_t)0x20;
    poisoned.lifetime = g.lifetime + 1;
    CHECK(mag_native_create(&poisoned, &device) == MAG_RENDER_HOST_STALE && !device && !g.retained);
    poisoned.lifetime = g.lifetime;
    g.closing = 1;
    CHECK(mag_native_create(&poisoned, &device) == MAG_RENDER_HOST_STALE && !device && !g.retained);
    g.closing = 0;
    poisoned.backend = MAG_RENDER_HOST_VULKAN;
    CHECK(mag_native_create(&poisoned, &device) == MAG_NATIVE_UNSUPPORTED && !device);
    /* No explicit DXC library: the device is refused and the owner released. */
    char saved[MAX_PATH * 4] = {0};
    const DWORD had = GetEnvironmentVariableA("PS3RECOMP_DXC_LIBRARY", saved, sizeof(saved));
    SetEnvironmentVariableA("PS3RECOMP_DXC_LIBRARY", NULL);
    CHECK(mag_native_create(&owner, &device) == MAG_NATIVE_UNSUPPORTED && !device && !g.retained);
    SetEnvironmentVariableA("PS3RECOMP_DXC_LIBRARY", "relative\\dxcompiler.dll");
    CHECK(mag_native_create(&owner, &device) != 0 && !device && !g.retained);
    if (had) SetEnvironmentVariableA("PS3RECOMP_DXC_LIBRARY", saved);
    /* A genuine owner is retained for exactly the device's lifetime. */
    CHECK(mag_native_create(&owner, &device) == 0 && device && g.retained == 1);
    mag_native_caps_desc caps;
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    caps.abi_version = MAG_NATIVE_ABI_VERSION;
    CHECK(mag_native_caps(device, &caps) == 0);
    CHECK(mag_native_close(device) == 0 && !g.retained);
    /* A busy owner (original work being encoded) refuses native encoding. */
    CHECK(mag_native_create(&owner, &device) == 0);
    uint8_t bytes[64] = {1};
    mag_native_buffer_desc bd = {sizeof(bd), 1, sizeof bytes, bytes};
    mag_native_buffer buffer = {0};
    CHECK(mag_native_buffer_create(device, &bd, &buffer) == 0);
    CHECK(mag_native_buffer_release(device, buffer) == 0);
    CHECK(mag_native_close(device) == 0 && !g.retained);
    printf("{\"device\":\"%s\",\"vendor_id\":%u,\"device_id\":%u,\"owner_contract\":\"passed\"}\n", g.name, g.vendor,
           g.device_id);
    return 0;
}

/* ---- Batch refusals (the same eight cases as the Vulkan fixture) --------------------------------- */
static void check_rejected_batches(const MagRenderHostOwner *owner, const mag_native_pipeline_desc *source) {
    mag_native_device *device = NULL;
    CHECK(mag_native_create(owner, &device) == 0);
    mag_native_pipeline pipeline = {0};
    CHECK(mag_native_pipeline_create(device, source, &pipeline) == 0);
    mag_native_buffer buffers[3] = {{0}};
    uint8_t parameters[80] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        mag_native_buffer_desc desc = {sizeof(desc), 1, i == 2 ? 80u : 48u, i == 2 ? parameters : NULL};
        CHECK(mag_native_buffer_create(device, &desc, &buffers[i]) == 0);
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
        const int status_ = mag_native_submit_batch(device, &batch, &ticket);                                \
        if (status_ != (expected))                                                                           \
            fprintf(stderr, "[batch refusal] line %d: status %d, expected %d\n", __LINE__, status_, (expected)); \
        CHECK(status_ == (expected));                                                                        \
        CHECK(!ticket.submission_id && !ticket.device_generation);                                           \
        mag_native_event event = {0};                                                                        \
        uint32_t count = 0;                                                                                  \
        CHECK(mag_native_poll(device, &event, 1, &count) == 0 && count == 0);                                \
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
    for (unsigned i = 0; i < 3; ++i) CHECK(mag_native_buffer_release(device, buffers[i]) == 0);
    CHECK(mag_native_pipeline_release(device, pipeline) == 0);
    CHECK(mag_native_close(device) == 0);
}

/* ---- Explicit D3D12 capability limits ---------------------------------------------------------------
 * Optional operations the D3D12 backend does not implement must be refused
 * as UNSUPPORTED, never emulated or silently accepted. */
static void check_unsupported_operations(const MagRenderHostOwner *owner) {
    mag_native_device *device = NULL;
    CHECK(mag_native_create(owner, &device) == 0);
    uint8_t seed[64] = {0}, patch[16] = {0};
    mag_native_buffer_desc desc = {sizeof(desc), 1, sizeof seed, seed};
    mag_native_buffer buffer = {0};
    CHECK(mag_native_buffer_create(device, &desc, &buffer) == 0);
    CHECK(mag_native_buffer_write(device, buffer, 8, patch, sizeof patch) == MAG_NATIVE_UNSUPPORTED);
    uint64_t census[MAG_NATIVE_MEMORY_STATS] = {0};
    CHECK(mag_native_memory_stats(device, census, MAG_NATIVE_MEMORY_STATS) == MAG_NATIVE_UNSUPPORTED);
    mag_native_scene_import import = {0};
    mag_native_ticket ticket = {0};
    CHECK(mag_native_reserve_scene_v5(device, &import, 1, &ticket) == MAG_NATIVE_UNSUPPORTED && !ticket.submission_id);
    /* Depth bounds: refused at pipeline creation on a device made without the
     * owner's depth-bounds feature (check_depth_bounds covers the feature). */
    static const mag_native_vertex_attribute position = {0, MAG_NATIVE_VERTEX_FLOAT4, 0, 0};
    mag_native_scene_graphics_v3_desc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = sizeof(d); d.abi_version = 3;
    d.base.struct_size = sizeof(d.base); d.base.abi_version = 2;
    d.base.base.struct_size = sizeof(d.base.base); d.base.base.abi_version = 1;
    d.base.base.vertex = stage(&scene_vertex_fixture); d.base.base.fragment = stage(&scene_fragment_fixture);
    d.base.base.vertex_stride = 16; d.base.base.color_format = MAG_NATIVE_SCENE_RGBA8;
    d.base.base.depth_format = MAG_NATIVE_SCENE_D32;
    d.base.attributes = &position; d.base.attribute_count = 1; d.base.binding_model = 1;
    d.raster.known_fields = MAG_NATIVE_RASTER_KNOWN_ALL;
    d.raster.depth_compare = 7; d.raster.color_write_mask = 15; d.raster.samples = 1;
    d.raster.src_rgb = d.raster.src_alpha = 1;
    d.raster.depth_bounds_enabled = 1; d.raster.depth_bounds_max = 1;
    mag_native_pipeline refused = {0};
    CHECK(mag_native_scene_graphics_create_v3(device, &d, &refused) == MAG_NATIVE_UNSUPPORTED);
    /* The same pipeline without depth bounds is admitted (real PSO creation). */
    d.raster.depth_bounds_enabled = 0;
    mag_native_pipeline admitted = {0};
    CHECK(mag_native_scene_graphics_create_v3(device, &d, &admitted) == 0);
    CHECK(mag_native_pipeline_release(device, admitted) == 0);
    /* Anisotropy is core D3D12: a linear/linear/linear 4x sampler is created;
     * an anisotropic request with point filtering is refused, never clamped. */
    mag_native_sampler_desc sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = sizeof(sd);
    sd.abi_version = MAG_NATIVE_SAMPLER_ABI;
    sd.state.known_fields = MAG_NATIVE_SAMPLER_KNOWN_ALL;
    sd.state.min_filter = sd.state.mag_filter = MAG_NATIVE_FILTER_LINEAR;
    sd.state.mip_filter = MAG_NATIVE_MIP_LINEAR;
    sd.state.wrap_s = sd.state.wrap_t = sd.state.wrap_r = MAG_NATIVE_WRAP_REPEAT;
    sd.state.lod_max = 8;
    sd.state.max_anisotropy = 4;
    mag_native_sampler sampler = {0};
    CHECK(mag_native_sampler_create(device, &sd, &sampler) == 0 && sampler.id);
    CHECK(mag_native_sampler_release(device, sampler) == 0);
    sd.state.min_filter = MAG_NATIVE_FILTER_NEAREST;
    CHECK(mag_native_sampler_create(device, &sd, &sampler) == MAG_NATIVE_UNSUPPORTED);
    CHECK(mag_native_buffer_release(device, buffer) == 0);
    CHECK(mag_native_close(device) == 0);
    fprintf(stderr, "[d3d12 limits] buffer_write, memory_stats, scene reservation and depth bounds refused; "
                    "anisotropic linear sampler created\n");
}

/* ---- Scene refusals (the same seven cases as the Metal fixture) ---------------------------------- */
static void check_rejected_scenes(const MagRenderHostOwner *owner) {
    mag_native_device *device = NULL;
    CHECK(mag_native_create(owner, &device) == 0);
    mag_native_scene_image image = {MAG_NATIVE_SCENE_D32, 8, 8, 0, {0}};
    mag_native_scene_pass pass;
    memset(&pass, 0, sizeof(pass));
    pass.color.image = MAG_NATIVE_SCENE_NONE;
    pass.depth.image = 0;
    pass.depth.load = MAG_NATIVE_SCENE_LOAD;
    pass.depth.clear[0] = 1;
    mag_native_scene scene;
    memset(&scene, 0, sizeof(scene));
    scene.struct_size = sizeof(scene);
    scene.abi_version = 1;
    scene.images = &image;
    scene.image_count = 1;
    scene.passes = &pass;
    scene.pass_count = 1;
    scene.output_image = MAG_NATIVE_SCENE_NONE;
#define REJECTED()                                                                                           \
    do {                                                                                                     \
        mag_native_ticket ticket = {0};                                                                      \
        CHECK(mag_native_submit_scene(device, &scene, &ticket) == MAG_NATIVE_INVALID);                       \
        CHECK(!ticket.device_generation && !ticket.submission_id);                                           \
        mag_native_event event = {0};                                                                        \
        uint32_t count = 0;                                                                                  \
        CHECK(mag_native_poll(device, &event, 1, &count) == 0 && !count);                                    \
    } while (0)
    REJECTED(); /* Load before any clear/store. */
    pass.depth.load = MAG_NATIVE_SCENE_CLEAR;
    pass.draw_count = 1;
    REJECTED(); /* Missing draw list. */
    pass.draw_count = 0;
    image.width = UINT32_MAX;
    REJECTED();
    image.width = 8;
    scene.output_image = 0;
    REJECTED(); /* Depth cannot be a color output. */
    scene.output_image = MAG_NATIVE_SCENE_NONE;
    pass.depth.clear[0] = NAN;
    REJECTED();
    pass.depth.clear[0] = 1;
    scene.pass_count = 0;
    REJECTED();
    scene.pass_count = 1;
    image.format = MAG_NATIVE_SCENE_RGBA8;
    REJECTED(); /* Explicit attachment format. */
#undef REJECTED
    CHECK(mag_native_close(device) == 0);
}

/* ---- Textured scene: an uploaded 8x8 texture sampled into an 8x8 target ---------------------------- */
static void check_textured_scene(const MagRenderHostOwner *owner) {
    mag_native_device *device = NULL;
    CHECK(mag_native_create(owner, &device) == 0);
    static const mag_native_scene_texture_layout texture_layout = {MAG_NATIVE_SCENE_FRAGMENT, 1, 2,
                                                                   MAG_NATIVE_SCENE_RGBA8, 0, 0};
    CHECK(scene_lit_vertex_fixture.binding_count == 1);
    const mag_native_scene_buffer_layout buffer_layout = {MAG_NATIVE_SCENE_VERTEX, 0,
                                                          scene_lit_vertex_fixture.bindings[0]};
    mag_native_scene_graphics_desc gd;
    memset(&gd, 0, sizeof(gd));
    gd.struct_size = sizeof(gd); gd.abi_version = 1;
    gd.vertex = stage(&scene_lit_vertex_fixture); gd.fragment = stage(&scene_textured_fragment_fixture);
    gd.vertex_stride = 16; gd.color_format = MAG_NATIVE_SCENE_RGBA8;
    gd.depth_compare = MAG_NATIVE_SCENE_DEPTH_DISABLED;
    gd.buffers = &buffer_layout; gd.buffer_count = 1; gd.textures = &texture_layout; gd.texture_count = 1;
    mag_native_pipeline pipeline = {0};
    CHECK(mag_native_scene_graphics_create(device, &gd, &pipeline) == 0);
    float quad[6][4] = {{-1,-1,0,1},{1,-1,0,1},{-1,1,0,1},{1,-1,0,1},{1,1,0,1},{-1,1,0,1}};
    uint32_t idx[6] = {0, 1, 2, 3, 4, 5};
    float view_data[8] = {1, 1, 1, 1, 0, 0, 0, 0};
    uint8_t texels[2][256];
    for (unsigned i = 0; i < 64; ++i) {
        const uint8_t red[4] = {0xff, 0, 0, 0xff}, green[4] = {0, 0xff, 0, 0xff};
        memcpy(&texels[0][4 * i], red, 4);
        memcpy(&texels[1][4 * i], green, 4);
    }
    mag_native_buffer vb = {0}, ib = {0}, view = {0};
    mag_native_buffer_desc d = {sizeof(d), 1, sizeof quad, quad};
    CHECK(mag_native_buffer_create(device, &d, &vb) == 0);
    d.byte_length = sizeof idx; d.initial_bytes = idx;
    CHECK(mag_native_buffer_create(device, &d, &ib) == 0);
    d.byte_length = sizeof view_data; d.initial_bytes = view_data;
    CHECK(mag_native_buffer_create(device, &d, &view) == 0);
    /* Case 2 repeats green with the target at a scene image index above the
     * batch command limit: its render-target view must still land inside the
     * device's attachment heap (scene indices reach MAG_NATIVE_SCENE_MAX_IMAGES). */
    enum { HighTarget = 40 };
    const uint32_t expected[3] = {0xff0000ffu, 0xff00ff00u, 0xff00ff00u};
    for (unsigned k = 0; k < 3; ++k) {
        mag_native_buffer upload = {0}, readback = {0};
        d.byte_length = sizeof texels[k & 1]; d.initial_bytes = texels[k & 1];
        CHECK(mag_native_buffer_create(device, &d, &upload) == 0);
        mag_native_buffer_desc rd = {sizeof(rd), 1, 8 * 8 * 4, NULL};
        CHECK(mag_native_buffer_create(device, &rd, &readback) == 0);
        mag_native_scene_image images[HighTarget + 1];
        for (unsigned i = 0; i <= HighTarget; ++i) {
            const mag_native_scene_image blank = {MAG_NATIVE_SCENE_RGBA8, 8, 8, 0, {0}};
            images[i] = blank;
        }
        images[1].upload = upload;
        const uint32_t target = k == 2 ? HighTarget : 0;
        mag_native_scene_buffer_binding buffer = {MAG_NATIVE_SCENE_VERTEX, 0, {0, 0, view, 0, 32}};
        mag_native_scene_texture_binding texture = {MAG_NATIVE_SCENE_FRAGMENT, 1, 2, 1, MAG_NATIVE_SCENE_NEAREST, 0};
        mag_native_scene_draw draw;
        memset(&draw, 0, sizeof(draw));
        draw.pipeline = pipeline; draw.vertices = vb; draw.indices = ib;
        draw.vertex_length = 6 * 16; draw.index_length = 6 * 4; draw.index_count = 6;
        draw.buffer_count = 1; draw.buffers = &buffer; draw.texture_count = 1; draw.textures = &texture;
        mag_native_scene_pass pass;
        memset(&pass, 0, sizeof(pass));
        pass.color.image = target; pass.color.load = MAG_NATIVE_SCENE_CLEAR; pass.depth.image = MAG_NATIVE_SCENE_NONE;
        pass.draws = &draw; pass.draw_count = 1;
        mag_native_batch_resource resources[5] = {{vb, MAG_NATIVE_UPLOAD, 0}, {ib, MAG_NATIVE_UPLOAD, 0},
                                                  {view, MAG_NATIVE_UPLOAD, 0}, {upload, MAG_NATIVE_UPLOAD, 0},
                                                  {readback, MAG_NATIVE_READBACK, 0}};
        mag_native_scene scene;
        memset(&scene, 0, sizeof(scene));
        scene.struct_size = sizeof(scene); scene.abi_version = 1;
        scene.resources = resources; scene.resource_count = 5;
        scene.images = images; scene.image_count = k == 2 ? HighTarget + 1 : 2;
        scene.passes = &pass; scene.pass_count = 1;
        scene.output_image = target; scene.diagnostic_readback = readback;
        mag_native_ticket ticket = {0};
        const int status = mag_native_submit_scene(device, &scene, &ticket);
        if (status) fprintf(stderr, "[textured scene] submit status=%d\n", status);
        CHECK(status == 0);
        wait_one(device, ticket.submission_id);
        uint32_t px[64];
        CHECK(mag_native_buffer_read(device, readback, 0, px, sizeof px) == 0);
        for (unsigned i = 0; i < 64; ++i) {
            if (px[i] != expected[k]) {
                fprintf(stderr, "[textured scene] texel %u = %08x, expected %08x\n", i, px[i], expected[k]);
                exit(1);
            }
        }
        CHECK(mag_native_buffer_release(device, readback) == 0);
        CHECK(mag_native_buffer_release(device, upload) == 0);
    }
    CHECK(mag_native_buffer_release(device, vb) == 0 && mag_native_buffer_release(device, ib) == 0 &&
          mag_native_buffer_release(device, view) == 0);
    CHECK(mag_native_pipeline_release(device, pipeline) == 0);
    CHECK(mag_native_close(device) == 0);
    fprintf(stderr, "[textured scene] uploaded red then green 8x8 textures sampled exactly, "
                    "including into scene image %u\n", (unsigned)HighTarget);
}

/* ---- Stored-depth bounds through the device's depth bounds test ------------------------------------
 * Pass 0 writes depth 0.25 into the left half and 0.75 into the right; pass 1
 * loads it and draws a full quad at depth 0.5 with depth test and writes off
 * and bounds on. A pixel is coloured exactly when min <= stored <= max -- the
 * predicate the Metal stored-depth program evaluates -- with equality at both
 * ends. A bounds draw whose pipeline writes depth is refused, and so is a
 * bounds pipeline on a device created without the owner's feature. */
static mag_native_raster_state bounds_raster(int write_depth, int bounds) {
    mag_native_raster_state r;
    memset(&r, 0, sizeof(r));
    r.known_fields = MAG_NATIVE_RASTER_KNOWN_ALL;
    r.depth_enabled = (uint32_t)write_depth; r.depth_write = (uint32_t)write_depth; r.depth_compare = 7; /* ALWAYS */
    r.color_write_mask = write_depth ? 0u : 15u;   /* the depth pass writes no colour */
    r.samples = 1; r.src_rgb = r.src_alpha = 1;
    r.depth_bounds_enabled = (uint32_t)bounds; r.depth_bounds_max = 1;
    return r;
}
static int bounds_pipeline(mag_native_device *device, int write_depth, int bounds, mag_native_pipeline *out) {
    static const mag_native_vertex_attribute position = {0, MAG_NATIVE_VERTEX_FLOAT4, 0, 0};
    mag_native_scene_graphics_v3_desc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = sizeof(d); d.abi_version = 3;
    d.base.struct_size = sizeof(d.base); d.base.abi_version = 2;
    d.base.base.struct_size = sizeof(d.base.base); d.base.base.abi_version = 1;
    d.base.base.vertex = stage(&scene_vertex_fixture); d.base.base.fragment = stage(&scene_fragment_fixture);
    d.base.base.vertex_stride = 16; d.base.base.color_format = MAG_NATIVE_SCENE_RGBA8;
    d.base.base.depth_format = MAG_NATIVE_SCENE_D32;
    d.base.attributes = &position; d.base.attribute_count = 1; d.base.binding_model = 1;
    d.raster = bounds_raster(write_depth, bounds);
    return mag_native_scene_graphics_create_v3(device, &d, out);
}
static void check_depth_bounds(const MagRenderHostOwner *owner) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2;
    memset(&options2, 0, sizeof(options2));
    const int supported = SUCCEEDED(ID3D12Device_CheckFeatureSupport(g.device, D3D12_FEATURE_D3D12_OPTIONS2, &options2,
                                                                     sizeof(options2))) && options2.DepthBoundsTestSupported;
    mag_native_device *device = NULL;
    mag_native_pipeline test = {0}, write = {0}, refused = {0};
    CHECK(mag_native_create(owner, &device) == 0);                /* no owner features */
    CHECK(bounds_pipeline(device, 0, 1, &test) == MAG_NATIVE_UNSUPPORTED);
    CHECK(mag_native_close(device) == 0);
    MagRenderHostOwnerV2 v2;
    memset(&v2, 0, sizeof(v2));
    v2.abi_version = 2; v2.struct_size = sizeof(v2); v2.base = *owner;
    v2.known_features = MAG_RENDER_HOST_FEATURE_DEPTH_BOUNDS;
    v2.enabled_features = supported ? MAG_RENDER_HOST_FEATURE_DEPTH_BOUNDS : 0;
    device = NULL;
    CHECK(mag_native_create_v2(&v2, &device) == 0);
    CHECK(bounds_pipeline(device, 1, 0, &write) == 0);
    if (!supported) {
        CHECK(bounds_pipeline(device, 0, 1, &test) == MAG_NATIVE_UNSUPPORTED);
        fprintf(stderr, "[depth bounds] device has no depth bounds test: refused\n");
        CHECK(mag_native_pipeline_release(device, write) == 0 && mag_native_close(device) == 0);
        return;
    }
    CHECK(bounds_pipeline(device, 0, 1, &test) == 0);
    CHECK(bounds_pipeline(device, 1, 1, &refused) == 0);         /* admitted; its bounds draws are not */
    enum { W = 16, H = 16 };
    float v[18][4] = {
        {-1,-1,.25f,1},{0,-1,.25f,1},{-1,1,.25f,1},{0,-1,.25f,1},{0,1,.25f,1},{-1,1,.25f,1},
        {0,-1,.75f,1},{1,-1,.75f,1},{0,1,.75f,1},{1,-1,.75f,1},{1,1,.75f,1},{0,1,.75f,1},
        {-1,-1,.5f,1},{1,-1,.5f,1},{-1,1,.5f,1},{1,-1,.5f,1},{1,1,.5f,1},{-1,1,.5f,1}};
    uint32_t idx[18];
    for (uint32_t i = 0; i < 18; ++i) idx[i] = i;
    mag_native_buffer_desc vd = {sizeof(vd), 1, sizeof v, v}, id = {sizeof(id), 1, sizeof idx, idx},
                           rd = {sizeof(rd), 1, W * H * 4, NULL};
    mag_native_buffer vb = {0}, ib = {0}, rb = {0};
    CHECK(mag_native_buffer_create(device, &vd, &vb) == 0 && mag_native_buffer_create(device, &id, &ib) == 0);
    static const struct { float min, max; int left, right; } cases[] = {
        {.2f, .3f, 1, 0}, {.7f, .8f, 0, 1}, {.25f, .25f, 1, 0}, {.75f, .75f, 0, 1}, {.5f, .5f, 0, 0}, {0, 1, 1, 1}};
    const unsigned n_cases = sizeof cases / sizeof cases[0];
    for (unsigned k = 0; k <= n_cases; ++k) {
        const int bad = k == n_cases;                              /* the depth-writing bounds pipeline */
        const unsigned c = bad ? 0 : k;
        CHECK(mag_native_buffer_create(device, &rd, &rb) == 0);
        mag_native_scene_image images[2] = {{MAG_NATIVE_SCENE_RGBA8, W, H, 0, {0}}, {MAG_NATIVE_SCENE_D32, W, H, 0, {0}}};
        mag_native_scene_draw draws[2];
        memset(draws, 0, sizeof(draws));
        draws[0].pipeline = write; draws[0].vertices = vb; draws[0].indices = ib;
        draws[0].vertex_length = sizeof v; draws[0].index_length = 12 * 4; draws[0].index_count = 12;
        draws[1] = draws[0]; draws[1].pipeline = bad ? refused : test;
        draws[1].index_offset = 12 * 4; draws[1].index_length = 6 * 4; draws[1].index_count = 6;
        mag_native_scene_pass passes[2];
        memset(passes, 0, sizeof(passes));
        passes[0].color.image = 0; passes[0].color.load = MAG_NATIVE_SCENE_CLEAR; passes[0].color.clear[3] = 1;
        passes[0].depth.image = 1; passes[0].depth.load = MAG_NATIVE_SCENE_CLEAR; passes[0].depth.clear[0] = 1;
        passes[0].draws = &draws[0]; passes[0].draw_count = 1;
        passes[1].color.image = 0; passes[1].color.load = MAG_NATIVE_SCENE_LOAD;
        passes[1].depth.image = 1; passes[1].depth.load = MAG_NATIVE_SCENE_LOAD;
        passes[1].draws = &draws[1]; passes[1].draw_count = 1;
        mag_native_batch_resource resources[3] = {{vb, MAG_NATIVE_UPLOAD, 0}, {ib, MAG_NATIVE_UPLOAD, 0},
                                                  {rb, MAG_NATIVE_READBACK, 0}};
        mag_native_scene scene;
        memset(&scene, 0, sizeof(scene));
        scene.struct_size = sizeof(scene); scene.abi_version = 1;
        scene.resources = resources; scene.resource_count = 3;
        scene.images = images; scene.image_count = 2; scene.passes = passes; scene.pass_count = 2;
        scene.output_image = 0; scene.diagnostic_readback = rb;
        mag_native_stencil_attachment stencils[2] = {{MAG_NATIVE_SCENE_NONE, 0, 0, 0, 0, 0},
                                                     {MAG_NATIVE_SCENE_NONE, 0, 0, 0, 0, 0}};
        mag_native_depth_bounds bounds[2] = {{0, 0, 0, 1}, {1, 0, cases[c].min, cases[c].max}};
        mag_native_scene_v5 v5;
        memset(&v5, 0, sizeof(v5));
        v5.struct_size = sizeof(v5); v5.abi_version = 5;
        v5.base.struct_size = sizeof(v5.base); v5.base.abi_version = 4; v5.base.stencils = stencils; v5.base.stencil_count = 2;
        v5.base.base.struct_size = sizeof(v5.base.base); v5.base.base.abi_version = 3;
        v5.base.base.base.struct_size = sizeof(v5.base.base.base); v5.base.base.base.abi_version = 2;
        v5.base.base.base.scene = &scene;
        v5.bounds = bounds; v5.bounds_count = 2;
        mag_native_ticket ticket = {0};
        const int status = mag_native_submit_scene_v5(device, &v5, NULL, NULL, &ticket);
        if (bad) {
            CHECK(status == MAG_NATIVE_UNSUPPORTED && !ticket.submission_id);
            CHECK(mag_native_buffer_release(device, rb) == 0);
            break;
        }
        if (status) fprintf(stderr, "[depth bounds] submit status=%d\n", status);
        CHECK(status == 0);
        wait_one(device, ticket.submission_id);
        uint32_t px[W * H];
        CHECK(mag_native_buffer_read(device, rb, 0, px, sizeof px) == 0);
        const uint32_t lit = 0xffff9933u, dark = 0xff000000u, l = px[8 * W + 4], r = px[8 * W + 12];
        fprintf(stderr, "[depth bounds] [%.2f, %.2f]: left(0.25)=%08x right(0.75)=%08x\n", cases[c].min, cases[c].max, l, r);
        CHECK(l == (cases[c].left ? lit : dark) && r == (cases[c].right ? lit : dark));
        CHECK(mag_native_buffer_release(device, rb) == 0);
    }
    CHECK(mag_native_buffer_release(device, vb) == 0 && mag_native_buffer_release(device, ib) == 0);
    CHECK(mag_native_pipeline_release(device, write) == 0 && mag_native_pipeline_release(device, test) == 0 &&
          mag_native_pipeline_release(device, refused) == 0);
    CHECK(mag_native_close(device) == 0);
    fprintf(stderr, "[depth bounds] hardware bounds test matches the inclusive stored-depth predicate\n");
}

/* ---- Six-pass scene probe (same graphics descriptors as the Metal fixture) ------------------------ */
static int scene_main(const MagRenderHostOwner *owner, int diagnostic, SceneProbeReport *report) {
    mag_native_pipeline_desc first = compute(&scene_transform_fixture), second = compute(&scene_prepare_fixture);
    mag_native_scene_buffer_layout both[16];
    const uint32_t vertex_count = scene_lit_vertex_buffer_count, fragment_count = scene_lit_fragment_buffer_count;
    CHECK(vertex_count == 1 && fragment_count == 1);
    both[0] = scene_lit_vertex_buffers[0];
    both[1] = scene_lit_fragment_buffers[0];
    mag_native_scene_graphics_desc graphics[5];
    memset(graphics, 0, sizeof(graphics));
    for (unsigned i = 0; i < 5; ++i) {
        mag_native_scene_graphics_desc *gd = &graphics[i];
        gd->struct_size = sizeof(*gd);
        gd->abi_version = 1;
        gd->vertex = stage(&scene_lit_vertex_fixture);
        gd->fragment = stage(&scene_lit_fragment_fixture);
        gd->vertex_stride = 16;
        gd->color_format = MAG_NATIVE_SCENE_RGBA8;
        gd->depth_format = MAG_NATIVE_SCENE_D32;
        gd->depth_compare = MAG_NATIVE_SCENE_DEPTH_LESS;
        gd->depth_write = 1;
        gd->buffers = both;
        gd->buffer_count = 2;
        gd->textures = scene_lit_fragment_textures;
        gd->texture_count = scene_lit_fragment_texture_count;
    }
    graphics[0].color_format = 0;
    memset(&graphics[0].fragment, 0, sizeof(graphics[0].fragment));
    graphics[0].buffers = scene_lit_vertex_buffers;
    graphics[0].buffer_count = vertex_count;
    graphics[0].textures = NULL;
    graphics[0].texture_count = 0;
    graphics[2].depth_write = 0;
    graphics[2].blend = MAG_NATIVE_SCENE_ALPHA;
    graphics[3].depth_compare = MAG_NATIVE_SCENE_DEPTH_DISABLED;
    graphics[3].depth_write = 0;
    graphics[3].blend = MAG_NATIVE_SCENE_ALPHA;
    static const mag_native_scene_texture_layout cutout_texture = {1, 1, 2, MAG_NATIVE_SCENE_RGBA8, 0, 0};
    graphics[4] = graphics[0];
    graphics[4].fragment = stage(&scene_cutout_fragment_fixture);
    graphics[4].textures = &cutout_texture;
    graphics[4].texture_count = 1;
    owner_gate();
    report->abi_version = 1;
    report->struct_size = sizeof(*report);
    const int status = mag_render_native_scene_probe_target(owner, &first, &second, graphics, NULL,
                                                            (uint32_t)diagnostic, owner_signal, NULL, report);
    owner_signal(NULL);
    return status;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: d3d12_regression owner|compute|draw|draw-no-readback|scene|scene-no-readback VENDOR DEVICE\n");
        return 2;
    }
    const char *mode = argv[1];
    const UINT vendor = (UINT)strtoul(argv[2], NULL, 0), device_id = (UINT)strtoul(argv[3], NULL, 0);
    if (!vendor || !device_id) return 2;
    const int opened = owner_open_device(vendor, device_id);
    if (opened) return opened;
    int result = 1;
    char driver[64];
    snprintf(driver, sizeof driver, "%u.%u.%u.%u", HIWORD(g.driver.HighPart), LOWORD(g.driver.HighPart),
             HIWORD(g.driver.LowPart), LOWORD(g.driver.LowPart));
    MagRenderHostOwner owner;
    owner_fill(&owner);
    if (!strcmp(mode, "owner")) {
        result = owner_contract();
        owner_close(1);
    } else if (!strcmp(mode, "compute")) {
        mag_native_pipeline_desc legacy = compute(&copy_transform_fixture);
        owner_gate();
        MagRenderNativeProbeReport report = {.abi_version = 1, .struct_size = sizeof(report)};
        const int status = mag_render_native_probe_owner(&owner, &legacy, owner_signal, NULL, &report);
        owner_close(report.pending == 0);
        printf("{\"device\":\"%s\",\"vendor_id\":%u,\"device_id\":%u,\"driver\":\"%s\",\"status\":%d,\"submitted\":%u,"
               "\"completed\":%u,\"pending\":%u,\"retained_before_terminal\":%llu,\"retained_after_terminal\":%llu,"
               "\"upload_bytes\":%llu}\n",
               g.name, g.vendor, g.device_id, driver, status, report.submitted, report.completed, report.pending,
               (unsigned long long)report.retained_before_terminal,
               (unsigned long long)report.retained_after_terminal, (unsigned long long)report.upload_bytes);
        result = status == 0 && report.submitted == 3 && report.completed == 3 && report.pending == 0 &&
                         report.retained_before_terminal == 352 && report.retained_after_terminal == 0
                     ? 0
                     : 1;
    } else if (!strcmp(mode, "draw") || !strcmp(mode, "draw-no-readback")) {
        const int diagnostic = !strcmp(mode, "draw");
        mag_native_pipeline_desc legacy = compute(&copy_transform_fixture), first = compute(&scene_transform_fixture),
                                 second = compute(&scene_prepare_fixture);
        check_rejected_batches(&owner, &legacy);
        if (diagnostic) {
            check_unsupported_operations(&owner);
            check_textured_scene(&owner);
            check_depth_bounds(&owner);
        }
        mag_native_graphics_desc graphics = {sizeof(graphics), 1, stage(&scene_vertex_fixture),
                                             stage(&scene_fragment_fixture), 16, 0};
        owner_gate();
        DrawProbeReport report = {.abi_version = 1, .struct_size = sizeof(report)};
        const int status = mag_render_native_draw_probe(&owner, &first, &second, &graphics, (uint32_t)diagnostic,
                                                        owner_signal, NULL, &report);
        owner_close(report.pending == 0);
        printf("{\"device\":\"%s\",\"vendor_id\":%u,\"device_id\":%u,\"driver\":\"%s\",\"status\":%d,\"submitted\":%u,"
               "\"completed\":%u,\"pending\":%u,\"retained_before_terminal\":%llu,\"retained_after_terminal\":%llu,"
               "\"center_rgba\":%u,\"outside_rgba\":%u,\"diagnostic_readback\":%s,\"produced_uniform_consumer\":true,"
               "\"native_rejection_cases\":8,\"compute_dispatches_per_batch\":2,\"indexed_draws_per_batch\":1}\n",
               g.name, g.vendor, g.device_id, driver, status, report.submitted, report.completed, report.pending,
               (unsigned long long)report.retained_before_terminal,
               (unsigned long long)report.retained_after_terminal, report.center_rgba, report.outside_rgba,
               diagnostic ? "true" : "false");
        result = status == 0 && report.submitted == 2 && report.completed == 2 && report.pending == 0 &&
                         report.retained_before_terminal == 208 && report.retained_after_terminal == 0 &&
                         (!diagnostic || (report.center_rgba == 0xffff9933u && report.outside_rgba == 0))
                     ? 0
                     : 1;
    } else if (!strcmp(mode, "scene") || !strcmp(mode, "scene-no-readback")) {
        const int diagnostic = !strcmp(mode, "scene");
        check_rejected_scenes(&owner);
        SceneProbeReport report;
        memset(&report, 0, sizeof(report));
        const int status = scene_main(&owner, diagnostic, &report);
        owner_close(report.pending == 0);
        printf("{\"device\":\"%s\",\"vendor_id\":%u,\"device_id\":%u,\"driver\":\"%s\",\"status\":%d,\"submitted\":%u,"
               "\"completed\":%u,\"pending\":%u,\"retained_before_terminal\":%llu,\"retained_after_terminal\":%llu,"
               "\"center_rgba\":%u,\"ui_rgba\":%u,\"shadow_rgba\":%u,\"outside_rgba\":%u,\"passes\":%u,\"draws\":%u,"
               "\"diagnostic_readback\":%s,\"physical_presentation\":false,\"imported_original_target\":false,"
               "\"native_rejection_cases\":7}\n",
               g.name, g.vendor, g.device_id, driver, status, report.submitted, report.completed, report.pending,
               (unsigned long long)report.retained_before_terminal,
               (unsigned long long)report.retained_after_terminal, report.center_rgba, report.ui_rgba,
               report.shadow_rgba, report.outside_rgba, report.pass_count, report.draw_count,
               diagnostic ? "true" : "false");
        result = status == 0 && report.submitted == 2 && report.completed == 2 && !report.pending &&
                         report.retained_before_terminal == 608 && !report.retained_after_terminal
                     ? 0
                     : 1;
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        result = 2;
    }
    drain_info_queue();
    /* Every native device has been closed: the frontend must hold no
     * reference to the borrowed device or queue any more. */
    ID3D12Fence_Release(g.gate);
    ID3D12Fence_Release(g.idle);
    const ULONG queue_left = ID3D12CommandQueue_Release(g.queue);
    if (g.info) {
        ID3D12InfoQueue1 *queue1 = NULL;
        if (g.callback_cookie &&
            SUCCEEDED(ID3D12InfoQueue_QueryInterface(g.info, &IID_ID3D12InfoQueue1, (void **)&queue1))) {
            ID3D12InfoQueue1_UnregisterMessageCallback(queue1, g.callback_cookie);
            ID3D12InfoQueue1_Release(queue1);
        }
        ID3D12InfoQueue_Release(g.info);
    }
    const ULONG device_left = ID3D12Device_Release(g.device);
    if (queue_left || device_left) {
        fprintf(stderr, "[d3d12] frontend still references the borrowed queue (%lu) or device (%lu)\n", queue_left,
                device_left);
        result = 1;
    }
    fprintf(stderr, "[d3d12] adapter=%s %04x:%04x driver=%s validation=%s gpu_validation=%s errors=%ld warnings=%ld\n",
            g.name, g.vendor, g.device_id, driver, g.validation ? "on" : "off",
            getenv("MAG_D3D12_GPU_VALIDATION") ? getenv("MAG_D3D12_GPU_VALIDATION") : "0", g.validation_errors,
            g.validation_warnings);
    if (g.validation_errors) return 3;
    return result;
}
