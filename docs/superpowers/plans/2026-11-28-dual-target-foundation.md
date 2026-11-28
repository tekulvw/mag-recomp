# Retail 2.12 branch foundation implementation plan

> Agentic execution: implement the approved scope with bounded synthetic tests and
> independent parallel audit/tool tasks. The user's standing fast-loop directions
> supersede routine proposal approvals and per-task review gates.

**Goal:** develop on `codex/retail-v2.12`, preserving main's v1.0 baseline while
preparing, inventorying and auditing separate 2.12 launcher/engine lifts. Temporary
target separation protects development; permanent dual-version support is superseded.

**Architecture:** immutable target definitions select per-target workspace paths.
Existing v1 APIs keep default arguments and legacy receipt shapes. New target
receipts identify audit-only scope; native 2.12 configuration is explicitly blocked.

**Tech stack:** Python 3.11+, JSON identity metadata, CMake, pinned ps3recomp tools.

**Spec:** `docs/superpowers/specs/2026-11-28-dual-target-foundation-design.md`.

## Constraints and review focus

- Preserve default v1 paths, hooks and framework pins; do not alter existing data.
- Reject mixed image pairs, changed manifests and outputs in the other target tree.
- Do not treat a zero census-baseline exit as zero hazards or runtime acceptance.
- No raw game content in tracked files; no game or server runs in this milestone.
- Test default invocations, explicit 2.12 selection, incorrect target/profile binding,
  output collisions and failed generation with retained audit evidence.

## Task 1: Target registry and strict input preparation

Files: `tools/targets.py`, `tools/prepare_inputs.py`, `tests/test_targets.py`,
`tests/test_workspace.py`, `.gitignore`.

Interface: `DEFAULT_TARGET`, `TARGET_NAMES`, `get_target(name=DEFAULT_TARGET)` return
an immutable definition exposing `name`, `app_version`, `title_id`, `executables`,
`profile`, `analysis`, `generated`, `local_usrdir`, `native_supported`.
Path fields are repository/workspace-relative strings; executable definitions map
SELF relative paths to `(self_sha256, elf_sha256)` tuples. `get_target` rejects
unknown values. `prepare_inputs(..., target=DEFAULT_TARGET)` and
`write_image_profile(..., target=DEFAULT_TARGET)` preserve legacy defaults.

- [x] Write and run failing synthetic tests for the default/explicit target paths,
  mixed identities, target-scoped profile writes and invalid output overlap.
- [x] Implement registry, separate preparation and ignored machine-profile paths.
- [x] Run affected preparation/workspace tests; commit focused source changes.

## Task 2: Target-aware inventory and PPU generation

Files: `tools/inventory.py`, `tools/lift.py`, inventory/lift synthetic tests.
Consume Task 1 definitions. Add defaulted target selection to reusable generation
APIs and CLI entry points. Preserve no-argument v1 behavior and existing hooks.
2.12 validates target identity in its profile/summary, uses fresh target output,
retains generator and audit evidence, and cannot opt into v1-only hooks/config.

- [x] Write/run failing synthetic target-isolation and identity tests.
- [x] Implement profile/path selection and target-specific hook guards.
- [x] Run focused old/new tests and retain default compatibility.

## Task 3: Offline SPU lift audit

Files: a new generic `tools/target_spu_audit.py` and synthetic tests.
Consume explicit input profiles and separate output; use pinned generic discovery,
disassembly/lifter tooling. Audit all 2.12 embedded ELFs and streamed WWS modules,
separating unsupported coverage from successful tool execution. Policy equality is
an identity observation, not proof of a complete version-specific registration map.

- [x] Test discovery, sectionless ELF handling, failed tool execution and isolation.
- [x] Implement auditable offline generation/census without production registration.
- [x] Execute bounded real-input audit privately after registry inputs are prepared.

## Task 4: Build guard, integration and real-input evidence

Files: `cmake/MagRetailTarget.cmake`, `CMakeLists.txt`, build guard synthetic tests,
`BUILDING.md`, `docs/retail-v2.12-preparation.md` and a dual-target usage guide.

- [x] Test explicit target validity/default and audit-only native-build rejection.
- [x] Add early CMake target guard and documented per-target commands/output paths.
- [x] Run affected synthetic suite and public-content checks.
- [x] Prepare original v1 and2.12 profiles in an owned private workspace; inventory
  and generate both 2.12 PPU images using clean pinned SDKs, then record actual
  census results and residual unknowns. Existing v1 source/input/output identities
  must remain unchanged.
- [x] Record results, inspect final diff and commit reviewable source. No publication.

## Foundation execution evidence

The dedicated branch is `codex/retail-v2.12`, based on fresh main `be2eb7669e0a57a3c8426b6031d83062a656ffec`.
Seventy-eight focused synthetic tests pass. Both original source pairs, local SELF
copies and ELF pairs were rehashed after the audits. Full2.12 PPU generation and
census completed using the unchanged approved analysis/PPU pins. The launcher
reports6non-word unsupported sites; the engine10 plus3fallthrough candidates.
The corrected SPU census generated45entries (11uniqueELFs,33WWS,1knownpolicy),
with310unclassified unsupported words,212unresolveddirecttarget candidates and
44unknownroles. Tool completion does not establish runtime coverage.

Original payloads, all generated code and full audit logs remain ignored/private.
The reusable public tools, synthetic tests and metadata-only documentation are
reviewable source changes. No game launch occurred during this foundation phase.

The user subsequently authorized autonomous continuation through a bootable2.12
client, first login, charactercreation and successfulre-login via the isolated2.12
server. That runtime milestone is subsequent implementation, not completed by these
offline audit receipts. Keep existing human accounts/services and main intact.
