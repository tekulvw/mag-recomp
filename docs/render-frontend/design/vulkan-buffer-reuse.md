# Retired Vulkan buffer reuse

`MAG_RENDER_VULKAN_BUFFER_REUSE=1` enables a physical buffer pool in the Vulkan
backend. It is off by default and does not change the native buffer API or the
logical contents of a newly created buffer.

An entry can enter the pool only after its public reference and all GPU readers
and writers have retired. Reuse requires the same device context, exact logical
byte length and the backend's fixed buffer usage. Every logical creation still
gets its normal identity/version and performs the complete initial-byte copy or
zero fill. An old logical handle cannot access the reused allocation.

The pool retains at most 32 entries and 128 MiB of actual Vulkan allocation bytes.
It evicts the oldest free entries to stay within those bounds and drains on close,
device failure or allocation failure. Other backends are unaffected. This is
separate from the existing immutable upload cache and completion-released input
arenas; it also covers short-lived compute output allocations.

With `MAG_RENDER_NATIVE_PHASE_TIMING=1`, the backend prints bounded periodic pool
hit, miss, retained-byte and eviction counters. Keep this diagnostic setting the
same in both arms of a timing comparison.

## Observed benefit

A Linux/RADV experiment on a Ryzen 7 7800X3D and Radeon RX 7900 XTX compared one
experimental executable with this setting disabled and enabled. The setting was
the only intentional control difference. Both arms used a 20-second settle and a
55-second focused measurement window, with a 25-second request trace and no trace
drops. Native phase timing was disabled in both measured arms.

| Measurement | Disabled | Enabled | Reduction |
| --- | ---: | ---: | ---: |
| Median guest-request interval | 49.456 ms | 47.221 ms | 4.52% |
| Median Main-poll overlap | 33.809 ms | 31.561 ms | 6.65% |
| Completed native updates in the window | 1,111 | 1,157 | — |
| Median admitted draw count per update | 1,717 | 1,718 | — |

Every counted update had a matching successful native completion, with no omitted
items or typed native failures. The enabled arm's draw count was 0.058% higher;
normalizing request cost by the admitted draw count gives a 4.58% reduction.

A separate short diagnostic run confirmed 7,392 pool hits and 408 misses, ending
with 28 retained entries using 60,597,760 allocation bytes and 14 evictions. It
completed 534 native updates without a typed failure. Its buffer allocation,
upload and zero-fill phase measured 1.969 ms median across the bounded diagnostic
samples. That phase is nested within submission; it must not be added to the
whole submission timing or treated as another measured frame-time comparison.

This is one paired exploratory result for the native path, not a comparison with
the original renderer, a physical presentation FPS measurement or a full fidelity
acceptance result. The prototype renders the world and weapon but still has color
artifacts and explicit raster approximations. No Windows or macOS performance
benefit is established by this result. Raw game evidence remains private.

The synthetic Vulkan CPU fixture checks default-off behavior, allocation reuse,
initialization, live/in-flight exclusion, stale handles, pool bounds, eviction,
close and failure paths without requiring game content or a GPU.
