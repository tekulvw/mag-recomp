# Runtime build options mag-recomp turns on for every host, applied before the runtime
# subdirectory. A cache entry the user sets explicitly still wins.
#
# PS3RECOMP_VM_FAST_ACCESSORS: the out-of-line PPU guest accessors take a plain
# big-endian load or store when the access needs no probe, coherence or MMIO work.
# Side-effect addresses (the SPU problem-state window, the GCM ref-poll and put-wake
# words, the null page) and coherence-marked lines keep the full path. On Windows the
# main PPU thread's running time fell from 6.15 to 4.70 ms per frame and Training
# guest fps rose 3.6% (44.29 to 45.91, three interleaved rounds).
include_guard(GLOBAL)
set(PS3RECOMP_VM_FAST_ACCESSORS ON CACHE BOOL
    "Plain big-endian PPU guest accesses when no probe, coherence or MMIO work is needed")
