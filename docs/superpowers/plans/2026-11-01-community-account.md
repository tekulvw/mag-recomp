# Community account settings repair

> Implement inline using executing-plans and test-driven-development. The user requested continued Windows fixes and explicitly opted out of repeated proposal approvals.

**Goal:** Make the packaged community-server launch supply the user's selected local account through the existing supported identity path.

**Design:** Add optional schema-1 `local_account_name`, default empty for migration. Accept 3–15 ASCII letters, digits, hyphens or underscores (the original short-string capacity is 15). Preserve case for display. Community launches require an explicit name before starting; blank server retains original configuration. Save the name in the immutable launch snapshot and set the four existing local identity environment values in both launcher and engine. Never replace a different nonempty guest identity or weaken ticket, code-identity or unresolved-API guards.

**Approaches:** An inherited environment profile disappears in the packaged supervisor and is not a durable preference. Hardcoding an account selects identity for the user. A saved, explicit account field fits the existing settings and next-launch connection contract.

**Files:** Portable and Foundation stores, native settings panels, portable/macOS startup runtime, guarded retail network policy and synthetic persistence/runtime/panel fixtures. No server changes or game-derived content.

## Tasks

- [x] Add failing synthetic account format, persistence/migration, original-setter guard and launch snapshot checks.
- [x] Implement the optional field and the guarded generic short-name policy; wire native panels and startup environment.
- [x] Run focused synthetic Windows checks, preserving next-launch transactions and original-server behavior. Foundation fixtures were updated; macOS compilation remains unavailable on this host.
- [x] Rebuild/package both owned stages and verify launcher authentication and engine handoff through saved settings without private identity injection. Engine title/menu validation is tracked separately and remains incomplete.

## Review focus

- Old settings migrate in memory without rewriting the file or selecting an account.
- Empty community identity requests setup; original-server mode does not opt into local identity.
- Invalid, overlong, non-ASCII and embedded-NUL input cannot reach the guest setter.
- Existing nonempty different identity, ticket-enabled/locked config and mismatched retail code remain rejected.
- In-game account changes affect the next launch; the running engine retains the snapshot.
