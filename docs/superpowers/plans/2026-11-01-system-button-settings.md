# Start, Select and D-pad rebinding

**Goal:** Make all six native button actions editable for keyboard/mouse and controller without producing the old action when a physical control is reassigned.

**Design:** Represent these six as native logical buttons. Keep the game's original traversal, callbacks, dynamic HUD watches, event timing and channel-consumption behavior. Up/Down retain their native scope/command context rules. Do not call tactical functions directly or invent a single universal semantic action for each physical direction. The private adapter proves the registered root or active UI descendant before projecting an offer; it keeps gameplay and mouse admission separate from menu/system routing.

**Public surface:** Extend physical controller capture to Select/Back, Start/Menu and D-pad directions; retain Guide as reserved. Append optional descriptor grouping and show the runtime's native button actions in a separate section. Read profile schemas1/2 and write schema3 with a catalog revision for one-time default migration; retain context-aware transactional editing. Suggested keyboard defaults are Escape, Tab and arrow keys, with each override explicit and cancellable in Settings.

**Private integration:** Add a complete native-button route catalog, verified native channel identities and root/active-child ownership checks. Preserve original native priority and consumption. Supply neutral values for explicitly unbound/reassigned native button actions. Deliver release as well as press; TCM HUD bindings need both. Pause/menu operation must not admit gameplay/mouse output or earn gameplay KBM provenance. Preserve focus/device/actor/profile resets, require fresh activation, and keep cached controller samples intact.

- [x] Verify native handler/context evidence with Astra; retain it privately.
- [x] Test then implement public controller capture/grouped descriptor rendering and profile edits.
- [ ] Test then implement native grouped-button routing and neutralization, including pause and active HUD children.
- [ ] Check old KBM/aim regressions, Rust fmt/Clippy/tests, public/native fixtures, then build the source-bound candidate with the reviewed SDK and normal rendering.
- [ ] Run a bounded owned correctness test where useful; distinguish keyboard acceptance from unavailable physical-controller verification. Coordinate the focused PR/integration with the existing owner.

**Review focus:** old Start/Select/D-pad action after rebinding; Up/Down shared consumers; Down+Up HUD callbacks; reserved Guide; stale focus/profile events; pause/menu callbacks allowing gameplay behind UI; native consumption/priority retained.


Implementation checkpoint: the native traversal remains authoritative. Public shared-context continuity is per physical source/device/code and matching action/behavior, so menu-only alternates cannot cancel or inherit a shared binding. An explicit native-controller action set retains native recognizers without inventing host pulse continuity. Menu relative-capture changes preserve valid mouse buttons and clear only motion. The UI now offers On foot and Menus scopes, editing one without changing the other. Focused checks passed49 Rust tests with rustfmt/Clippy and38 public input/settings fixtures; source-bound build and live acceptance remain pending.
