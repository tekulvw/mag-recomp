# Contributor and agent rules

These rules apply to everyone, including coding agents.

## Never commit game content

- No original game files, firmware, keys or licences, in any form or encoding.
- Nothing extracted, decrypted, recompiled or lifted from them: no generated C/C++, SPU objects,
  embedded byte arrays, disassembly, decompiler output or hex dumps.
- No runtime captures, logs, screenshots, frame dumps or traces from running the game.
- Keep all of the above under the Git-ignored `local/`, `analysis/`, `generated/`, `build/` and
  `runs/` directories.

Identities that verify inputs (SHA-256 hashes, sizes, offsets, addresses, generated-function hashes)
are allowed. Run `python3 tools/check_public_tree.py` before committing; CI runs it too.

## Clean-room native code

The game's SPU jobs run as recompiled original programs. Any native replacement must follow
[docs/native-graphics-reimplementation-method.md](docs/native-graphics-reimplementation-method.md):
specify behaviour in your own words first, then implement from that specification. Never port,
transliterate or paste original code structure, and never re-use the original program's internal
memory layout, register use or call order.

## Portability

- No absolute or user-specific paths. Take locations from arguments, environment variables or
  repository-relative defaults.
- Retail v1.0 is the only supported build. Do not apply addresses from other builds without evidence.
- Keep unsupported runtime behaviour visible instead of silently skipping it.

## Extension points

The workspace, pin overlay, `MAG_EXTENSIONS`, `--receipt-extra`, lift hook modules and runtime
callbacks (BUILDING.md, "Extending a build") must stay inert by default: with none of them set, the
build, its outputs and its receipts are unchanged. Keep them generic. They name no particular
extension, and code that uses them lives outside this repository.

## Testing

Tests must be synthetic: generate their inputs and never require game files. Tests that compare
against the original program belong outside this repository.

## Commits and releases

Use [Conventional Commits](https://www.conventionalcommits.org/): `feat:`, `fix:`, `perf:`,
`refactor:`, `docs:`, `test:`, `build:`, `ci:` or `chore:`, with `!` (or a `BREAKING CHANGE:` footer)
for incompatible changes. [release-please](https://github.com/googleapis/release-please) reads them on
`main` to keep a release pull request with the next version and `CHANGELOG.md`; merging it tags the
release. Squash-merged pull requests need a conventional title.

Release assets are only the installers CI builds from the release's own source
(`.github/workflows/installers.yml`). An installer may contain this repository's tooling and source
and the pinned ps3recomp source; never a built launcher or engine (`MagRecomp`), generated or lifted
sources, firmware, or anything else derived from game files. Never attach anything else to a release,
and never commit installer builds.
