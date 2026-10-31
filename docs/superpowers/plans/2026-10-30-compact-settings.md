# Compact Settings Implementation Plan

> Implement inline in this chat, using executing-plans and test-driven-development. The user has approved the compact preview and explicitly requested implementation.

**Goal:** Add the compact macOS settings panel to the player app, with working persistence and runtime integration.

**Architecture:** A Foundation store validates and atomically saves configuration. An AppKit controller handles editing and reversible display changes. A bounded PSARC helper creates a private server override; the runner and packaged supervisor opt into the feature explicitly.

**Tech Stack:** Objective-C/AppKit, Objective-C++/C++, zlib, Python synthetic test drivers.

**Spec:** `docs/superpowers/specs/2026-10-30-compact-settings-design.md`

## Constraints and review focus

- Preserve original game files, account/ticket policy, private workspaces and indices. Do not run the game or publish.
- macOS implementation; other platforms continue to build without AppKit.
- Empty server retains existing routing. Reject URL syntax, IPv6-only destinations, invalid ports, malformed/oversized settings and unsupported schemas.
- Failed writes and display rollback leave the last saved settings intact. Recovery setup precedes guest initialization.
- Use only the exact owned Metal window; prevent controller/keyboard input leaking while settings is active.

## Tasks

- [x] 1. Add Foundation validation/storage and synthetic tests for defaults, address/port errors, atomic round trips and corrupt-file recovery.
- [x] 2. Add the bounded archive member editor, private overlay and synthetic tests proving unrelated content/originals survive and malformed inputs fail.
- [x] 3. Add the compact AppKit panel, display transaction, startup/reopen paths, AA startup selection and input gating. Compile and exercise it against a synthetic window.
- [x] 4. Wire the runner/CMake and packaged supervisor, update player documentation, and verify affected native/installer tests and the public-tree check. Record validation and remaining game-test limits.

## Local execution notes

- The app-managed worktree tool cannot select this source repository from the current project. Use an isolated local checkout on `codex/compact-settings`; leave the original checkout untouched.
- Use Python 3.11 or newer. The system `python3` is older and cannot run the existing hashing tests.
- No delegated work: the workspace pause continues to cover delegation. Perform the review inline.

## Validation

- Client suite: 242 tests, passing with 6 platform skips.
- Installer suite: 130 tests, passing with 1 platform skip. Loopback download fixtures required the normal sandbox exception.
- Native panel: Apply/Cancel, Keep/Revert, the full 15-second timeout, fullscreen keyboard/close eligibility and damaged-file recovery exercised with a synthetic window.
- CMake module: compiled and ran the synthetic launcher/engine settings snapshot and native/SDL input ownership checks through the actual module.
- Private archive: source and unrelated-member preservation, identity refusal, cache reuse and changed-cache rejection verified with synthetic data.
- Supervisor: compiled, signed and exercised with two synthetic native executables; setup flags are removed for engine handoff and the owned temporary view is removed.
- Standalone native preview built locally from the UI/store sources; no game or account data included.
- No MAG run, private submodule change, index change, remote push or publication performed. Full in-game acceptance and an updated game binary remain outside this settings-only source checkpoint.
