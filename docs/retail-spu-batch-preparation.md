# Inventory-driven MAG retail SPU batch preparation

`retail_spu_batch.py` processes every `scope=retail` image in an explicitly
SHA-256-pinned coverage inventory. It does not select only observed runtime
misses. All carrier hashes, sizes and image occurrences are verified before any
original ELF is written. ELF32 big-endian SPU headers, table/backing/LS bounds,
entry, executable end and exact runtime fingerprint must agree with the profile.
Verified identical bytes are deduplicated by full SHA-256. Prefixes use the full
identity so different images cannot collide through a shortened filename.

For each unique image it extracts into a fresh private directory, runs the
pinned SDK lifter with `--auto-functions`, explicit `--code-end` and a unique
symbol prefix, rewrites only relative `runtime/spu` header includes, records
unsupported markers and entry/function definitions, and compiles an object with
`-O1 -std=gnu11 -ffp-contract=off`. It records commands, exit/timeout/elapsed
results, source/header/object hashes and logs. Timeouts kill only the tool's
owned subprocess group on POSIX. Default lift/compile deadlines are60/90seconds;
explicit overrides remain bounded to600seconds.

The SDK must be clean at the requested Git revision. SDK identity is rechecked
before/after each image and at completion. An individual lift/build/entry failure
is retained, and the remaining images are attempted. A global identity failure
before extraction writes no output. A later SDK drift leaves an explicit failed
or incomplete report, never a successful batch. `report.json` is replaced
atomically after each image. Exit0 means all requested unique objects compiled
with no unsupported markers; exit1 means completed with per-image failures or
unsupported markers; exit2 means global input/tool identity/setup validation
failed. Interrupted runs retain `status=incomplete`; no stale output is reused.

`compiled` does **not** mean linked, registered, correctly routed, executed or
accepted. The report always says `link_status=not_linked`,
`runtime_activation=not_enabled`, and `runtime_routing=unverified`. Existing
registration data remains the inventory's named source-cutoff evidence; the tool
does not substitute it for a current link map or live routing proof. It emits no
registration source/constructor, links no original program and runs no original
program. Nonretail ELF occurrences and inventory registrations outside the
selected ELF set are reported separately. Raw policies, streamed non-ELF job
modules and unlisted/compressed inputs are outside this ELF inventory scope.
Zero unsupported markers does not establish complete helper/ISA conformance.

Example (paths and revisions must come from the selected reviewed build):

```sh
python3 tools/retail_spu_batch.py \
  --inventory builds/retail-v1.0-spu-coverage.json \
  --inventory-sha256 REVIEWED_COVERAGE_SHA256 \
  --carrier EBOOT=/absolute/private/EBOOT.elf \
  --carrier mage_g=/absolute/private/bin/mage_g.elf \
  --sdk /absolute/private/pinned-sdk \
  --sdk-revision REVIEWED_SDK_COMMIT \
  --output /tmp/fresh-private-spu-batch
```

Original ELF/C/H/O/log outputs must remain outside Git or in an explicitly
ignored directory. The tool refuses unignored in-repository outputs and Git
metadata paths. Its fresh final directory is owner-only. Never commit or publish
the extracted or compiled outputs.

Run neutral tests:

```sh
SPU_BATCH_TEST_SDK=/absolute/private/pinned-sdk python3 tests/test_retail_spu_batch.py
```

The suite covers exact dedup, carrier/image/fingerprint identities, malformed
ELF/header/table/LS/slice bounds, code/entry pins, partial batches, subprocess
errors/timeouts, missing objects/entry classification, fresh/symlink inputs,
Git output exclusion and a real SDK lift+compile of a tiny synthetic SPU ELF.
The controlled failure generator is used only to exercise orchestration; the
real SDK smoke separately validates actual generator/compile arguments.

The coverage inventory lists 14 retail occurrences and 12 unique ELF identities.
The tool accepts that inventory without embedding any game hashes, addresses,
bytes, role assumptions or expected count in its code.
