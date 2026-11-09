# Working rules

These rules come from the project's bring-up and the mistakes made along the way. They complement
[AGENTS.md](../../AGENTS.md), which has the content and clean-room rules that apply to every change.

## Starting point

- Start every task from freshly fetched `origin/main` and create a focused branch from it. A
  historical feature branch, an old build directory or an object file from someone else's build is
  never an implicit baseline.
- A checkout with uncommitted work belongs to whoever made it. Use a separate worktree or clone
  rather than resetting or stashing it.
- Fetch again before publishing, and resolve concurrent changes explicitly.
- `profiles/framework.json` is the only source of the ps3recomp pins. `tools/fetch_ps3recomp.py`
  checks them out; the build and the lifting tools refuse other or dirty revisions. An SDK change
  reaches mag-recomp only as a new tagged pin (see [the fork](ps3recomp-fork.md)), never as a local
  commit added to an allowlist.

## Normal defaults versus experiments

The normal build is what the installer produces: the launcher and engine lifted at the pinned
revisions, every streamed WWS job compiled and registered, and the runtime switches listed in
[ps3recomp-flags.md](../ps3recomp-flags.md). Those switches are the ones the accepted hosted runs
used. Generated PPU code builds at optimisation level 1.

Everything else is opt-in and off by default: the Rust render frontend and adapter, native job
replacements, capture and trace observers, ablation stages and timing diagnostics. When comparing:

- Change one thing, and record what changed. A launch receipt and the actual launch environment
  must agree; never relabel a switch that was on, or loosen a qualification, to make a run count.
- A linked provider is not evidence that it ran. Check its own counters or trace events.
- An environment switch cannot enable code that is not in the binary. The clearest example: a build
  whose generation step produced none of the 30 streamed WWS job registrations still ran, but every
  job fell back to the SPU interpreter and the frame rate dropped by an order of magnitude. No
  runtime setting could recover it; the fix was to generate and register the full job set, which
  `tools/wws_job_lift.py --require-complete` now enforces.
- Counters from two unmatched runs are a hint, not a comparison. See
  [Verifying a run](verification.md) for what a comparison needs.

## Engineering rules

These have held since the first native boot and still apply.

- **Supply real behaviour or stop visibly.** When the runtime reaches an unimplemented syscall or
  import, it stops with a clear diagnostic under the strict policy rather than returning a guessed
  success. Implement the missing behaviour on its own SDK branch, with a regression that fails
  before the fix.
- **Never fabricate progress.** Do not synthesize job completions, events, callbacks, network
  results or game-state transitions to get past a wait. If the game waits on work, that work has
  to run. For example, the offline network backend reports a cancelled network dialog instead of a
  success it cannot back, which lets the game take its own connection-lost path.
- **Title-specific behaviour is explicit.** A change that exists only for MAG is opt-in, guarded by
  the identity of the input it applies to (a file hash, a generated-function hash or an exact guest
  address), and documented as a deliberate adjustment. Ordinary behaviour stays the default.
- **Lift only what is code.** The lifting tools work from the executable sections and from bounded
  reachability. Data and embedded SPU images inside an executable segment are not lifted as PPU code.
- **Identities over trust.** Inputs, generated sources and builds carry SHA-256 identities and
  receipts. Tools refuse substituted inventories, failed audits and mismatched receipts instead of
  warning and continuing.
- **Decryption is the user's step.** The project validates decrypted executables but never decrypts
  or downloads them, and never launches an emulator to do so.
- **Retail v1.0 only.** Addresses and hashes from other builds are not applied without evidence.
- **Keep limits visible.** When a change is known to be partial (a placeholder, a polling delay, an
  untested platform), say so in the change and in the docs, and leave the unsupported case loud.

## Acceptance

Automated evidence and a person's confirmation are recorded separately; neither replaces the other.
A synthetic test proves a contract, not that the game renders, sounds right or plays. Game-level
acceptance is per platform: a result on macOS does not establish Windows or Linux parity, and green
CI on all three platforms does not establish that a hardware backend works on them.

The platforms the installer currently supports, and what has been played on each, are listed in the
[README](../../README.md#status).
