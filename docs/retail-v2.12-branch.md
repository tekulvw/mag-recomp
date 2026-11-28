# Retail 2.12 branch foundation

Development of BCUS98110 2.12 takes place on `codex/retail-v2.12`. The intended
outcome is a full switch once the new build works. Permanent player-facing
dual-version support is not the goal. Temporary target selection keeps the
working v1.0 baseline separate while the new port is prepared and audited.

The supported default remains retail v1.0. The 2.12 branch now has an explicit
experimental native build, with separate original-code modules and source-bound
launcher adapters. Its launcher and engine compile and link; boot, character
creation and login acceptance remain unproven. Do not apply v1 sign-in, controls,
native SPU/Edge or renderer adapters to the new images.

## Inputs and isolated paths

First [assemble and verify the complete installation](retail-v2.12-preparation.md).
`tools/targets.py` pins the original SELF/ELF identity pairs for both images.
Supply independently decrypted copies using the same interpreter and environment
as the rest of the repository. Decryption and all generated content remain private.

| Working data | Existing default | Explicit 2.12 |
| --- | --- | --- |
| Machine image profile | `profiles/retail-v1.0.json` | `profiles/retail-v2.12.json` |
| Inventory | `analysis/retail-v1.0/` | `analysis/retail-v2.12/` |
| PPU source | `generated/retail-v1.0/` | `generated/retail-v2.12/` |
| Default copied USRDIR | `local/retail-v1.0/PS3_GAME/USRDIR` | `local/retail-v2.12/dev_hdd0/game/BCUS98110/USRDIR` |

`MAG_WORKSPACE` relocates these working paths as before. Each copied USRDIR must
contain original `EBOOT.BIN`, `bin/mage_g.self`, and independently decrypted
`EBOOT.elf`, `bin/mage_g.elf`. Preparation verifies these files; it does not perform
decryption or copy them. It requires the complete launcher/engine pair for 2.12.

```sh
.venv/bin/python tools/prepare_inputs.py --target retail-v2.12 \
  --source-root '/path/to/installation/dev_hdd0/game/BCUS98110/USRDIR' \
  --installation '/path/to/installation'
```

`--installation` additionally verifies all 375 installation files and binds the
machine profile to the receipt. It is optional for a binary-only offline audit.
Use `--local-root` only when the copied SELF/ELF directory is elsewhere. Unknown
targets, mixed executable pairs and outputs overlapping the other target's
canonical roots are rejected, including resolved symlink aliases.

## Pinned tools and PPU generation

Fetch the analysis pin and the separately selected 2.12 PPU lifter:

```sh
.venv/bin/python tools/fetch_ps3recomp.py --role runtime --role lifter-spu --role lifter-ppu-v212
.venv/bin/python tools/inventory.py --target retail-v2.12 \
  --sdk local/ps3recomp-lifter-5244db9
.venv/bin/python tools/lift.py --target retail-v2.12 --image EBOOT \
  --sdk local/ps3recomp-lifter-v212
.venv/bin/python tools/lift.py --target retail-v2.12 --image mage_g \
  --sdk local/ps3recomp-lifter-v212
```

Inventory uses the clean pinned analysis SDK. The 2.12 generator uses the clean
optional `lifter-ppu-v212` role from `profiles/framework.json`; the existing v1 generation defaults
stay unchanged. Inventory and generation require fresh output directories for
2.12. An explicit `--output` may select another private directory but may not
overlap v1 working roots. Existing evidence is never automatically deleted.

Target and manifest hashes bind each inventory and receipt. The 2.12 path rejects
v1 nonlocal-control-flow profiles and lift hooks, including `MAG_LIFT_HOOKS`.
Unsupported generated PPU instructions and unresolved targets retain explicit
execution guards. The normal CMake build defaults to v1.0;
`-DMAG_RETAIL_TARGET=retail-v2.12` also requires the explicit
`-DMAG_V212_EXPERIMENTAL_BUILD=ON` gate. Unsupported v1 adapter switches are rejected.

## Experimental native preparation

Prepare the original SPU programs and the independently decrypted primary PACE
module into fresh ignored directories:

```sh
.venv/bin/python tools/v212_spu_prepare.py \
  --sdk local/ps3recomp-runtime --output generated/retail-v2.12/spu-original
.venv/bin/python tools/v212_pace_prepare.py \
  --source /path/to/decrypted/libpace_g.prx \
  --output generated/retail-v2.12/pace-native
.venv/bin/python tools/firmware_lift.py --output generated/retail-v2.12/firmware
```

PACE preparation validates the original module identity and exact lifter sources,
relocates it into a non-overlapping reservation, and preserves its real exports
and module start/stop functions. The alternative `libpacePrx_g` image is retained
for investigation and is rejected as a substitute by native build validation.
The generated source and embedded module image remain private.

Configure a separate directory for each image, replacing `EBOOT` with `mage_g`
for the engine:

