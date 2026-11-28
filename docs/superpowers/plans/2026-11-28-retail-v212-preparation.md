# Retail 2.12 preparation implementation

1. Isolate four PSARCs and official PKG in the private backup folder; verify identities.
2. Generate a public metadata-only profile from the original disc inventory, official
   package inventory and independently verified patch chain. Pin the existing SDK
   extractor's source hash. Keep all payloads outside tracked paths.
3. Write synthetic tests first: valid assembly, immutable inputs, corrupt/missing data,
   patch dependency errors, path/symlink rejection, existing-output preservation,
   extraction failure cleanup, and independent installed-tree verification.
4. Implement standalone assembly/verification and bounded external PKG extraction.
   Preserve default v1.0 tooling. Document exact inputs, layout and offline limits.
5. Run focused synthetic tests and public-tree checks. Assemble the user's actual
   disc, backed-up PKG and PSARCs; independently verify the resulting layout.

Implementation proceeds within the user's requested automation scope. No releases,
game runs, publishing, native support claims or changes to existing configurations.

## Execution result

Completed all five steps. Fourteen synthetic preparation tests and four existing
v1.0 input tests pass under Python 3.14. Public-tree checks cover all new files as
well as tracked content. A real-input assembly from the original disc, raw official
PKG and four PSARCs produced 375 files (16,606,898,421 bytes); a separate verify
invocation passed all pinned identities and embedded patch dependencies.

The external extractor is the byte-identical helper from the current pinned runtime,
not an SDK override. An RPCS3 extraction probe was rejected as the production path
because it transforms an SDAT and this installed version aborts during shutdown.
No firmware installation or game boot was needed for the selected helper path.
Runtime and native 2.12 acceptance remain outside this offline preparation result.
