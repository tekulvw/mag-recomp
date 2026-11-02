# Controls Settings UI Implementation Plan

**Goal:** Edit effective keyboard/mouse and controller bindings in the existing MAG Settings window.

**Architecture:** A portable Rust binding editor handles conflict-safe edits to the existing binding records. A portable C++ service owns defaults, action descriptors, persistence and a runtime apply callback. AppKit renders the controls page and captures physical input while gameplay is blocked. The private adapter supplies action names and queues profile changes onto its native input thread.

**Constraints:** Preserve connection/graphics settings and accepted raw mouse behavior. Keep native controller axes/curve unchanged. KBM aim assist cannot be enabled. Only on-foot, implemented settings are editable. Preserve original profile on validation/write failure. No game content or native layouts in public source. Rust fmt and Clippy warnings denied.

- [x] Add tested portable edit operations: primary/secondary, clear, context-aware conflicts, explicit replacement, no mutation on cancellation.
- [x] Add descriptors, draft/save/reset service; register private catalog and native-thread apply.
- [x] Implement separate KBM/controller pages, press-to-bind capture, clear and reset; integrate existing window Apply/Cancel without display confirmation for controls-only edits.
- [ ] Wire complete digital controller remapping, including explicit unbound actions and fresh activation on reset. Preserve native movement/look.
- [ ] Run focused Rust, C++ and native settings fixtures; inspect the panel and produce the stable-identity test build. Coordinate Windows API details through its owner.

**Review focus:** Unbinding an action must not restore its native binding; settings/capture cannot leak gameplay events; held keys must be rearmed after close; profile apply cannot erase sticky KBM provenance; controls-only saves cannot alter graphics/server fields.

## Execution checkpoint

Portable editor unit tests and the C ABI capacity/conflict fixture pass. Service and AppKit fixtures verify Apply/Cancel/explicit-unbind and unchanged General settings. Existing affected input/settings tests pass36 checks; Rust tests pass42 checks. Rustfmt, Clippy all-targets/all-features with warnings denied, and public-tree checks pass. Private native digital remapping and queued apply are developed alongside this public slice; a source-bound game build remains pending.

Implementation decisions: Apply saves the visible page, with explicit “Apply controls” copy. Play saves both pages during startup. Native controller reset relinquishes digital replacement; the supported catalog stays complete after unbinding. There is no approval gate between implementation tasks under the workspace's standing fast-iteration authorization.
