# Installer packaging and releases

CI builds one frozen installer per supported host, natively on that host: Windows x86_64 on
Windows and macOS arm64 on macOS. There is no Linux installer because there is no Linux player
route. Each installer is a PyInstaller one-file executable that embeds CPython, Tcl/Tk, this
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
4. **verify** (pull requests and `main`): the same `verify-set` check a release runs, on the preview set.
5. **publish** (releases only): `verify-set` and `publish_release.py`.

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
