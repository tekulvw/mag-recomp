# ps3recomp switches mag-recomp uses

The ps3recomp runtime keeps most of its behaviour behind opt-in switches: environment variables
it reads at run time and CMake options it reads at configure time. This page lists every one
mag-recomp sets, with the value, the platforms, the file that sets it, the reason and how to turn
it off for a comparison. It applies to the runtime pinned in
[`profiles/framework.json`](../profiles/framework.json) (`mag-recomp/runtime-2026.11.09`, `767c5311`).

`tools/check_ps3recomp_flags.py` extracts the switches from the sources and fails when this page
is missing one, lists one nothing sets any more, or gives a value or file the sources no longer
have. `tests/test_check_ps3recomp_flags.py` runs it, so the test suite fails too.

```sh
python3 tools/check_ps3recomp_flags.py           # this page against the sources
python3 tools/check_ps3recomp_flags.py --list    # every switch found, its value and file
python3 tools/check_ps3recomp_flags.py --runtime local/ps3recomp-runtime   # also against a runtime checkout
```

When you add, remove or change a switch mag-recomp sets, update the matching row here. When you
move the runtime pin, run the check with `--runtime` against the new checkout, and look through
the runtime's new opt-in switches for ones worth an A/B run.

What counts: environment names the runtime owns (`PS3RECOMP_`, `PS3_`, `SPU_`, `SPURS_`, `RSX_`,
`PPU_`, `GCM_`, and `MAG_NATIVE_CONSUMER`, which the runtime reads), and the runtime's CMake cache
entries and compile definitions. `MAG_*` variables are mag-recomp's own and are not listed. The
check scans `src/`, `tools/`, `installer/mag_installer/`, `CMakeLists.txt` and `cmake/`; tests
and CI scripts are out of scope.

## How the defaults apply and how to turn one off

- **Compiled defaults** (`src/mag_runtime_defaults.h`). `mag_apply_runtime_defaults()` sets each
  variable only when it is absent, so an explicit value wins, `0` included. It runs from a
  constructor in `src/mag_runtime_defaults_early.cpp` (before the runtime's own constructors read
  their switches) and again from `main()`. The Windows Vulkan and macOS blocks in `src/main.cpp`
  follow the same rule.
- **Packaged players** start the game from an environment they build themselves: macOS from an
  empty one, Linux from a fixed map, Windows after removing inherited `MAG_`, `PS3_`,
  `PS3RECOMP_`, `RSX_` and `SPU_` variables. `tools/boot.py` also removes inherited runtime
  variables. To compare a switch, launch `MagRecomp` directly with the variable set.
- **CMake options** set in mag-recomp's CMake files are cache entries: `-DNAME=OFF` on the first
  configure (or in an existing cache) wins. Options the installer recipe passes are arguments:
  drop them from the recipe, or override them with the recipe's `cmake_args`.

## Compiled runtime defaults

Every launch gets these unless the variable is already set. The RSX, PPU and SPU set from #136 is
the one every accepted hosted run used: it took an installer-built Linux client from 4.6 to 21.5
guest fps (3.7 to 13.1 guest flips/s). Applied in `main()` only, the deferred-report and
early-release switches came too late for the runtime's constructor and Linux reached 11.5 fps;
applied before the constructors it reached 21.0.

