# Installer packaging and releases

CI builds one frozen installer per supported host, natively on that host: Windows x86_64 on
Windows, macOS arm64 on macOS, and Linux x86_64 on Ubuntu 24.04. Each installer is a PyInstaller one-file executable that embeds CPython, Tcl/Tk, this
directory's fixed source registry and the source kit; players need no Python.

## What an installer contains

- The files listed in `SHIPPABLE_FILES` (`mag_installer/distribution.py`), staged from a clean copy;
  nothing else in the repository is scanned into the stage.
- `release/native-kit.tar.gz`: this repository's `src/`, `tools/`, `cmake/`, `profiles/*.json` and
  top-level build files, plus the four ps3recomp checkouts at the commits pinned in
  `profiles/framework.json`. `release/player-build.json` binds it by size and SHA-256 and names the
  one host the installer is for.
- Python, PyInstaller and packager license notices, `PROVENANCE.json` and a per-member
  size/SHA-256/mode `INVENTORY.json`.

Never: game files, firmware, decrypted executables, generated or lifted sources, a built launcher
or engine, logs or captures. Everything derived from game files is produced on the player's
machine.

## Kinds and names

- `preview`: pull requests and pushes to `main`. Version `preview-<run number>`; archive
  `mag-installer-preview-N-<target>-preview.zip`. Kept as workflow artifacts only.
- `release`: built when release-please creates a release. Version is the release tag
  (`vX.Y.Z`), the repository must be `tekulvw/mag-recomp`, and the frozen launcher must have
  passed its smoke checks. Archive `mag-installer-vX.Y.Z-<target>.zip`.

Linux archives use `.tar.gz` instead of `.zip` to preserve executable permissions.

Each archive has an exact `.sha256` companion. `verify-set` checks the complete per-target set,
writes `SHA256SUMS.txt` and `RELEASE-ASSETS.json`, and `scripts/publish_release.py` re-verifies
the set and attaches it to the existing release without replacing or deleting any asset.

## CI flow

`.github/workflows/installers.yml` is called by the release-please workflow when a release is
created (release events made with the workflow token do not start other workflows), and runs on
its own for pull requests and pushes to `main`:

1. **kit** (Linux): `tools/fetch_ps3recomp.py`, then `scripts/prepare_player_kit.py` writes the kit
   and a manifest per host.
2. **windows**: unit tests; a clang-cl compile check of MAG's sources and the pinned runtime
   (synthetic header only); the installer's own toolchain route (Build Tools detection, managed
   downloads, managed CMake/Ninja building the runtime library, the `MAG.exe` supervisor); then
   the frozen build, smoke checks and a readiness check of the extracted executable.
3. **macos**: unit tests, frozen build, smoke checks and readiness check.
4. **linux**: unit tests, pinned runtime and MAG source compilation with synthetic headers, native
   supervisor relocation/handoff tests, frozen build, smoke checks and readiness check.
5. **verify** (pull requests and `main`): the same `verify-set` check a release runs, on the preview set.
6. **publish** (releases only): `verify-set` and `publish_release.py`.

The kit and its per-host `release/player-build.json` are not part of the fixed source registry:
each installer's kit manifest must name that installer's own target and match its kit's size and
SHA-256, and a release installer must carry one. The registry digest is therefore the same for
every host and matches the release checkout.

To rebuild and attach installers for an existing release (for example after an attach failure),
run the **Installers** workflow by hand from `main` with that release's tag. The kit is built from
the tag's source; the installer tooling and CI scripts come from the commit that runs the workflow,
and each archive records both in `PROVENANCE.json` (`source_revision`, `tooling_revision`).

Smoke checks run the frozen executable with an empty program `PATH`: `--help`, `status`, and the
synthetic installation. A checksum inventory is integrity metadata, not a signature: installers
are unsigned, so macOS Gatekeeper and Windows SmartScreen warn on first launch.

## Update signing keys

`profiles/update-signing.json` (at the repository root) lists the public keys that may sign a
release's `SHA256SUMS.txt` ([launcher updates](../../docs/superpowers/specs/2026-11-28-launcher-updates-design.md#release-signing)).
It ships with no keys, so nothing checks or requires a signature until the user adds them:

```json
{
  "schema_version": 1,
  "repository": "tekulvw/mag-recomp",
  "keys": [
    {"id": "<release key id>", "role": "release", "public_key": "<release key line>"},
    {"id": "<recovery key id>", "role": "recovery", "public_key": "<recovery key line>"}
  ]
}
```

Each `.pub` file minisign writes has two lines:

```
untrusted comment: minisign public key 0123456789ABCDEF
RWQ...(56 base64 characters)
```

- `id` is the 16 hex digits at the end of the **first** line (`0123456789ABCDEF` above).
- `public_key` is the whole **second** line, exactly as written.
- `role` is `release` for the key that signs releases and `recovery` for the offline recovery key.

Only public keys go here. A `.key` file is the secret half: it never enters the repository, CI, an
agent session or a GitHub secret. `tools/check_public_tree.py` refuses minisign secret keys and a
`.key` file beside a `.pub` file.

The list is checked strictly (`mag_installer/minisign.py`, `load_trusted_keys`): only the three
top-level fields and the three per-key fields; `id` must be the key id that the `public_key` line
embeds; roles are `release` or `recovery`; ids and keys are unique; at most two keys per role and
four in all (room for a rotation). Signatures must use minisign's prehashed `ED` algorithm (the
default since minisign 0.11); the legacy `Ed` algorithm is refused.

## Commands

```sh
python3 scripts/release_installer.py check --version VERSION [--preview]
python3 scripts/release_installer.py stage --output PATH
python3 scripts/release_installer.py build --output PATH --version VERSION [--preview]
python3 scripts/release_installer.py verify --artifact PATH
python3 scripts/release_installer.py verify-set --directory PATH --version VERSION --source-revision COMMIT [--preview]
```

`INSTALLER_SOURCE_REVISION` supplies the full source commit and `EXPECTED_TARGET` must equal the
native host. Build-only pins (PyInstaller and its dependencies, with wheel hashes) are in
`requirements-release.txt` and `release/packager-lock.json`; install them with
`--require-hashes --only-binary=:all:` in an isolated environment.
