#include "mag_particle_cohort_ablation.h"
#include "mag_c630_dispatch_observation.h"
#include "mag_dynamic_light_ablation.h"
#include "mag_grid_level_ablation.h"
#include "mag_occlude_input_ablation.h"
#include "mag_cull_input_ablation.h"
#include "mag_remaining_item_ablation.h"
#include "mag_depth_item_ablation.h"
#include "mag_main_item_ablation.h"
#include "mag_render_diagnostics.h"
#include "mag_native_provenance_watch.h"
#include "mag_native_completion.h"
/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"
#include "macos_runloop.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
void     cellGame_init_from_paramsfo(const char* path);
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);
void     mag_queue_snapshot(void);
void     mag_job_marker_snapshot(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */
extern uint32_t ppu_hle_inject_base;          /* title-selected runtime structures */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
#include "gcm_flip_receipt.h"
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" void rsx_metal_backend_present_guest_frame(uint64_t guest_flip_id, uint32_t buffer_id);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;
/* Initialized before the frame clock or guest workers start. The SDK owns the
 * process-lifetime auto-reset event; this thread only waits without holding
 * the RSX consumer lease. A wake requests real progress, never completion. */
static bool g_native_consumer = false;
extern "C" void cellGcm_native_consumer_init(void);
extern "C" int cellGcm_native_consumer_enabled(void);
extern "C" void cellGcm_fifo_kick_wait(unsigned milliseconds);

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

/* Same lease as FIFO/report retirement and guest mapping transitions. */
extern "C" void cellGcm_rsx_consumer_begin(void);
extern "C" void cellGcm_rsx_consumer_end(void);
struct MagRsxConsumerLease {
    MagRsxConsumerLease(){cellGcm_rsx_consumer_begin();}
    ~MagRsxConsumerLease(){cellGcm_rsx_consumer_end();}
};

static void present_guest_frame(const CellGcmFlipReceipt& receipt)
{
    MagRsxConsumerLease consumer_lease;
#if defined(__APPLE__)
    rsx_metal_backend_present_guest_frame(receipt.guest_flip_id,receipt.buffer_id);
#else
    rsx_backend_present();
#endif
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    LONG count = InterlockedIncrement(&g_frames_presented);
    if (count <= 3 || count % 120 == 0)
        fprintf(stderr, "[mag] guest frame presented %ld\n", (long)count);
}

static DWORD WINAPI frame_clock(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "MAG v1.0 — native macOS";

#if defined(__APPLE__)
    // Native diagnostics default on; an explicit 0 preserves the plain title.
    if (!getenv("PS3RECOMP_METAL_FPS")) setenv("PS3RECOMP_METAL_FPS", "1", 0);
#endif
    int rsx_ok;
    {MagRsxConsumerLease consumer_lease;rsx_ok=(rsx_backend_init(WINDOW_WIDTH,WINDOW_HEIGHT,title)==0);}
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    unsigned last_flip_count = 0;
    ULONGLONG next_tick = GetTickCount64();
    ULONGLONG next_snapshot = next_tick + 1000;
    ULONGLONG next_fallback_present = next_tick + 4;

    for (;;) {
        if (g_native_consumer) cellGcm_fifo_kick_wait(4);
        else Sleep(4);
        mag_native_provenance_poll();
        ULONGLONG now = GetTickCount64();
        if (now >= next_snapshot) { mag_queue_snapshot(); mag_job_marker_snapshot(); next_snapshot = now + 1000; }

        CellGcmFlipReceipt receipt = {};
        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_receipt(&receipt)) {
                present_guest_frame(receipt);
                /* Preserve the historical post-present scheduling snapshot;
                 * the immutable receipt above carries this submission's ID. */
                last_flip_count = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += 16;             /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_receipt(&receipt)) {
                present_guest_frame(receipt);
                /* Preserve the historical post-present scheduling snapshot;
                 * the immutable receipt above carries this submission's ID. */
                last_flip_count = cellGcm_flip_request_count();
            }
            cellGcm_rsx_process_fifo();

            int pump_result;
            {MagRsxConsumerLease consumer_lease;pump_result=rsx_backend_pump();}
            if (pump_result != 0) {
                /* A blocked guest may never unwind. End the owned host
                 * session without running teardown against live workers. */
                _Exit(0);
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present at the original idle cadence, so completion wakes cannot
             * create a self-sustaining stream of fallback submissions. */
            cellGcm_latest_flip_receipt(&receipt);
            if (receipt.flip_count != last_flip_count) {
                present_guest_frame(receipt);
                last_flip_count = receipt.flip_count;
            } else if (receipt.flip_count == 0 &&
                       (!g_native_consumer || GetTickCount64() >= next_fallback_present)) {
                MagRsxConsumerLease consumer_lease;
                rsx_backend_present();
                /* Start from completion of this host call, as Sleep(4) did;
                 * never catch up missed fallback presents after a long drain. */
                if (g_native_consumer) next_fallback_present = GetTickCount64() + 4;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is committed up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left. */
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc < 2) {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== ps3recomp game runner ===\n");

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    /* MAG reserves [0x20000000,0x30000000) for its resource pool. Its
     * EdgeZlib output legitimately uses that range, including the SDK's
     * default GCM labels/control/tables. Select a separate runtime window
     * before ELF/module loading, graphics initialization or guest threads. */
    ppu_hle_inject_base = 0x70000000u;

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    mag_particle_cohort_ablation_vm_begin();
    mag_native_depth_vm_begin(argv[1]);
    (void)mag_c630_dispatch_observation_init();
    (void)mag_particle_cohort_ablation_init();
    (void)mag_dynamic_light_ablation_init();
    (void)mag_grid_descendant_ablation_init();
    (void)mag_level_descendant_ablation_init();
    (void)mag_occlude_input_ablation_init();
    (void)mag_cull_input_ablation_init();
    (void)mag_shadow_item_ablation_init();
    (void)mag_other_item_ablation_init();
    (void)mag_depth_item_ablation_init();
    (void)mag_main_item_ablation_init();
    mag_render_diagnostics_start();
    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);
    /* Our runner always uses a private disc-layout overlay. */
    char sfo[4096];
    snprintf(sfo, sizeof(sfo), "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
    cellGame_init_from_paramsfo(sfo);

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
#if defined(__APPLE__)
    /* MAG patches fragment-program constants through NV308A inline data about
     * 1,100 times per menu frame. Draining the GPU before each one held the
     * menu near 10 fps; report ordering is kept without it. An explicit 0
     * restores the drain for comparison. */
    if (!getenv("PS3RECOMP_GCM_INLINE_NO_GPU_WAIT"))
        setenv("PS3RECOMP_GCM_INLINE_NO_GPU_WAIT", "1", 0);
    /* Wake the FIFO drain when the guest moves the put pointer instead of
     * polling it every 4 ms: SPU jobs that wait on RSX labels otherwise sit
     * out the rest of the poll (training mission ~99 -> ~93 ms per update).
     * An explicit 0 restores the timed poll. */
    if (!getenv("MAG_NATIVE_CONSUMER"))
        setenv("MAG_NATIVE_CONSUMER", "1", 0);
#endif
    cellGcm_native_consumer_init();
    const char* native_consumer = getenv("MAG_NATIVE_CONSUMER");
    if (native_consumer && !strcmp(native_consumer, "1")) {
        g_native_consumer = cellGcm_native_consumer_enabled() != 0;
        fprintf(stderr, "[mag] native consumer wake=%s idle_timeout_ms=4\n",
                g_native_consumer ? "enabled" : "unavailable; timed fallback");
    }
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    int rc;
#ifdef __APPLE__
    const char* headless = getenv("PS3RECOMP_METAL_HEADLESS");
    if (!headless || !*headless || *headless == '0') {
        rc = mag_run_with_main_queue([](void* opaque) {
            return ppu_run(*static_cast<uint32_t*>(opaque), STACK_TOP);
        }, &entry);
    } else
#endif
        rc = ppu_run(entry, STACK_TOP);
    mag_render_diagnostics_stop();
    mag_particle_cohort_ablation_vm_end();
    mag_native_depth_vm_shutdown();
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
