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

Native replacements under `native/` follow
[docs/native-graphics-reimplementation-method.md](docs/native-graphics-reimplementation-method.md):
specify behaviour in your own words first, then implement from that specification. Never port,
transliterate or paste original code structure.

## Portability

- No absolute or user-specific paths. Take locations from arguments, environment variables or
  repository-relative defaults.
- Retail v1.0 is the only supported build. Do not apply addresses from other builds without evidence.
- Keep unsupported runtime behaviour visible instead of silently skipping it.

## Testing

Tests must be synthetic: generate their inputs and never require game files. Tests that compare
against the original program belong outside this repository.

## Commits and releases

Use [Conventional Commits](https://www.conventionalcommits.org/): `feat:`, `fix:`, `perf:`,
`refactor:`, `docs:`, `test:`, `build:`, `ci:` or `chore:`, with `!` (or a `BREAKING CHANGE:` footer)
for incompatible changes. [release-please](https://github.com/googleapis/release-please) reads them on
`main` to keep a release pull request with the next version and `CHANGELOG.md`; merging it tags the
release. Squash-merged pull requests need a conventional title.

Releases are source-only. Never attach builds, generated sources or anything made from game files
to a release.
