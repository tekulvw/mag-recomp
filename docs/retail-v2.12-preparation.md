# Prepare an offline US disc 2.12 installation

`tools/prepare_v212_install.py` assembles a separate BCUS98110 disc installation from
local inputs. It verifies all 355 original disc files, all 15 official update files,
the four patch PSARCs and their embedded dependency chain. The result is an offline
verified layout at APP_VER `02.12`, content level `489979`.

**Native execution of 2.12 is not supported yet.** This tool does not change v1.0
build profiles, decrypt/lift executables, configure an emulator or boot the game.
No PS3 or access to the former game download servers is needed for this preparation.
Runtime installation behavior and gameplay acceptance still require separate work.
The next stage is documented in the [2.12 branch foundation and lift audit](retail-v2.12-branch.md).

## Inputs

Supply an extracted, original US retail disc root containing `PS3_DISC.SFB` and
`PS3_GAME/`, plus the following five files. The disc must retain its original encrypted
SDATs and SELF executables. A modified/decrypted disc tree will fail verification.
The tool accepts directories, not ISO or 7z containers; extract those beforehand.

| File | Bytes | Role |
| --- | ---: | --- |
| `UP9000-BCUS98110_00-MAGPATCH00000001-A0212-V0100-PE.pkg` | 36,198,816 | Official application update |
| `patch_r000000_445230.psarc` | 1,397,283,797 | Content 0 → 445230 |
| `patch_r445230_465304.psarc` | 163,343,555 | Content 445230 → 465304 |
| `patch_r465304_467225.psarc` | 684,130 | Content 465304 → 467225 |
| `patch_r467225_489979.psarc` | 40,166,221 | Content 467225 → 489979 |

Sizes and SHA-256 identities are pinned in `profiles/retail-v2.12-inputs.json`.
Digital-edition copies of the first two PSARCs have the same names but different
contents; they are rejected. These are the installed PSARCs, so the earlier `.dlarc`
transfer containers are unnecessary. Optional Interdiction and Escalation DLC and
their entitlements are outside this base update layout.

## Assemble

Use Python 3.11+ as for the rest of the repository. Fetch only the pinned runtime SDK
to obtain its existing PKG extraction helper. No keys are included in this tool.
Installing pycryptodome avoids the helper's much slower pure Python AES fallback.
The extraction subprocess has a three-minute limit and fails without finalizing an
installation if it times out.

```sh
python3 tools/fetch_ps3recomp.py --role runtime
python3 -m venv .venv
.venv/bin/python -m pip install pycryptodome
.venv/bin/python tools/prepare_v212_install.py assemble \
  --disc '/path/to/original-disc' \
  --patches '/path/to/patch-backup' \
  --update-pkg '/path/to/patch-backup/UP9000-BCUS98110_00-MAGPATCH00000001-A0212-V0100-PE.pkg' \
  --output 'local/retail-v2.12-install'
```

On Windows, use `.venv\Scripts\python.exe` and one command line or PowerShell
continuations. Extraction uses the interpreter running this command. All input and
output locations come from arguments; assembly and verification do not download files.
The default helper path is `local/ps3recomp-runtime/tools/pkg_extract.py` relative to
the repository. `--pkg-extractor` can point to another copy of that identical helper;
its source hash must match the shipped profile.

An already extracted, byte-identical official package tree can replace `--update-pkg`:

```sh
python3 tools/prepare_v212_install.py assemble \
  --disc '/path/to/original-disc' --patches '/path/to/patch-backup' \
  --update-tree '/path/to/extracted-update' --output 'local/retail-v2.12-install'
```

This alternative needs only Python's standard library. The extraction tree must have
`PARAM.SFO` and `USRDIR/` directly beneath it. RPCS3's installed tree can contain
automatically decrypted SDATs, which this byte-preserving profile rejects; use a raw
PKG extractor for this mode.

The output must be a new directory outside every input. The tool refuses existing
outputs and symlink inputs, checks every input identity, builds and verifies a staging
directory, then renames it into place. It copies only the known profile files, so
extra prepared ELFs or account files in an input directory are omitted. It never
hardlinks files. macOS uses independent APFS clones when available; other filesystems
need about 16.7 GB free for full copies, plus temporary PKG extraction space.

## Output and verification

```text
retail-v2.12-install/
  disc/
    PS3_DISC.SFB
    PS3_GAME/                       original disc, unchanged
  dev_hdd0/game/BCUS98110/
    PARAM.SFO                      official update metadata
    USRDIR/EBOOT.BIN                official 2.12 launcher
    USRDIR/bin/mage_g.self          official 2.12 engine
    ...                            all other official package files
    USRDIR/game.psarc.sdat          original disc settings archive
    USRDIR/patches/                 four PSARCs, retained as overlays
  installation-receipt.json
```

The disc retains its v1.0 files and metadata. The separate HDD update supplies 2.12
executables and metadata. The PSARCs are installed overlays, not binary patches to
apply directly to individual disc files. No user profiles, accounts, DLC licenses or
emulator configuration are created. The tool does not flatten this into a modified disc.

Recheck the installation at any time:

```sh
python3 tools/prepare_v212_install.py verify --install 'local/retail-v2.12-install'
```

Verification reads all 375 installed files against the shipped profile, checks the
patch manifests, rejects unexpected files and checks the receipt. It does not trust
hashes supplied by the receipt. The receipt explicitly records `runtime_verified: false`
and `native_v212_supported: false`. This verifies the offline data set and layout,
not a successful boot, original-server availability or native 2.12 compatibility.

Keep the installation and original inputs private. Back up the five input files and
their checksum manifest separately from the original disc.
