# Retail v1.0 decrypted inputs

The build needs both v1.0 executables from your own BCUS98110 disc copy, `EBOOT.BIN` and
`bin/mage_g.self`, plus their decrypted ELFs. This repository contains no keys and no game bytes.

1. Copy the original `PS3_GAME/USRDIR` files to `local/retail-v1.0/PS3_GAME/USRDIR` (ignored by Git).
2. Decrypt those copies with RPCS3's standalone `--decrypt`, writing `EBOOT.elf` and `bin/mage_g.elf`
   next to them. No game needs to be booted.
3. Run `python3 tools/prepare_inputs.py --source-root /path/to/your/PS3_GAME/USRDIR`.

`prepare_inputs.py` verifies the originals, the local copies and the decrypted images against the
SHA-256 identities in the tool, validates the ELF64/PowerPC64 headers, segments and entry
descriptors, and writes `builds/retail-v1.0.json` (ignored by Git) with the paths and identities the
later lift and build steps read. Any other release or a modified file is refused.

For reference, the v1.0 EBOOT entry descriptor `0x376f38` resolves to code `0x10230`, TOC
`0x3853c0`; mage_g's entry descriptor `0x1ad9650` resolves to code `0x10230`, TOC `0x1b25b30`.
The equal code addresses belong to separate images.
