# Retail 2.12 branch foundation and lift audit

The user superseded permanent side-by-side support: develop on a dedicated 2.12
branch, then switch fully once 2.12 works. Main remains the working v1.0 baseline.
Keep every 2.12 input profile, inventory, generated lift, build directory and audit
receipt separate during development. This milestone prepares and audits code;
it does not establish runnable 2.12 support or authorize a premature cutover.

Use a small explicit target registry as transitional branch scaffolding and optional `--target` arguments on input
preparation, inventory and PPU lifting. Omitting the argument must retain existing
v1.0 paths, return shapes, hooks and framework pins. Registry identities bind the
two SELF/ELF pairs, title ID, application version and target data paths. Reject
unknown targets and mixed identities. Per-machine paths stay Git-ignored.

For 2.12, validate the completed installation and independently decrypted executable
copies before writing a separate image profile. Inventory and lifting select that
profile explicitly, carry target identity in new receipts, verify manifest hashes,
and refuse output paths belonging to the other target. Existing v1-only extension
hooks/nonlocal profiles must not be silently applied to 2.12.

The production CMake build remains v1.0-only at this milestone. Expose the target
choice with a clear early error for audit-only 2.12, before v1 adapters can be
compiled into a mislabeled build. Native handoff, controls, sign-in, server protocol
and optional renderer mappings require later work.

Use the already pinned analysis/PPU lifter revisions. Generate the complete 2.12
launcher and engine offline, retaining unsupported instruction, unresolved target,
fallthrough and branch-coverage census results. An audit-baseline update is evidence,
never a correctness pass. Add a separate reusable offline SPU audit covering embedded
ELFs, streamed WWS jobs and policy identification; preserve role/ABI unknowns.

All tests use synthetic fixtures. Original payloads, generated code and raw audit
logs remain private/Git-ignored. No keys, game assets, decrypted bytes, generated
source or user paths are committed. No game launch, deployments or releases.

Approach: temporary explicit target selection reuses the current source pipeline
while keeping address-specific integration guarded. Globally changing the v1 profile
before native acceptance would corrupt the baseline; duplicating the whole runtime
would unnecessarily fork generic implementation. Permanent version selection in
the player UI, dual server deployment and long-term v1 maintenance are not goals.
Python follows the existing build-tool interfaces.
