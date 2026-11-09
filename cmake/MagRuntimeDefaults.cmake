# Runtime configuration defaults for every host, applied before the runtime subdirectory.
#
# PS3RECOMP_VM_FAST_ACCESSORS: the pinned PPU lifter calls the out-of-line guest
# accessors for every load and store. Their full bodies run range checks, a dozen
# getenv-guarded probes, thread-local spin trackers and, for stores, the coherence
# path. With the option, an access that needs none of that (no diagnostic armed, an
# ordinary mapped address, and for a store no watch and no coherence-marked line) does
# the plain big-endian load or store. Everything else still takes the full body.
#
# Measured on macOS in Training: about 21% less main PPU thread busy time, 15% less
# PPU busy time overall and about 80% fewer thread-local lookups, with frame rate
# unchanged, since the main thread mostly waits on SPU work.
#
# A cache entry the user sets explicitly still wins. A build directory configured
# before this default keeps its cached OFF until it is reconfigured with the option.
include_guard(GLOBAL)
set(PS3RECOMP_VM_FAST_ACCESSORS ON CACHE BOOL
    "Give the out-of-line PPU guest accessors a fast path for accesses that need no probe, coherency or MMIO handling")
