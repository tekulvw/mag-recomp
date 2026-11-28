# Retail 2.12 branch foundation

Development of BCUS98110 2.12 takes place on `codex/retail-v2.12`. The intended
outcome is a full switch once the new build works. Permanent player-facing
dual-version support is not the goal. Temporary target selection keeps the
working v1.0 baseline separate while the new port is prepared and audited.

Native execution remains supported for retail v1.0 only. The new 2.12 path is
offline preparation, inventory and lift auditing. Do not use its generated code
with v1 handoff, sign-in, controls, native SPU/Edge or renderer adapters.

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

Fetch the existing pins; no SDK allowlist or revision changes are needed:

```sh
.venv/bin/python tools/fetch_ps3recomp.py --role runtime --role lifter-spu --role lifter-ppu
.venv/bin/python tools/inventory.py --target retail-v2.12 \
  --sdk local/ps3recomp-lifter-5244db9
.venv/bin/python tools/lift.py --target retail-v2.12 --image EBOOT \
  --sdk local/ps3recomp-lifter-dfa7073
.venv/bin/python tools/lift.py --target retail-v2.12 --image mage_g \
  --sdk local/ps3recomp-lifter-dfa7073
```

Inventory uses the clean pinned analysis SDK. The 2.12 generator uses the clean
PPU lifter pin from `profiles/framework.json`; the existing v1 generation defaults
stay unchanged. Inventory and generation require fresh output directories for
2.12. An explicit `--output` may select another private directory but may not
overlap v1 working roots. Existing evidence is never automatically deleted.

Target and manifest hashes bind each inventory and receipt. The 2.12 path rejects
v1 nonlocal-control-flow profiles and lift hooks, including `MAG_LIFT_HOOKS`.
Unsupported generated PPU instructions and unresolved targets retain explicit
execution guards. The normal CMake build defaults to v1.0;
`-DMAG_RETAIL_TARGET=retail-v2.12` fails early with an audit-only explanation until
the runtime integration is implemented. This prevents a mislabeled v1 adapter build.

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

The next work is to classify these sites and map the 2.12 import/SPU roles before
enabling native execution. `libpace`, updated filesystem/metadata and engine
handoff, guest adapters, and protocol 1793 server integration remain separate gates.
No game boot or real-match acceptance is established by this milestone.
