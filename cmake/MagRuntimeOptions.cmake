# Runtime build options mag-recomp turns on, applied before the runtime subdirectory.
# A cache entry the user sets explicitly still wins.
#
# PS3RECOMP_VM_FAST_ACCESSORS (Windows and macOS): the out-of-line PPU guest accessors take a
# plain big-endian load or store when the access needs no probe, coherence or MMIO
# work. Side-effect addresses (the SPU problem-state window, the GCM ref-poll and
# put-wake words, the null page) and coherence-marked lines keep the full path.
# On Windows the main PPU thread's running time fell from 6.15 to 4.70 ms per frame
# and Training guest fps rose 3.6% (44.29 to 45.91, three interleaved rounds). On
# macOS Training guest fps rose 5.2% (76.90 to 80.94, three interleaved rounds, on main with
# the SPU put kick and native Edge geometry). On Linux it cut main-thread CPU about 30% but
# guest fps fell 3.8% over six rounds (36.45 to 35.07), so it stays off there.
include_guard(GLOBAL)
if(WIN32 OR APPLE)
    set(PS3RECOMP_VM_FAST_ACCESSORS ON CACHE BOOL
        "Plain big-endian PPU guest accesses when no probe, coherence or MMIO work is needed")
endif()

# PS3RECOMP_PAD_USE_SDL2 (Windows): cellPad reads controllers through SDL2's GameController API, as
# it does on Linux and macOS, instead of XInput. XInput sees Xbox-class pads only, so a DualSense or
# DualShock 4 was ignored unless Steam Input or DS4Windows wrapped it; SDL reads those through its
# HIDAPI drivers (rumble included) as well as XInput pads. The build needs SDL2's CMake package
# (SDL2_DIR; the managed installer passes the official SDL2-devel VC package) and ships SDL2.dll
# beside the executables. OFF restores the XInput backend. Not a performance switch: no A/B applies.
if(WIN32)
    set(PS3RECOMP_PAD_USE_SDL2 ON CACHE BOOL "Read Windows controllers through SDL2 instead of XInput")
endif()

# SPU_FLOAT_X86_RZ: on x86 builds that target AVX-512 F/VL/DQ (x86-64-v4, chosen automatically
# where the CPU has AVX-512), SPU single-precision add, subtract, multiply and fused ops run as one
# native instruction with AVX-512 embedded round-toward-zero instead of the exact double-precision
# emulation; extended-range operands and possible overflows still take the exact path, and the
# results are bit-identical (447M-lane exhaustive test). Builds without AVX-512 keep the exact
# forms whatever the define says. Training guest fps: Windows +1.5% (45.75 to 46.44), Linux
# +0.8% (40.99 to 41.30), three interleaved rounds each.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    add_compile_definitions(SPU_FLOAT_X86_RZ=1)
endif()