```sh
cmake -S . -B build/retail-v2.12/EBOOT -G Ninja \
  -DMAG_RETAIL_TARGET=retail-v2.12 -DMAG_V212_EXPERIMENTAL_BUILD=ON \
  -DMAG_IMAGE=EBOOT -DPS3RECOMP_DIR="$PWD/local/ps3recomp-runtime" \
  -DRECOMP_DIR="$PWD/generated/retail-v2.12/EBOOT" \
  -DMAG_FIRMWARE_DIR="$PWD/generated/retail-v2.12/firmware"
cmake --build build/retail-v2.12/EBOOT --parallel 2
```

The original PACE imports require the typed loaded-library API from
[SDK PR192](https://github.com/tekulvw/ps3recomp/pull/192), which is absent from the
normal runtime baseline. Until that API is part of a reviewed runtime pin, select
its explicit 2.12 SDK candidate with `MAG_RUNTIME_CANDIDATE_MANIFEST`, binding a clean
SDK commit and tree, framework hash, reviewed ancestor revision, target,
`codex/retail-v2.12` integration branch and a focused SDK source PR. This leaves
the ordinary runtime allowlist unchanged. Candidate manifests and local SDK paths
belong in ignored working data; no release tag is required or created by this flow.

Native receipts retain `experimental: true` and `not_runtime_verified: true` and
bind the PPU, SPU, firmware, PACE and adapter sources. Execution requires separate
explicit disc and HDD roots. The launcher handoff verifies the exact updated
HDD engine, its native receipt and key hash at the actual spawn request; the
original availability check still runs for both engine and PACE. A test uses a
disposable HDD copy so the verified assembled installation remains unchanged.
The observed launcher settings come from `tpps.psarc.sdat`, which takes precedence
over the same member in `game.psarc.sdat`. A bounded headless bootstrap uses
`tools/v212_boot_probe.py`, requiring a verified private TPPS settings proof with
literal closed-loopback endpoints. It checks ownership and stops/reaps its exact
process group; logs and receipts stay ignored. This tool exposes no live-host or
input-injection option. Its loopback settings checks are not an OS egress sandbox.

## Offline SPU audit

```sh
.venv/bin/python tools/target_spu_audit.py --target retail-v2.12 \
  --sdk local/ps3recomp-runtime
```

The default output is `analysis/retail-v2.12/spu-lift-audit/`. Use a new `--output`
directory for another audit. It validates the selected profile and framework-pinned
runtime SDK, inventories embedded SPU ELFs and raw WWS modules, and generates fresh
audit-only lifts with diagnostic manifests. The known policy region is accepted
only after its source-bound hash matches. Sectionless SPU images use program headers.

`audit.json` records tool/input hashes, discovery occurrences, unsupported code,
unresolved direct targets, unknown roles, timeouts and failures. Each subprocess is
bounded, and a ten-minute phase limit prevents unbounded generation. No production
registrations are emitted. A nonzero CLI exit with a completed report can mean that
hazards were found; it must not be converted into a coverage pass.

## First real-input audit

The original 2.12 launcher and engine were prepared and inventoried successfully.
The inventory found 6,840 launcher and 56,130 engine function candidates; these are
heuristic discovery counts rather than proof of execution coverage.

| PPU census from the pinned lifter | Launcher | Engine |
| --- | ---: | ---: |
| Non-`.word` unsupported markers | 6 | 10 |
| `.word` markers needing classification | 3,532 | 26,006 |
| Unresolved-target marker stubs | 0 | 0 |
| Fallthrough-into-gap candidates | 0 | 3 |
| Static branch targets outside the input function map | 1,963 | 22,089 |

The non-word markers are `vexptefp` and `vlogefp` in both images, plus `vsubuws`
in the engine. The runtime has helpers for some of these; this does not establish
that the selected older PPU lifter emits the correct calls. Updating or splicing a
lifter requires its own source-bound validation, not a pin/allowlist bypass.

The corrected SPU audit covered 11 unique embedded ELFs, 33 raw WWS modules and
one exact known policy region. All 45 entry symbols were generated. Its census
reported 310 unsupported words: 306 in two sectionless ELFs and four in the known
policy. It also reported 61 unresolved direct targets in embedded ELFs, 150 in
raw WWS modules and one in the policy. Forty-four program roles remain unattributed.

These are initial census findings, not 310 proven reachable failures or a claim
that the raw-module analysis is complete. `.word` sites can include embedded data,
and branch scans can include data interpreted as instructions. Conversely, zero
TODO markers cannot prove indirect-call or ABI coverage. The PPU audit writes a
census baseline by design; a zero exit only means the census was retained.

With the newer explicit lifter role, all 21 launcher/engine PPU translation units
pass syntax checks without non-word TODOs, unresolved targets or missing
continuations. The three engine fallthrough candidates were input-map boundary
errors; their actual next functions are emitted and registered. Word-marker
censuses and executed halt guards remain. All 46 prepared SPU C units also compile.

The first bounded native launcher probe reached original startup and firmware
loading, then halted at missing SDK `sys_prx_register_library`. Correct registration
of the game's shared-library exports is required before PACE can use them. No live
2.12 server connection, game boot or real-match acceptance is established by these
build and bootstrap findings. Protocol 1793 server work proceeds independently on
its integration branch with isolated accounts and service endpoints.
