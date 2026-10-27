# Reproduce the focused PPU CTR relift

This MAG-owned utility validates the entire original function against the old
lifter, validates the entire replacement against the corrected lifter, and
changes only the MTCTR assignment. It keeps all other generated file bytes and
formatting intact. It does not contain original game instructions or assets.
The external retail ELF and generated file must match the reviewed profile.

Use Python 3.10 or later and a local ps3recomp Git repository containing both
immutable commits in the profile. The current checkout and its dirty working
files are not imported: both lifters and their shared decoder are read directly
from the specified Git objects. Review the profile before running a different
one, because the pinned generator Python is executed during source generation.

From the MAG repository, verify an existing original or corrected file:

```sh
python3 tools/partial_ppu_relift.py \
  --sdk /path/to/ps3recomp \
  --profile profiles/retail-v1.0-ppu-ctr64-partial-relift.json \
  --elf /path/to/retail/mage_g.elf \
  --source /path/to/generated/ppu_recomp_007.cpp \
  --verify-only
```

Prepare a corrected file from the profiled original generated file by replacing
`--verify-only` with `--output /path/to/fresh/ppu_recomp_007.cpp`. The output and
its `.receipt.json` must both be absent, and no input is overwritten. The JSON
receipt records the immutable tool revisions, source, original ELF/function,
profile, utility and output hashes. Integrate that output through the existing
MAG build workflow, retain fresh build receipts, and test the resulting binary.
The source-only receipt does not establish native success.

The current profile repairs one complete function. Other generated functions
have not been relifted; the generic SDK also corrects full-width CTR decrement,
which is not silently applied to the rest of this existing generated tree.

Synthetic regression checks (no title bytes):

```sh
python3 tests/test_partial_ppu_relift.py --sdk /path/to/ps3recomp
```

The tests exercise exact/full-body equivalence, an already-applied read-only
verification, malformed ELF bounds, changed source/ELF/instruction identity,
unpinned tools, changes beyond MTCTR, and fresh output/receipt requirements.
