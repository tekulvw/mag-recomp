/* The pinned runtime's real cellPad SDL2 backend, renamed onto the SDL seam (player_input_sdl_pad.c)
 * exactly as cmake/MagControls.cmake and cmake/MagPlayerSettings.cmake rename it, with the real
 * capture layer (player_input_capture.cpp) and synthetic SDL virtual controllers. No physical
 * controller, window or game file. tests/test_player_input_sdl_cellpad.py builds and runs it.
 *
 * Checked: a controller registers with the capture layer at the port cellPad gives it, not its SDL
 * device index; with PS3RECOMP_PAD_CLAIM_ON_INPUT an idle XInput-style pad connected first does not
 * take port 0 from the DualSense that is played with; the capture layer then reports the DualSense's
 * raw buttons (Cross as SDL button A) and sticks, and the settings input block masks only what the
 * SDK sees. */
static int s_focused = 1;
#define PAD_SDL_FOCUS_OVERRIDE() s_focused
#define PS3RECOMP_PAD_USE_SDL2 1
#include "libs/input/cellPad.c"
#include "player_input_capture.h"
#include "player_settings_runtime.h"

static uint8_t s_guest[0x10000];
uint8_t* vm_base = s_guest;
uint32_t ppu_vm_size = sizeof s_guest;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
#if defined(__APPLE__) && !TARGET_OS_IPHONE
PS3RecompMacOSPadState ps3recomp_pad_macos_state(void) { PS3RecompMacOSPadState s = {0, 128, 128, 128, 128}; return s; }
#endif
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
u32 cellGcm_flip_request_count(void) { return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#if SDL_VERSION_ATLEAST(2, 24, 0)
static SDL_JoystickID attach(const char* name, Uint16 vendor, Uint16 product)
{
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.vendor_id = vendor;
    desc.product_id = product;
    desc.name = name;
    const int device = SDL_JoystickAttachVirtualEx(&desc);
    CHECK(device >= 0);
    return SDL_JoystickGetDeviceInstanceID(device);
}

static uint64_t s_sequence;

/* One native poll, as the controls adapter frames it; 0 when the capture layer discards it. */
static int try_sample(MagInputControllerSample* out)
{
    memset(out, 0, sizeof *out);
    CHECK(mag_input_capture_poll_begin(++s_sequence));
    pad_poll_sdl2();
    return mag_input_capture_poll_complete(s_sequence, out);
}

static MagInputControllerSample sample(void)
{
    MagInputControllerSample out;
    CHECK(try_sample(&out));
    return out;
}
#endif

int main(void)
{
#if !SDL_VERSION_ATLEAST(2, 24, 0)
    printf("SKIP: SDL %d.%d.%d has no SDL_JoystickAttachVirtualEx (2.24+)\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
           SDL_PATCHLEVEL);
    return 0;
#else
    static const char* const off[] = {
        "SDL_JOYSTICK_HIDAPI", "SDL_JOYSTICK_RAWINPUT", "SDL_JOYSTICK_WGI", "SDL_XINPUT_ENABLED",
        "SDL_DIRECTINPUT_ENABLED", "SDL_JOYSTICK_GAMEINPUT", "SDL_JOYSTICK_MFI", "SDL_JOYSTICK_IOKIT",
        "SDL_JOYSTICK_LINUX_CLASSIC", "SDL_JOYSTICK_DEVICE",
    };
    for (size_t i = 0; i < sizeof off / sizeof off[0]; i++) SDL_SetHint(off[i], "0");
    s_pad_initialized = 1;
    s_max_connect = 4;
    s_sdl_claim_on_input = 1;   /* the Windows compiled default (src/mag_runtime_defaults.h) */
    pad_init_backend();
    if (SDL_NumJoysticks() != 0) { printf("SKIP: host controllers still visible\n"); return 0; }

    const SDL_JoystickID keypad = attach("Synthetic keypad XInput interface", 0x045E, 0x028E);
    const SDL_JoystickID dualsense = attach("Synthetic DualSense", 0x054C, 0x0CE6);
    MagInputControllerSample s = sample();
    CHECK(!s.connected);   /* neither has a port yet */
    CHECK(!s_host_state[0].connected);

    /* The DualSense (SDL device 1) is played with: it takes port 0, and the capture layer follows. */
    SDL_Joystick* pad = SDL_JoystickFromInstanceID(dualsense);
    CHECK(pad);
    CHECK(SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_A, 1) == 0);
    CHECK(SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_LEFTX, 32767) == 0);
    CHECK(SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY, -32768) == 0);
    /* The poll in which port 0 changes controller is discarded by the capture layer (its
     * controller changed mid-sample); cellPad reports the claiming press at once. */
    CHECK(!try_sample(&s));
    CHECK(s_sdl_instance[0] == dualsense && pad_sdl_waiting_of(keypad) >= 0);
    CHECK(s_host_state[0].connected && (s_host_state[0].buttons & CELL_PAD_CTRL_CROSS));
    s = sample();
    CHECK(s.connected && s.buttons == 1u << SDL_CONTROLLER_BUTTON_A);
    CHECK(s.move_x == 1.0 && s.look_y == -1.0 && s.move_y == 0.0);

    /* The settings input block masks the SDK's reading, not the capture layer's. */
    mag_player_set_input_blocked(1);
    s = sample();
    CHECK(s.connected && s.buttons == 1u << SDL_CONTROLLER_BUTTON_A && s.move_x == 1.0);
    CHECK(s_host_state[0].buttons == 0 && s_host_state[0].analog_lx == pad_sdl_axis_to_u8(0));
    mag_player_set_input_blocked(0);

    /* Unplugged: port 0 and the capture sample go away together. */
    CHECK(SDL_JoystickDetachVirtual(1) == 0);
    s = sample();
    CHECK(!s.connected && !s_host_state[0].connected);

    pad_shutdown_backend();
    printf("player_input_sdl_cellpad: ok\n");
    return 0;
#endif
}
