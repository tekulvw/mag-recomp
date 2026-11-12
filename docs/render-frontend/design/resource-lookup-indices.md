# Optional resource lookup indices

The native scene validator repeatedly finds declared resources by their complete
buffer token. The Vulkan backend repeatedly finds live buffers and pipelines
while validating, preparing descriptors and recording commands. Two independent
switches replace those linear searches with bounded, local indices:

| Switch | Effect | Default |
| --- | --- | --- |
| `MAG_RENDER_SCENE_RESOURCE_INDEX=1` | Use a per-validation resource index for membership and duplicate checks. | Off |
| `MAG_RENDER_VULKAN_LOOKUP_INDEX=1` | Use context-local buffer and pipeline indices in the Vulkan backend. | Off |
| `MAG_RENDER_VULKAN_LOOKUP_PROBES=1` | Report bounded cumulative backend lookup counts and probes. This is a diagnostic, separate from either index. | Off |

Only the exact value `1` enables each switch. This change enables no native
rendering selector and changes no SDK pin, native API or descriptor allocation
policy. Other backends can use the shared validator index without the Vulkan
index.

The shared index preserves full-width token equality, duplicate/error priority,
registry callback order, budgets and read/write usage. Its 8,192 `uint16_t` slots
reserve 16 KiB of local stack; clearing occurs only when enabled. At most 4,096
admitted resources keep the table at or below half load. State lasts only for
validation, under the existing immutable-scene contract.

The Vulkan indices use 1,024 buckets per object type and separate intrusive
links, retaining the original disposal lists, token checks and GPU lifetimes.
Only successful creation publishes an entry. Public release rejects further
lookups; final disposal removes entries before free or reuse, after existing
reader/writer and pipeline pins retire. On a 64-bit host the tables use 16,384
bytes per context, full lookup state uses 16,472 bytes, and each object gains one
eight-byte link. There is no separate index allocation.

## Combined Linux application comparison

A same-executable exploratory comparison on Linux with a Ryzen 7 7800X3D,
Radeon RX 7900 XTX and RADV changed exactly the two index switches together:

| Control | Disabled arm | Enabled arm |
| --- | --- | --- |
| `MAG_RENDER_VULKAN_LOOKUP_INDEX` | `0` | `1` |
| `MAG_RENDER_SCENE_RESOURCE_INDEX` | `0` | `1` |
| Backend lookup probes | `0` | `0` |
| Busy diagnostics | `0` | `0` |
| Owner-span diagnostics | `0` | `0` |
| Focused measurement window | 55 seconds | 55 seconds |

All other controls and the executable were held constant. Each arm retained
focus for the full window, made progress throughout it and had no trace drops,
late plateau or omitted work. Intended and admitted work matched, and counted
native updates had matching genuine completion acknowledgements.

| Measurement | Disabled | Enabled | Change |
| --- | ---: | ---: | ---: |
| Median request interval | 36.381565 ms | 33.5582505 ms | 7.76029% lower |
| Median Main-poll overlap | 21.692198 ms | 18.9717055 ms | 12.54134% lower |
| Completed native updates | 1,490 | 1,611 | Update rate 8.1205% higher |
| Median admitted draws per update | 1,749 | 1,729 | 1.1435% lower |
| Completed draw-work per second | — | — | 6.9149% higher |

Normalizing request cost by the admitted draws gives a 6.6933% reduction. The
whole-window completion and draw-work rates qualify the request medians; the
result does not depend only on a short trace interval.

This measures the **combined** change and does not isolate either index's
application contribution. The native frontend still has world stippling and
explicit color-only/depth-bounds approximations. These request and work metrics
are not physical presentation FPS or full rendering acceptance.

A separate fresh, same-executable normal-settings reference maintained healthy
logical progress throughout its own 55-second window:

| Request interval | Normal settings | Indexed native prototype |
| --- | ---: | ---: |
| Median | 33.8924835 ms | 33.5582505 ms |
| Mean | 34.3830301 ms | 33.9335942 ms |
| 95th percentile | 37.537106 ms | 37.856271 ms |

The native median was 0.9862% lower and its mean 1.307% lower, while its 95th
percentile was slightly worse. This is near parity with a small, single-run,
**provisional** advantage; incomplete fidelity and this tail result prevent a
conclusive product-performance or FPS claim. Raw game evidence, payloads and
captures remain private; only aggregate results are documented here.

## Synthetic CPU checks and component microbenchmark

The 32-case authored validator fixture checks status, complete usage/budgets and
callback order across both routes, including identity, collision, earlier-error,
full-budget and producer/consumer cases. The Vulkan fixture checks exact objects,
stale handles, collisions, versions, public release, final reader/writer
retirement and probe counts. Neither requires game content or creates a GPU
device.

A separate macOS **CPU component microbenchmark** of the shared validator used
408 synthetic resources, 1,700 draws, constant-time registry callbacks and 128
validations per arm in the same binary. It measured CPU `clock()` time of
0.372289 ms per validation with the shared index off and 0.033680 ms with it on.
This isolates synthetic validation work; it is neither an application timing
comparison nor an attribution of the combined Linux result above. It does not
measure the Vulkan index's application contribution.

Run the focused shared-validator check, or explicitly request its component
microbenchmark:

```sh
python3 tools/test_render_scene_resource_index.py
python3 tools/test_render_scene_resource_index.py --bench
```

For the focused Vulkan CPU fixture on Linux with Vulkan headers available (no
Vulkan loader or GPU is required), run these commands in Bash from the repository
root:

```bash
native=crates/mag-render-native/native
fixture=crates/mag-render-native/tests/native/test_vulkan_scene_extended.c
mkdir -p build/lookup-cpu
cc -std=c11 -O2 -Wall -Wextra -Werror \
  -I"$native" -I"${VULKAN_INCLUDE:-/usr/include}" \
  "$fixture" \
  "$native"/mag_render_{scene_validate,stencil_validate,image_validate,raster_validate,sampler_validate,sampler_registry}.c \
  -lm -o build/lookup-cpu/vulkan-lookup
MAG_RENDER_SCENE_RESOURCE_INDEX=1 build/lookup-cpu/vulkan-lookup
```

The Vulkan fixture selects and checks its own backend index/probe arms. The last
command also enables the shared index for its existing scene-encoding cases.
On macOS, use an explicit Vulkan header directory and add `-Wl,-dead_strip` for
the fixture's unused host integration entry. The shared-validator helper also
supports the Windows `clang-cl` toolchain. Local CPU results do not establish
Windows or macOS application performance.

Backend probe reports are cumulative scalar diagnostics: first two accepted
scenes, every 120th scene and close. Keep probes off in both timing arms.
Hash collisions retain bounded worst cases; family bookkeeping and disposal-list
unlinking retain their original costs.