| Switch | Kind | Value | Platforms | Set in | Why | Opt out |
|---|---|---|---|---|---|---|
| `SPU_DISPATCH_CACHE` | env | `1` | all | `src/mag_runtime_defaults.h` | Per-thread cache of lifted SPU function lookups for indirect dispatch (#15). | `0`; `shadow` checks every hit against the full resolver |
| `SPU_MFC_LOOKUP_CACHE` | env | `1` | all | `src/mag_runtime_defaults.h` | Per-thread cache of the last MFC engine lookup (#15). | `0` |
| `SPU_FLOAT_RZ` | env | `1` | all (takes effect on arm64 only) | `src/mag_runtime_defaults.h` | SPU code runs with round-toward-zero and flush-to-zero, so the single-precision helpers take their native form (#15). | `0` |
| `PS3_VERBOSE` | env | `0` | all | `src/mag_runtime_defaults.h` | Quiet per-event logging. Without it a redirected stderr turns logging on, and one shared log lock changes the title's timing (#15). | `1` |
| `PS3RECOMP_GCM_INLINE_NO_GPU_WAIT` | env | `1` | all | `src/mag_runtime_defaults.h` | MAG patches fragment-program constants through NV308A inline data about 1,100 times per menu frame; draining the GPU before each held the menu near 10 fps. Report ordering is kept. macOS only before #136. | `0` |
| `MAG_NATIVE_CONSUMER` | env | `1` | all | `src/mag_runtime_defaults.h` | Wakes the FIFO drain when the guest moves the put pointer instead of polling every 4 ms; SPU jobs waiting on RSX labels no longer sit out the poll (Training about 99 to 93 ms per update). macOS only before #136. | `0` |
| `PS3RECOMP_RSX_DEFERRED_REPORTS` | env | `1` | all | `src/mag_runtime_defaults.h` | RSX reports and labels are queued and completed by the FIFO consumer once the GPU finishes, instead of inline; early label release needs it. Logs `[deferred reports] enabled` (#136). | `0` |
| `PS3RECOMP_RSX_DEFERRED_REPORT_CAPACITY` | env | `4096` | all | `src/mag_runtime_defaults.h` | How many deferred records may queue before a push waits (runtime default 1024, at most 8192); the accepted runs saw no capacity waits at 4096 (#136). | another count |
| `PS3RECOMP_RSX_EARLY_LABEL_RELEASE` | env | `1` | all | `src/mag_runtime_defaults.h` | Labels and synthesized reports publish at their FIFO position instead of after GPU completion, which is still verified; jobs waiting on labels resume sooner. Logs `[early release]` (#136). | `0` |
| `PS3RECOMP_RSX_EARLY_FIFO_SYNC` | env | `1` | all | `src/mag_runtime_defaults.h` | SET_REFERENCE, semaphore acquire and NV0039 transfers stop waiting for the GPU. Needs early label release (#136). | `0` |
| `PS3RECOMP_RSX_GPU_DEPTH_VIEW` | env | `1` | all | `src/mag_runtime_defaults.h` | Depth reads come from the GPU's depth view instead of a store to guest memory; a backend without one logs it and keeps the store (#136). | `0` |
| `PS3RECOMP_PPU_STORE_FASTPATH` | env | `1` | all | `src/mag_runtime_defaults.h` | Plain PPU stores to lines no reservation is tracking skip the coherence path; logs `[spu-coh] PPU store fast path on` (#136). | `0` |
| `SPU_REGION_FRAMED` | env | `1` | all | `src/mag_runtime_defaults.h` | A lifted SPU region may also step locally inside a drain-owned frame, instead of falling back to the drain for every framed transfer (#136). | `0` |
| `SPU_INTERP_REJOIN_PREFILTER` | env | `1` | all | `src/mag_runtime_defaults.h` | The SPU interpreter asks a cheap "may be a lifted entry" filter before each full lookup to rejoin lifted code (#136). | `0` |
| `PS3RECOMP_SPURS_WORKLOAD_STACK_ARG9` | env | `1` | all | `src/mag_runtime_defaults.h` | The SPURS workload attribute call's ninth argument is read from the caller's stack, where the PPU ABI passes it (#136). | `0` |
| `PS3RECOMP_VK_BATCHED_ENGINE` | env | `1` | Linux; Windows Vulkan builds | `src/mag_runtime_defaults.h`, `src/main.cpp` | Batches the Vulkan backend's engine recording (#136 on Linux, #124 on Windows). | `0` |
| `PS3RECOMP_VK_WINDOW` | env | `1` | Linux players; Windows Vulkan builds | `src/main.cpp`, `installer/mag_installer/native_package_linux.py` | Opens the Vulkan backend's window; without it the backend stays headless and the title never appears (#124). | Windows: `0`. Linux players: set by the supervisor |
| `PS3RECOMP_VK_GUEST_PROGRAMS` | env | `1` | Linux players; Windows Vulkan builds | `src/main.cpp`, `installer/mag_installer/native_package_linux.py` | Draws with the register-file engine and the guest's own shader programs (through glslang) instead of the fixed-function path (#124). | Windows: `0`. Linux players: set by the supervisor |
| `PS3RECOMP_METAL_FPS` | env | `1` | macOS | `src/main.cpp` | Native frame-rate diagnostics in the Metal window title. | `0` keeps the plain title |

## Packaged players and launchers

The supervisors in `installer/mag_installer/native_package*.py` set these for both the launcher
and the engine. `tools/boot.py` and `tools/handoff.py` set the same ones for diagnostic runs.

| Switch | Kind | Value | Platforms | Set in | Why | Opt out |
|---|---|---|---|---|---|---|
| `PS3RECOMP_SAIL_TIMED_THX` | env | `1` | builds with the movie decoder | `installer/mag_installer/native_package.py`, `tools/boot.py` | Timed Sail pictures. With the decoder but without the three movie switches each movie opens and closes at once and the game waits after the legal screen (#104, #131). | none in players; a build without the decoder never gets it |
| `PS3RECOMP_SAIL_ATRAC_AUDIO` | env | `1` | builds with the movie decoder | `installer/mag_installer/native_package.py`, `tools/boot.py` | Movie ATRAC audio (#104, #131). | as above |
| `PS3RECOMP_GCM_CHECKED_REF` | env | `1` | builds with the movie decoder | `installer/mag_installer/native_package.py`, `tools/boot.py` | Checked GCM references, which timed playback needs before it opens a movie (#104, #131). | as above |
| `PS3_VFS_ROOT` | env | the game files, or the player's server overlay | all | `installer/mag_installer/native_package.py`, `installer/mag_installer/native_package_linux.py`, `installer/mag_installer/native_package_windows.py`, `src/player_settings_portable.cpp`, `src/player_settings_runtime.mm`, `tools/boot.py` | Where the runtime finds `PS3_GAME`. With a server chosen, the player settings point it at a private overlay with the server's address (#11, #125). | required |
| `PS3_GAMEDATA_ROOT` | env | `home/gamedata` under the installation | all players | `installer/mag_installer/native_package.py`, `installer/mag_installer/native_package_linux.py`, `installer/mag_installer/native_package_windows.py` | Saves and trophies live in the installation, not the launch's log folder, so progress survives every launch. | required |
| `PS3_BOOT_PATH` | env | `/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN` (launcher), `/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self` (engine) | all | `installer/mag_installer/native_package.py`, `installer/mag_installer/native_package_linux.py`, `installer/mag_installer/native_package_windows.py`, `tools/boot.py`, `tools/handoff.py` | The guest path the title sees for its own image; the engine stage is started after the launcher hands off. | required |
| `PS3_HLE_UNRESOLVED` | env | `halt` | all | `installer/mag_installer/native_package.py`, `installer/mag_installer/native_package_linux.py`, `installer/mag_installer/native_package_windows.py`, `tools/boot.py` | An unimplemented firmware import stops the game with its NID logged (exit 88) instead of carrying on with a made-up result. | none in players |
| `PS3_SYSCALL_UNRESOLVED` | env | `halt` | all | `installer/mag_installer/native_package.py`, `installer/mag_installer/native_package_linux.py`, `installer/mag_installer/native_package_windows.py`, `tools/boot.py` | The same for an unimplemented system call. | none in players |

## Player settings

The settings window (macOS) and settings file (all platforms) turn into these at launch;
see [player-settings.md](player-settings.md).

| Switch | Kind | Value | Platforms | Set in | Why | Opt out |
|---|---|---|---|---|---|---|
| `PS3_NP_ONLINE_ID` | env | the player name | all | `src/player_settings_portable.cpp`, `src/player_settings_runtime.mm`, `tools/boot.py` | The runtime's online ID: the server signs players in by this name (#125). | leave the player name empty |
| `PS3RECOMP_OSK_INITIAL_TEXT` | env | the player name | all | `src/player_settings_portable.cpp`, `src/player_settings_runtime.mm`, `tools/boot.py` | The sign-in keyboard opens with the name typed in, so the player only confirms it (#125). | leave the player name empty |
| `PS3_NET_ONLINE` | env | `1` | all | `src/player_settings_portable.cpp`, `src/player_settings_runtime.mm`, `tools/boot.py` | The network reports a live connection, so the title takes its online path; set only when the server overlay is ready, otherwise the game plays offline (#11, #125). | choose no server |
| `PS3_AA` | env | the anti-aliasing setting (`fxaa` or off) | macOS | `src/player_settings_runtime.mm` | FXAA on the display buffer; only the Metal backend reads it (#11). | the setting |

## Build options

| Switch | Kind | Value | Platforms | Set in | Why | Opt out |
|---|---|---|---|---|---|---|
| `PS3RECOMP_RSX_VULKAN` | CMake | `ON` | Linux; Windows with the managed prefix | `installer/mag_installer/native_recipe.py` | Linux and Windows players present through the runtime's Vulkan backend (#128 for Windows). | omit it: Windows then builds D3D12 |
| `PS3RECOMP_VULKAN_INCLUDE_DIR` | CMake | the managed prefix's `include` | Windows | `installer/mag_installer/native_recipe.py` | Vulkan-Headers come from the managed prefix; the loader comes from the GPU driver (#128). | needed with the Vulkan backend |
| `PS3RECOMP_SAIL_FFMPEG` | CMake | `ON` | all (Windows when FFmpeg is configured) | `installer/mag_installer/native_recipe.py` | Links the movie decoder; the receipt records it and the movie switches above follow (#104). | omit it: no movies, and no movie switches |
| `PS3RECOMP_FFMPEG_RUNTIME` | CMake | `ON` | Linux; Windows with FFmpeg | `installer/mag_installer/native_recipe.py` | Loads FFmpeg at run time instead of linking it. | omit it |
| `PS3RECOMP_FFMPEG_INCLUDE_DIR` | CMake | the FFmpeg headers | all with FFmpeg | `installer/mag_installer/native_recipe.py` | The pinned FFmpeg's headers. | with `PS3RECOMP_SAIL_FFMPEG` |
| `PS3RECOMP_FFMPEG_AVCODEC_LIBRARY` | CMake | the FFmpeg library | all with FFmpeg | `installer/mag_installer/native_recipe.py` | The pinned FFmpeg libraries, not whatever the system has. | with `PS3RECOMP_SAIL_FFMPEG` |
| `PS3RECOMP_FFMPEG_AVFORMAT_LIBRARY` | CMake | the FFmpeg library | all with FFmpeg | `installer/mag_installer/native_recipe.py` | As above. | with `PS3RECOMP_SAIL_FFMPEG` |
| `PS3RECOMP_FFMPEG_AVUTIL_LIBRARY` | CMake | the FFmpeg library | all with FFmpeg | `installer/mag_installer/native_recipe.py` | As above. | with `PS3RECOMP_SAIL_FFMPEG` |
| `PS3RECOMP_FFMPEG_SWSCALE_LIBRARY` | CMake | the FFmpeg library | all with FFmpeg | `installer/mag_installer/native_recipe.py` | As above. | with `PS3RECOMP_SAIL_FFMPEG` |
| `PS3RECOMP_SPU_DECODE_CACHE_HEAP` | CMake | `ON` | Linux | `cmake/MagLinuxRuntime.cmake` | glibc carves static TLS out of each thread's stack. The 2 MiB thread-local SPU decode cache made SPURS workers with 1 MiB stacks fail to start, so the launcher could not decompress its archive; the cache is allocated per thread on first use instead (#134). | `-DPS3RECOMP_SPU_DECODE_CACHE_HEAP=OFF` |
| `SPU_USE_DISPATCH_MT` | define | defined | all | `CMakeLists.txt` | The lifted SPU sources and `spu_dispatch_mt.c` take the musttail indirect-branch path. It must be built by clang in this project, or the tail call becomes a plain call and a guest loop walks the thread stack off its guard page. | none |

## Developer tools

Diagnostic switches that `tools/` sets for its own runs. Packaged players never set them.

| Switch | Kind | Value | Platforms | Set in | Why | Opt out |
|---|---|---|---|---|---|---|
| `PS3RECOMP_NP_LOCAL_TICKET` | env | `1` | all | `tools/boot.py` | A local NP ticket for a network profile that asks for one. | the profile |
| `PS3_HLE_TRACE` | env | `1` | all | `tools/boot.py` | Firmware import trace in diagnostic boots. | none |
| `PS3_SCTRACE` | env | `1` or `fail` | all | `tools/boot.py` | System call trace: every call, or only failures. | `--syscall-trace` |
| `PS3_NET_TRACE` | env | `1` | all | `tools/boot.py` | Socket trace when a network profile uses host sockets. | none |
| `PS3RECOMP_METAL_FRAME_DUMP` | env | a file in the run's output folder | macOS | `tools/boot.py`, `tools/handoff.py` | The presented frame for the run's receipt (kept under the ignored output folder). | none |
| `PS3RECOMP_METAL_FRAME_DUMP_FIRST_NONBLACK` | env | `1` | macOS | `tools/boot.py` | Dumps the first non-black frame instead of the last. | `--first-nonblack` |
| `PS3RECOMP_METAL_HEADLESS` | env | `1` | macOS | `tools/boot.py` | Runs without a window unless the run asks to be visible. | `--visible` |
| `PS3RECOMP_VK_DEVICE` | env | the selected GPU index | Linux, Windows | `tools/check_render_frontend_native.py`, `tools/check_render_frontend_sdk.py` | The GPU checks pick a device. | `--gpu-index` |

## Set but not read by the pinned runtime

| Switch | Kind | Value | Platforms | Set in | Why | Opt out |
|---|---|---|---|---|---|---|
| `PS3RECOMP_STRICT_HLE` | env | `1` | all | `tools/probe_edge_queue.py` | The edge-queue probe asks for strict HLE. The pinned runtime no longer reads this name; strict handling comes from `PS3_HLE_UNRESOLVED` and `PS3_SYSCALL_UNRESOLVED`. | no effect |

## Runtime defaults mag-recomp relies on

Switches mag-recomp leaves at the runtime's default and depends on. The check fails if one of them
starts being set here; move its row up when that happens.

| Switch | Kind | Runtime default | Platforms | Why mag-recomp relies on it | Opt out |
|---|---|---|---|---|---|
| `PS3RECOMP_NATIVE_OSK_SDL` | CMake | `ON` | Linux | The SDL2 text-entry dialog for the native on-screen keyboard: players sign in without automation (runtime pin of #128 and #134). | `-DPS3RECOMP_NATIVE_OSK_SDL=OFF` |
| `PS3RECOMP_PAD_SDL_KEYBOARD` | CMake | `ON` | Linux | The game window's keyboard drives controller port 0 until a controller is connected (README, "For players"). The opt-in controls adapter's SDL keyboard/mouse capture runs from that keyboard's per-pump sample, so `MAG_GAME_CONTROLS` refuses to configure with it off (`cmake/MagControls.cmake`). | `PAD_NO_KEYBOARD=1` at run time, or `-DPS3RECOMP_PAD_SDL_KEYBOARD=OFF` |
| `PS3RECOMP_NATIVE_OSK_WINDOWS` | CMake | `ON` | Windows | Native Windows text input for the on-screen keyboard. The opt-in controls adapter's Win32 keyboard/mouse capture runs from the window pump calls this host shares, so `MAG_GAME_CONTROLS` refuses to configure with it off (`cmake/MagControls.cmake`). | `-DPS3RECOMP_NATIVE_OSK_WINDOWS=OFF` |

No runtime switch is documented in mag-recomp's code as one that must stay off. Diagnostics stay
off by default (#136). Other opt-in runtime switches become defaults here only after an A/B run
shows they help.
