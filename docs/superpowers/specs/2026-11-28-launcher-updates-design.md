# Launcher updates

The user approved the outline of this design on November 27, 2026: MAG finds, downloads and applies
new releases itself, with the player's consent each time. This document specifies it before any
implementation. Nothing here is built yet, and the user reviews this spec before any of it is.
Decisions still open are collected in [Open questions](#open-questions).

An update today is a manual rebuild. You move the old `MAG` folder's contents aside, run the new
installer with the same game folder, firmware and voice choice, and move `home/` and `logs/` back.
Then you put back the access-control entry the fresh build dropped, and delete `MAG-build`. This
design automates that process, adds verification, and makes it safe to interrupt.

## Decisions

These are the user's and bind the design.

1. **Check.** When MAG starts, it asks the GitHub Releases API for the latest stable release (never
   a prerelease or a draft) and compares it with its own version. The check works offline, fails
   silently, respects rate limits, and has an opt-out setting.
2. **Prompt.** If a newer release exists, MAG says: "MAG X is available. Update now (about N
   minutes) or later." It never updates silently, and never during a session.
3. **Download and verify.** MAG downloads this platform's installer asset. It checks the asset's
   SHA-256 against `SHA256SUMS.txt`, and checks a detached ed25519 signature (minisign format) made
   with a key whose public half is built into the launcher. It refuses to run an installer that is
   unsigned or badly signed. Only the user creates and holds the private key. Agents never create
   it, see it or handle it.
4. **Hand off.** The launcher exits and hands off to the new installer, because Windows cannot
   replace a running executable. The installer rebuilds in place with the remembered game folder,
   firmware, voice choice and compile jobs. Executable paths stay the same, so Windows asks no new
   firewall questions. Home, logs, settings and the player's access-control entries are kept.
5. **Rollback.** The previous app is kept. If the build fails, it is restored automatically and MAG
   is started again.
6. **Later:** a minimum client version that the server advertises ("Update required").

## Constraints this design keeps

- **Release assets.** A release carries only what CI builds from the release's own source: the
  three installers and their checksum files (AGENTS.md). The updater downloads the same installer
  a player would download by hand. A built launcher or engine is never published, downloaded or
  cached anywhere but the player's own machine. The only new asset is one signature file, which
  needs an AGENTS.md change ([Release signing](#release-signing), open question 3).
- **The MAG folder is self-contained.** Nothing in it refers to anything outside it
  ([installer/README.md](../../../installer/README.md#what-goes-where)). The remembered build
  inputs are therefore kept in the installer's per-user folder, not in the MAG folder.
- **Player-visible defaults.** Release builds check for updates by default. Developer, preview and
  test-lane runs never do ([Inert outside release builds](#inert-outside-release-builds)).
- **Synthetic tests.** No test needs game files or the real signing key.

## Terms

- **MAG folder:** the installation (`MAG`). It holds the app, `game/`, `home/` and `logs/`.
- **App folder:** the replaceable part of the MAG folder. That is `MAG/MAG.app` on macOS,
  `MAG\MAG\` (with `MAG.exe` and `bin\`) on Windows, and `MAG/MAG/` on Linux.
- **Supervisor:** the small native program the player starts: `MAG.exe`, `MAG/MAG`, or
  `MAG.app/Contents/MacOS/MAG`. The installer compiles it at packaging time
  (`installer/mag_installer/native_package*.py`). It verifies the images, runs the settings window,
  and starts the launcher and the engine. "Launcher" in the decisions above means the supervisor
  together with its settings window.
- **Settings window:** MAG Settings, which opens before every launch. On Windows and Linux it is
  `MagEngine --mag-player-settings-setup`, a process the supervisor runs. On macOS it is the panel
  inside `MagRecomp`, shown before the guest starts.
- **Installer:** the frozen `mag-installer` from a release (`MAG Installer.app` on macOS).

## Overview

```
start MAG ──> supervisor ──> settings window
                               ├─ background: check GitHub (cached, rate-limited)
                               ├─ fetch and verify SHA256SUMS.txt + .minisig (small)
                               └─ banner: "MAG X is available. Update now (about N min) | Later"
                                     │ Update now
                                     v
                               download the installer archive (progress, Cancel)
                               verify its SHA-256 against the signed sums
                               exit with "update requested"
supervisor ──> verify again ──> extract into a fresh folder ──> verify INVENTORY.json
           ──> start `mag-installer update --output <MAG folder> --wait-pid <pid> ...` ──> exit
installer  ──> wait for MAG to exit, take the locks, resolve the remembered inputs, check disk space
           ──> build into MAG-build (build cache reused) ──> package a new app folder
           ──> swap the app folder in place (journaled) ──> self-check ──> ACL check
           ──> on any failure: keep or restore the previous app
           ──> start MAG (new or previous), with a notice
```

## 1. Check

### Where it runs

The check runs in the settings window, on a background thread, when the window opens before a
launch. Nothing else waits for it. When it is done, the window shows a banner if there is an
update. The F10 settings window during play never checks and never offers an update. At most, its
Updates row says "MAG X is available. It will be offered next time you start MAG."

A launch that skips setup makes no check. That covers `--no-prompt`, `MAG_SKIP_SETUP=1`,
`--server` and `--name` on Windows and Linux, and Linux without a desktop. Test lanes and scripts
therefore never see an update and never reach GitHub.

### The request

`GET https://api.github.com/repos/<repository>/releases/latest`, with these headers:

- `Accept: application/vnd.github+json`
- `X-GitHub-Api-Version: 2022-11-28`
- `User-Agent: MAG-launcher` (a generic name, without the version or anything about the player)
- `If-None-Match: <etag>` when the state file has one

GitHub's `latest` endpoint returns only the newest release that is neither a draft nor a
prerelease. The launcher also checks `draft == false` and `prerelease == false` in the response.
The repository comes from the build ([Inert outside release builds](#inert-outside-release-builds)).
Releases may be fetched only from `tekulvw/mag-recomp`.

The response is accepted only when all of these hold:

- `tag_name` matches `v<major>.<minor>.<patch>` (release-please tags, `include-v-in-tag`).
- That version is greater than the launcher's own `MAG_VERSION`, compared numerically by part.
- The assets include `SHA256SUMS.txt`, `SHA256SUMS.txt.minisig` and exactly this platform's archive:
  `mag-installer-<tag>-macos-arm64.zip`, `...-windows-x86_64.zip` or `...-linux-x86_64.tar.gz`.

A release that does not have all three assets yet is treated as "no update". This covers the gap
after release-please publishes a release: CI attaches the installers about 90 minutes later, and
the signature comes after that. The launcher stays silent and checks again later.

Once the release is accepted, the launcher fetches the two small files (`SHA256SUMS.txt` is at most
4 KiB and the `.minisig` at most 1 KiB) and verifies them
([Verification](#3-download-and-verify)). It shows the banner only after that passes. A release
that is published but not signed, or signed with the wrong key, is never offered.

### Offline-safe, silent and rate-limit aware

- Timeouts are 5 s to connect and 15 s in total for the check. Every failure (no network, DNS, TLS,
  an HTTP error or malformed JSON) produces no banner and no dialog. It adds one line to
  `settings.log` (`[update] check failed: <reason>`) and a quiet status in Settings → General →
  Updates ("Couldn't check for updates").
- The check runs at most once every 6 hours. Between checks, the cached result shows the banner.
  **Check now** in Settings bypasses the interval, but never more than once a minute.
- The request is conditional, with the cached ETag. GitHub does not count a `304 Not Modified`
  against the unauthenticated limit of 60 requests per hour per IP address.
- Rate limits are honoured. On `403` or `429` with `X-RateLimit-Remaining: 0`, the next check waits
  until `X-RateLimit-Reset`, at most 24 hours. `Retry-After` is honoured the same way, using the
  existing `parse_retry_after` from `src/player_portal.h`. Players behind one shared address, such
  as a LAN party, therefore back off together.
- The state file is `update-state.json`, next to `settings.json`: `%APPDATA%\MAG`,
  `~/Library/Application Support/MAG`, or `$XDG_CONFIG_HOME/MAG` (`~/.config/MAG`). It holds the
  ETag, the time of the last check, the earliest next check, the last accepted release (tag, asset
  names, sizes, sums hash) and any pending or failed update, keyed by the resolved MAG folder path.
  The file is written atomically through `src/player_preferences_file.cpp` and its reads are
  bounded. A damaged state file is ignored and replaced.

### Opt-out

- **MAG Settings → General → Updates → Check for updates when MAG starts** is on by default in
  release builds. The setting is `check_updates` in `settings.json`. A missing field means on, and
  the file holds `"check_updates": false` only when the setting is off, following the
  `send_reports` convention. With the setting off, MAG makes no request at all. **Check now** still
  works, because the player asked.
- `MAG_UPDATE_CHECK=0` in the environment that starts MAG turns the check off for that launch (for
  labs and kiosks).
- The Updates row shows one of these states: "MAG 0.15.0 is up to date (checked 2 hours ago)",
  "MAG X is available", "Couldn't check for updates", or "Updates are not available in this build".

## 2. Prompt

The banner sits at the top of the settings window, above **Play MAG**:

> **MAG 0.16.0 is available** (you have 0.15.0). [What's new]
> [Update now (about 15 minutes)] [Later]

- **What's new** opens the release's `html_url` in the browser. The launcher opens it only if it
  starts with `https://github.com/<repository>/releases/tag/`. Release notes are never rendered
  inside MAG.
- **About N minutes** comes from the last build of this MAG folder: the installer records the
  build's duration in the app's `runtime.json` (`last_build_seconds`). It is rounded up to 5 minutes
  and includes a download estimate from the asset size. Without a record, it uses a
  per-platform default (Windows: 20 minutes; macOS and Linux: measured in phase C).
- **Later** dismisses the banner for this launch. It comes back at the next launch (open question 6).
- **Play MAG** works as always. The banner never blocks playing.
- If the MAG folder lacks the free space the update needs
  ([Disk space](#disk-space)), the banner replaces **Update now** with "MAG 0.16.0 needs about
  N GB free on <volume> (M GB free now)".
- If another MAG window from this folder is open, **Update now** is disabled: "Close the other MAG
  window to update." See [Concurrency](#concurrency).
- If an earlier update was interrupted or failed, the banner says "Finish updating to MAG 0.16.0
  (about N minutes; earlier work is reused)" or "The update to MAG 0.16.0 failed on <date>. Try
  again | Details". **Details** opens that attempt's log folder.

## 3. Download and verify

### Download

After **Update now**, the banner becomes a progress bar ("Downloading MAG 0.16.0: 41 of 86 MB",
with **Cancel**). The settings window downloads the platform archive into the per-user update
folder, which is owner-only:

| Host | Update folder |
| --- | --- |
| Windows | `%LOCALAPPDATA%\MAG\updates\<tag>\` |
| macOS | `~/Library/Caches/MAG/updates/<tag>/` |
| Linux | `$XDG_CACHE_HOME/MAG/updates/<tag>/` (`~/.cache/MAG/updates/<tag>/`) |

- HTTPS only, with the system trust store, verifying certificate and host name like the portal
  transports (`player_portal_winhttp.cpp`, `player_portal_openssl.cpp`, `player_portal_macos.mm`).
  The portal transports refuse redirects. Asset downloads need them: `browser_download_url`
  redirects to GitHub's asset host. A new GET-to-file method follows at most 3 redirects, and only
  to `github.com`, `objects.githubusercontent.com` and `release-assets.githubusercontent.com`.
  Anything else fails the download. If a download fails, it is retried once through the asset's
  REST URL (`Accept: application/octet-stream`), as the installer already does for tools.
- The size must equal the asset's `size` in the API response and stay at most 256 MiB (the
  installer's own `MAX_ARCHIVE`). The archive is written to `<name>.part` and renamed once it is
  complete and its hash matches.
- **Cancel** stops the download and keeps the `.part` file. The next attempt resumes it with an HTTP
  `Range` request when the server answers `206` with a matching `Content-Range`, and starts again
  otherwise. A verified archive is kept until it is installed or a newer release replaces it.
  Older tags' folders are removed.

### Verification

There are two separate checks. The second runs in the process that executes the installer,
immediately before it does.

1. **Signature over the sums.** `SHA256SUMS.txt.minisig` is a minisign signature
   ([format](https://jedisct1.github.io/minisign/)):
   - Line 2 is base64 of the algorithm `ED`, an 8-byte key id and a 64-byte signature. `ED` means
     the file is prehashed with BLAKE2b-512, which minisign 0.11 and later produce by default. The
     legacy `Ed` algorithm is refused.
   - Line 3 is `trusted comment: <text>`. Line 4 is the 64-byte global signature over the signature
     followed by the trusted comment text.
   - The key id must name one of the public keys built into this launcher. Both signatures must
     verify with that key.
   - The trusted comment must be exactly `mag-recomp release <tag> source <40-hex commit>`, and
     `<tag>` must equal the release's `tag_name`. An old signed `SHA256SUMS.txt` therefore cannot
     be replayed under a newer release, and a release whose comment names another tag is refused.
2. **The archive's hash.** `SHA256SUMS.txt` must hold exactly one line for this platform's archive
   name, in the existing `<sha256>  <name>` form. The downloaded archive's SHA-256 must equal it.
   Every name in the file must contain `<tag>`. If the API response carries the asset's `digest`
   (`sha256:...`), it must match as well. That is a cross-check, not a source of trust.
3. **Extraction.** The supervisor extracts with the system's extractor into a new, empty,
   owner-only folder: `tar.exe` (bsdtar, which ships with Windows 10 1803 and later),
   `/usr/bin/ditto -x -k`, or `/usr/bin/tar -xzf`. It then checks the result against the archive's
   own `INVENTORY.json` (written by `releasing.package_payload`): every file has exactly the listed
   size, SHA-256 and mode, there are no extra files, no links and no special files, and the archive
   holds exactly one top-level folder named after the archive. `INVENTORY.json` can be trusted
   because the archive it is in was verified in step 2. On macOS the supervisor also runs
   `codesign --verify --strict` on the extracted `MAG Installer.app`.
4. **Version binding.** The installer's `PROVENANCE.json` must say `kind: release`, `version: <tag>`,
   and the `source_revision` from the trusted comment. The installer checks this again itself when
   it is started with `--expect-version` ([Hand off](#4-hand-off)).

Any failure deletes that tag's update folder. It records "The download of MAG X could not be
verified" in the state file and logs the reason. The installer is never run, and the banner offers
**Try again**. A verification failure is never retried silently in a loop.

**Ed25519 in the launcher.** CNG on Windows and the Security framework on macOS have no C API for
Ed25519. The launcher therefore vendors [Monocypher](https://monocypher.org) (CC0-1.0 or
BSD-2-Clause, audited in 2020) for BLAKE2b-512 and Ed25519 signature checks on all three platforms.
It uses the core and the optional `monocypher-ed25519` (SHA-512) file, and only for verification.
The vendored files go under `src/third_party/monocypher/` with their licence, and their source
hashes are recorded in NOTICE.md. A synthetic test runs RFC 8032's test vectors.

### Release signing

**What is signed:** `SHA256SUMS.txt`, once per release. A new release asset
`SHA256SUMS.txt.minisig` sits beside it. That one signature covers all three installers through
their hashes, and players who download by hand can check it with `minisign -V`.

`publish_release.py` and `verify-set` learn about the signature file. `verify-set` accepts the set
with or without it. When it is present, they check it against the public keys in
`profiles/update-signing.json`. `publish_release.py` still never replaces or deletes an asset, and
it allows exactly one later upload of `SHA256SUMS.txt.minisig`.

The private key can be held in one of two ways, and the user chooses (open question 1):

- **A. Offline, by the user (recommended).** CI publishes the release as it does today. The user
  then runs `tools/sign_release.py --tag vX.Y.Z --key <path>` on their own computer:
  1. It downloads `SHA256SUMS.txt` and `RELEASE-ASSETS.json` with `gh release download`.
  2. It checks that `RELEASE-ASSETS.json` names exactly this tag and this `kind: release`, and that
     its hash for `SHA256SUMS.txt` and the archive lines agree.
  3. It prints the lines to be signed.
  4. It runs `minisign -S -s <key> -m SHA256SUMS.txt -t "mag-recomp release <tag> source <sha>"`.
     minisign itself asks for the password, so the script never reads the key or the password.
  5. It verifies the result with the committed public key.
  6. It uploads the signature with `gh release upload <tag> SHA256SUMS.txt.minisig`.

  Until the user signs, launchers stay silent. That fits the existing rule that releases are human
  actions. If the user later turns on GitHub's immutable releases, signing has to move before
  publication, and `publish_release.py` already requires a mutable release.
- **B. In CI, with an Actions secret.** A `sign` job runs between `verify` and `publish` in
  `installers.yml`, for releases only:
  - It runs on a GitHub-hosted `ubuntu-latest` runner, never on the shared self-hosted `mag-recomp`
    runner.
  - It uses the protected environment `release-signing`, which has the user as its required
    reviewer and allows only `main`.
  - It downloads a minisign release binary pinned by SHA-256, runs the same `verify-set`, and signs
    with the secret `MAG_UPDATE_SIGNING_KEY` (the password-encrypted key file). The password,
    `MAG_UPDATE_SIGNING_PASSWORD`, is passed on standard input. Phase B confirms that minisign
    reads it there, using an ephemeral key.
  - `publish` attaches the signature with the installers.

  This saves a manual step. But anyone who controls the user's GitHub account can approve the
  environment, so it does not protect against a compromised account
  ([Threat model](#threat-model)).

Whichever option is chosen, every preview run (pull requests and `main`) signs its preview set with
an ephemeral key generated in the job and verifies it. The signing and verification code is
therefore exercised constantly without the real key. Preview installers embed no public key, so the
apps they build never check for updates.

**Key steps for the user** (from a terminal on the user's own computer, never in an agent session).
These are the same for options A and B:

```sh
# 1. Install minisign (0.11 or later; check with `minisign -v`).
brew install minisign                 # macOS; Linux: apt install minisign; Windows: winget install jedisct1.minisign

# 2. A private folder, ideally on an encrypted external volume.
mkdir -m 700 "$HOME/mag-release-signing" && cd "$HOME/mag-release-signing"

# 3. The release key. Choose a strong password when asked.
minisign -G -p mag-release.pub -s mag-release.key

# 4. A recovery key with a different password (open question 2). Store it apart from the release key.
minisign -G -p mag-recovery.pub -s mag-recovery.key
chmod 600 mag-release.key mag-recovery.key

# 5. Check that signing works.
echo test > t.txt && minisign -S -s mag-release.key -m t.txt -t "test" && minisign -V -p mag-release.pub -m t.txt && rm t.txt t.txt.minisig

# 6. Back up both .key files and both passwords offline, for example in a password manager and on an
#    encrypted USB drive. Losing the key means shipping a new public key through a release signed
#    with the recovery key.

# 7. The public halves are safe to share. Print them and paste the second line of each file into
#    profiles/update-signing.json (or hand both .pub files to an agent to open that PR).
cat mag-release.pub mag-recovery.pub
```

Only for option B, after step 7:

```sh
# A protected environment: in GitHub, Settings → Environments → New environment "release-signing".
# Set Required reviewers to yourself and Deployment branches to "Selected: main". Then:
gh secret set MAG_UPDATE_SIGNING_KEY --env release-signing --repo tekulvw/mag-recomp < mag-release.key
gh secret set MAG_UPDATE_SIGNING_PASSWORD --env release-signing --repo tekulvw/mag-recomp   # type the password at the prompt
```

The recovery key never goes into CI. Agents must never run `minisign -G` or `-S` with a real key,
read a `.key` file, or ask for its password. `tools/check_public_tree.py` gains a rule that refuses
minisign secret-key files: their `untrusted comment: minisign encrypted secret key` header, or a
`.key` file beside a `.pub` file.

**Public keys in the launcher.** `profiles/update-signing.json` lists the repository and the
trusted keys, each with its key id, the base64 public key and a role (`release` or `recovery`). The
source kit already ships `profiles/*.json`. At packaging time, `native_package*.py` compiles the
keys into the supervisor (`supervisor_config.h` on Windows and Linux, the macOS launcher source)
and the settings window. This happens only when the installer doing the packaging is a release
installer ([Inert outside release builds](#inert-outside-release-builds)). Until the user adds a
key, the file lists none and updates stay off everywhere. Early phases can therefore merge without
the key.

**Key rotation.** To rotate, a release signed by the current key ships a new key list. The launcher
accepts either key in the list. If the release key is lost or compromised, the next release is
signed with the recovery key and drops the old key from the list. Launchers too old to know the
recovery key need one manual installer download.

## 4. Hand off

1. After the download is verified, the settings window writes `update-ready.json` in the tag's
   update folder. It holds the tag, archive name, archive SHA-256, sums SHA-256, MAG folder and
   current version. The window then exits with a new code, "update requested", which is distinct
   from Play, Cancel and the existing 90 handoff.
2. The supervisor reads `update-ready.json`, repeats verification steps 1–4 on the files as they are
   on disk, and extracts the installer. On Windows it hashes with a handle opened without write
   sharing and keeps that handle until extraction ends.
3. The supervisor starts the installer, detached, with the player's normal environment (not the
   fixed game environment):

   ```
   <installer> update --output <MAG folder> --from-version 0.15.0 --expect-version v0.16.0 \
               --wait-pid <supervisor pid> --relaunch
   ```

   - **Windows:** `CreateProcessW` with `CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP`.
     The supervisor's job object must not end the installer when the supervisor exits.
   - **macOS:** `posix_spawn` with `POSIX_SPAWN_SETSID` of `MAG Installer.app/Contents/MacOS/mag-installer`.
   - **Linux:** fork, `setsid`, then exec, keeping the desktop session variables.

   The supervisor writes a log line and exits 0. The supervisor never runs the build itself.
4. The installer checks that its own `PROVENANCE.json` version equals `--expect-version`. It waits up
   to 60 s for `--wait-pid` to exit, then takes the locks ([Concurrency](#concurrency)). If MAG is
   still running, the update window says "Close MAG to continue", with **Retry** and **Cancel**.

`mag-installer update` is also the command-line form for labs, and the wizard offers the same thing
([Players who installed before this feature](#players-who-installed-before-this-feature)).

### In-place update in the installer

Today `install()` refuses an existing output. Update mode is a new transaction beside it, and it
shares the build steps:

1. **Validate the MAG folder.** It must be a complete layout-2 folder: the app folder, `game/`,
   optionally `home/`, `logs/` and `launch-error.log`, and nothing unknown. Its app's
   `runtime.json` gives the current version. A layout-1 folder is migrated first, by the existing
   `_migrate`.
2. **Resolve the inputs** ([Remembered build inputs](#remembered-build-inputs)) and check disk space.
3. **Build** into `MAG-build` with the normal steps and the per-user build cache. If `MAG-build`
   already holds this update's own incomplete build, the existing retry rules continue it. If it
   holds a complete earlier build, its logs are archived under `logs/attempts/` and its stages are
   cleared. Anything unknown stops the update with nothing changed.
4. **Package** a new app folder into `MAG-build/package`. Packaging normally stages `game/` as well.
   In update mode it compares the staged game tree with the MAG folder's `game/` (names, sizes and
   SHA-256, with hard links compared by file identity). If they are the same, the existing `game/`
   is kept. If not, the new tree is published by a second journaled swap.
5. **Swap**, journaled in `MAG-build/build.json` (`update.phase`):
   1. `staging`: move the new app folder to `<MAG folder>/.MAG-new`. This is on the same volume,
      because `MAG-build` sits beside the MAG folder.
   2. `swapping`: rename the app folder to `<MAG folder>/.MAG-previous`, then rename `.MAG-new` to
      the app folder's name. The executable paths are then exactly what they were.
   3. `verifying`: the new supervisor's `--mag-verify-install` checks the receipts, images and game
      identities and exits. It shows no window, starts no game image and opens no socket. On
      Windows the installer also runs the ACL check ([Windows](#windows)).
   4. `keeping`: move `.MAG-previous` to `MAG-build/previous/` with a `previous.json` that records
      its version, then update the install record.
   5. `done`: remove the build workspace (`build-kit`, `build-work`, `managed-tools`). Keep
      `MAG-build/logs` and `MAG-build/previous`. The build cache and the downloads stay in the
      per-user cache, so the next update is as fast as this one.

   `home/`, `logs/` and `game/` are never moved or rewritten. Settings, the portal link, reports and
   the Windows shader and pipeline caches live outside the MAG folder and are never touched.
6. **Relaunch.** The installer starts the supervisor at its usual path, detached, with a one-time
   notice ("MAG was updated to 0.16.0"), and exits. An unchecked **Start MAG when done** skips this.

## 5. Rollback

The previous app is never changed until a complete, verified new app exists. Rollback therefore
depends on when the failure happens:

| Failure | What happens |
| --- | --- |
| Before the swap (inputs, tools, build, package, a cancel or a crash) | The previous app was never touched. The journal records `incomplete`. MAG starts again with "The update to MAG 0.16.0 did not finish. MAG 0.15.0 is unchanged. [Details]". |
| During the swap | The journal knows which renames happened and reverses them: `.MAG-previous` goes back to the app folder's name, and `.MAG-new` returns to `MAG-build/package`. |
| `--mag-verify-install` or the ACL check fails | The swap is reversed in the same way, and MAG starts the previous app with the failure notice. |
| Interrupted by power loss or a kill | Whoever runs the installer next for this MAG folder finishes or reverses the swap before anything else: the launcher's "Finish updating" banner, or the wizard. |

Between the two renames in step 5.2, the app folder does not exist for a moment. A power cut at
exactly that point leaves the player's shortcut broken until the installer runs again. The cached
installer in the update folder can do that, and a manually downloaded one can too. The design
accepts this rare case, because Windows cannot swap directories atomically. The README documents
the recovery.

After a successful update, `MAG-build/previous/` holds one previous app (about 300 MB on Windows).
Each later update replaces it. Whether Settings offers **Go back to MAG 0.15.0** is open question 5.
Deleting `MAG-build` remains safe, and only removes that rollback copy.

## Remembered build inputs

The installer already remembers the compile job count and the voice choice per user, in
`preferences.json` in its cache folder. The game folder and firmware paths are only in
`MAG-build/build.json`, which players are told they may delete. This design adds two records:

- **`installs.json`** beside `preferences.json` (`%LOCALAPPDATA%\MAG Installer\Cache`,
  `~/Library/Caches/MAG Installer` or `~/.cache/mag-installer`). It is owner-only on macOS and
  Linux and written atomically. It holds one entry per resolved MAG folder path, with the host, the
  game folder path, the firmware path, the version built, the build's duration per stage and the
  peak size of `MAG-build`. The installer writes it after every successful build or update. It
  stores no hashes: each update checks the inputs against the kit's reviewed identities
  (`input_identities`), as every build already does. The paths never leave the computer and never
  enter the MAG folder.
- **Build options in the app.** Packaging writes `build_options` into the app's `runtime.json`:
  `voice`, `game_controls` (false when `MAG_GAME_CONTROLS=0`) and the x86-64 level override. These
  hold no paths, so the MAG folder stays movable. An update therefore keeps exactly the options the
  current app was built with, even if `preferences.json` changed since, for example because
  another MAG folder was built without voice chat. The compile job count comes from
  `preferences.json`, because it depends on the computer and not on the build.

The installer looks for the inputs in this order:

| Input | Where it looks |
| --- | --- |
| Game folder | (1) the recorded path, if every file passes the reviewed-identity check; (2) the MAG folder's own `game/`, which holds the full `PS3_GAME` tree with both decrypted executables and is checked at every start. In update mode it is read-only, and the rule "source must be separate from output" is relaxed for exactly `<output>/game`; (3) RPCS3 detection (`rpcs3_detect.py`) |
| Firmware module | (1) the recorded path; (2) `MAG-build`'s copy, if it still exists; (3) RPCS3 detection of a decrypted `dev_flash/sys/external/libsre.prx` (open question 4) |
| Voice, controls, x86 level | the app's `build_options`; for an app made before that field existed: the old `MAG-build/build.json` `voice` flag if present, then `preferences.json` |
| Compile jobs | `preferences.json`, else this computer's default |

If anything is still missing, or the new release needs an input the old one did not, the update
does not guess. It opens the wizard in update mode with the fields it found filled in and a message:
"MAG 0.16.0 needs your firmware file (libsre.prx) again. Choose it to continue the update." The
"Getting your files ready" checklist shows the RPCS3 steps as usual. If the game files have moved
and the MAG folder's `game/` is gone too, the player chooses the game folder again. Nothing is
built until every identity checks.

## Disk space

An update needs, on the MAG folder's volume: the peak size of `MAG-build`, plus one more app
folder (the old one is kept until the next update), plus a 2 GiB margin. On the per-user cache's
volume it needs the archive size plus room for the build cache to grow (at most 20 GiB, which it
already enforces itself). The peak comes from `installs.json`. Without that record, it uses a
per-platform constant measured in phase A. Today's guidance is "roughly 40 GB" (installer
`docs/testing.md`), and the build cache usually makes a rebuild smaller.

The check runs three times:

- in the banner, as a quick free-space read, before anything is downloaded;
- in the installer, before the build starts;
- again after the managed tools are downloaded.

A shortfall says how much is needed and where. With a shortfall, nothing is built and nothing is
changed.

## Progress during the rebuild

The installer's update window (Tk, in the same process) is small and focused:

```
Updating MAG to 0.16.0                                     about 9 minutes left
[████████████░░░░░░░░░░░░░]
 ✓ Checking your game files
 ✓ Getting build tools
 ✓ Generating sources            (reused from the build cache)
 ▸ Compiling                     1,214 of 2,050
   Packaging
   Translating shaders           (Windows)
   Finishing
MAG 0.15.0 stays as it is until the update finishes. You can keep using your computer.
[Cancel update]  [Show details]  [Open logs]          [✓] Start MAG when done
```

- Stages come from the existing `stage-start` events. The compile step is determinate wherever the
  build reports its step count. The overall bar is weighted by the last build's stage durations
  from `installs.json`, with equal weights when there is no record.
- **Show details** shows the human log tail, which the wizard already prints.
- While the build runs, the installer keeps the computer awake: `SetThreadExecutionState` on
  Windows, an `IOPMAssertion` (through `caffeinate -i -w <pid>`) on macOS, and `systemd-inhibit`
  when it is available on Linux. The display may still sleep.
- On Windows the update window replaces the console window that the wizard opens. The console
  output goes to the log.

## Cancel and resume

- **During the download:** **Cancel** keeps the `.part` file. The next **Update now** resumes it.
- **During the build:** **Cancel update** uses the existing `Cancellation` token. The journal records
  `incomplete`, `MAG-build` is kept, and the window says "Update cancelled. MAG 0.15.0 is
  unchanged." with **Start MAG** and **Close**. The next launch shows the banner "Finish updating
  to MAG 0.16.0 (about N minutes; earlier work is reused)". It reuses the already verified installer
  in the update folder, after verifying it again, and the retry path and the build cache.
- **Closing the window** is the same as **Cancel update**, after a confirmation.
- **After a crash or power loss:** handled as described in [Rollback](#5-rollback).
- **When a newer release appears** while an update is pending, the pending one is dropped, unless
  it is mid-swap. In that case it is first finished or reversed. The banner then offers the newer
  release, and its build reuses the cache.

## Concurrency

- **A session lock.** The supervisor holds a shared kernel lock on `<MAG folder>/home/.session-lock`
  for its whole life: `LockFileEx` on Windows, `flock` on macOS and Linux, the same kind of lock as
  `pipeline._exclusive_lock`. The installer's update mode needs that lock exclusively, so it cannot
  swap while any MAG process from that folder runs, including one in a match. The swap's renames
  would also fail on Windows while an image is running. The lock gives a clear message instead of
  that failure.
- **Two launchers.** A second MAG window from the same folder sees that the lock is shared by
  someone else and disables **Update now**, with the reason. Two windows never download at the
  same time: each tag's update folder has its own lock, and the second window waits for or reuses
  the first's verified archive.
- **The installer is already open.** The existing player lock (`.MAG.player-lock` beside the MAG
  folder) is taken by both the wizard and update mode, so a wizard build and an update never
  overlap.
- **Two MAG folders on one computer** update independently. `installs.json` keys them by path, and
  the build cache is already shared safely under its own lock.
- **MAG started during the update** waits at the session lock and says "MAG is being updated to
  0.16.0", offering to show the update window. It does not start the old app halfway through a
  swap.

## Players who installed before this feature

A MAG built before this feature has no update check, so its players never see the banner. They
update once by hand, and the first release with this feature makes that step much easier than
today's manual move:

- When the wizard's **Install inside** location already holds a complete MAG folder, the wizard no
  longer stops with "Output already exists". The main button becomes **Update MAG (0.14.0 →
  0.16.0)**, or "(an earlier MAG → 0.16.0)" for a folder whose `runtime.json` has no version. The
  game folder and firmware fields are filled in by detection as usual, and voice chat comes from
  `preferences.json`. The 0.15.0 installer already remembers it.
- It runs the same in-place update: same paths, `home/` and `logs/` untouched, the previous app kept
  in `MAG-build/previous/`, and the ACL kept. It just has no launcher handoff. `mag-installer build
  --update --output <MAG folder> --source ... [--firmware ...]` is the command-line form, for labs.
- After that, MAG updates itself. The release notes and the README say so plainly.
- Layout-1 folders (build files inside `MAG`) are first migrated by the existing `_migrate`, then
  updated.

## Platform specifics

### Windows

- **Firewall.** Windows Defender Firewall rules match the program's path. `MAG\MAG\bin\MagEngine.exe`
  keeps its path, so the existing rules apply and Windows asks nothing. The staged app at `.MAG-new`
  never runs before the swap. `--mag-verify-install` runs only after the swap, and it opens no
  socket. The acceptance run compares the rules that name the MAG folder before and after.
- **ACL (depends on the separate fix in progress).** A fresh build today leaves `MAG\` and
  `MAG\MAG\` without the player's own full-access entry. In the manual updates, that entry had to be
  put back by hand. This design depends on that fix, which another change is making: a build gives
  those folders the player's own entry. On top of it, update mode keeps `MAG\` in place, so its
  descriptor is untouched. It records the SDDL of `MAG\` and the app folder before the swap. After
  the swap, it gives the new app folder the previous app folder's explicit entries and lets its
  children inherit. It then checks that the player's SID has full access on `MAG\`, the app folder
  and every file under it. A difference rolls the update back. The check uses the fix's own helper
  instead of a second implementation.
- **SmartScreen and Mark of the Web.** Files the launcher downloads with WinHTTP get no
  `Zone.Identifier`, and the launcher adds none. The installer then starts without a SmartScreen
  prompt. The ed25519 signature is the trust check, not Authenticode, and the installer is not
  Authenticode-signed today. Antivirus may still scan or hold the file. A blocked start reports the
  Windows error and keeps the old MAG.
- **UAC** appears only if the pinned toolchain needs a Visual Studio Build Tools change. The update
  window says so before the prompt appears.
- **Path limit.** The existing location check (ASCII, at most 83 characters) already passed for the
  existing folder. It is checked again.

### macOS

- **Quarantine and Gatekeeper.** The download uses MAG's own `NSURLSession` code. MAG's bundle does
  not set `LSFileQuarantineEnabled`, so the archive gets no `com.apple.quarantine` attribute. Gatekeeper
  assesses only quarantined files, so the extracted ad-hoc-signed installer starts without the
  "cannot verify the developer" dialog that a manual download shows. The ed25519 signature is the
  trust check in its place. If a quarantine attribute is present anyway (some security tools add
  one), the supervisor removes it from the extracted folder only after every verification step has
  passed. The design does this on purpose, and player-settings.md documents it.
- **Code signing.** The installer is ad-hoc signed and not notarized. The supervisor runs `codesign
  --verify --strict` on it after extraction. The rebuilt `MAG.app` is ad-hoc signed on the player's
  computer, as today. Its code-signing identity is its hash, so it changes with every build. macOS
  therefore asks again for permissions tied to that identity, such as the microphone for voice
  builds, after each update. A manual rebuild does the same today. Avoiding this needs a stable
  signing identity (open question 9).
- **Command Line Tools.** If a release needs newer Apple Command Line Tools, the existing
  `ensure_apple_sdk` flow opens Apple's installer, and the update window explains why.
- **The app path** `MAG/MAG.app` stays the same, so Dock items and Finder aliases keep working.

### Linux

- **Extraction.** The `.tar.gz` keeps its executable bits, and `INVENTORY.json` checks the modes.
- **Desktop.** The update window needs a desktop. The settings window, which offers the update, is
  shown only on a desktop, so this always holds. Without one there is no check and no prompt.
- **New packages.** If a release needs new Ubuntu packages, the existing PolicyKit request appears,
  and the update window says why first. Without PolicyKit, the update stops with the existing
  `apt-get` line and changes nothing.
- **Folder modes.** The owner and modes of the MAG folder and `home/` are untouched, because they
  never move.

## Inert outside release builds

Updates are on only when all of these hold. Otherwise the launcher makes no request and shows
"Updates are not available in this build":

1. A **release installer** packaged the app. Its `PROVENANCE.json` has `kind: release` and
   `source_repository: tekulvw/mag-recomp`. Packaging then writes an `updates` block into the app's
   `runtime.json` and the supervisor's config: the repository, the trusted public keys and the
   platform's asset name pattern. Preview installers (pull requests and `main`), running the
   installer from source (`python3 -m mag_installer`) and developer builds (BUILDING.md, CMake)
   write no such block. Their outputs and receipts are byte-for-byte what they are today.
2. `profiles/update-signing.json` lists at least one key.
3. `MAG_VERSION` is a release version. It is not `unknown`.
4. Setup is shown, so the launch was not started with `--no-prompt`, `MAG_SKIP_SETUP=1` or the
   direct-start arguments.
5. Neither `check_updates: false` nor `MAG_UPDATE_CHECK=0` is set.

For the lab and tests only, a preview installer accepts `MAG_UPDATE_TEST_CHANNEL=<file>` at build
time. The file names a test public key and an API base on `127.0.0.1` or `localhost`. A release
installer refuses it. The API base accepts plain `http` only for those hosts, following the rule
for `portal_url`. This switch lets the acceptance runs below update a real installation from a
fake release server without the real key.

This feature is not a ps3recomp switch, so `docs/ps3recomp-flags.md` is unchanged. `check_updates`,
`MAG_UPDATE_CHECK` and `MAG_UPDATE_TEST_CHANNEL` are documented in player-settings.md and the
installer docs.

## Threat model

| Threat | What stops it | What remains |
| --- | --- | --- |
| **A compromised GitHub account or token** publishes a release with malicious installers | With option A, the attacker has no offline key, so no valid signature exists and launchers refuse the release. With option B, a token without environment approval cannot reach the secret. | With option B, whoever controls the user's account can approve the `release-signing` environment and sign anything. This is the main reason option A is recommended. |
| **Asset replacement** on an existing release | The SHA-256 is bound by the signed sums. `publish_release.py` never replaces assets. | None for signed releases. |
| **MITM** (a hostile network, or a TLS-intercepting proxy with its own root) | TLS with the system trust store. Even if TLS is bypassed, a changed archive fails the signature. Redirects go only to allowlisted hosts. | An attacker can block updates (a freeze), but MAG keeps working on its current version. |
| **Replay or downgrade** (an old signed release served as new) | The trusted comment binds the tag, the tag must equal `tag_name`, and the version must be greater than the installed one. | None. |
| **Malformed responses** (huge JSON, path traversal in the archive, links) | Bounded reads and sizes; exact asset names; the system extractor (which refuses absolute paths and `..`) into an empty folder, then an exact `INVENTORY.json` match with no links or extra files. | None known. |
| **A tampered local cache** (`.part`, the archive or an extracted folder changed after the check) | Everything is verified again just before execution, by the process that executes. On Windows the file is held without write sharing from hashing until extraction, and extraction goes into a new random folder. | Malware already running as the player can change anything the player can, including MAG itself. That is out of scope. |
| **A compromised build** (the self-hosted runner or a pinned dependency builds a bad kit) | Not by signatures: with either option, the signer signs what CI built. The existing pins, the kit inventory, `verify-set`, and the user's review at release time apply. | Reproducible builds are later work. Option A lets the user compare `RELEASE-ASSETS.json` with the CI run before signing. |
| **A lost or leaked signing key** | A recovery key built into launchers; rotation through a signed release. | Launchers too old to know the recovery key need one manual installer download. |
| **Privacy** | One unauthenticated GET to `api.github.com` at most every 6 hours, with a generic User-Agent and no identifiers. Opt-out setting. | GitHub sees the player's IP address, as it does for any download. player-settings.md says so. |

## Later: "Update required"

Once updates have shipped, the server may advertise a minimum client version. The design goal is
recorded here, and its details are left to that later spec (open question 10):

- The portal, or the Medius login reply, advertises `min_client_version` for each server. The
  launcher already talks to the portal before each phase on Windows and Linux. On macOS the game
  fetches the ticket itself.
- Below the minimum, the banner becomes **Update required to play online: MAG X**, with **Update
  now** and **Quit**. Training and offline play may stay available.
- The server keeps the authority. A client that is too old is refused at login with a clear reason,
  so an old launcher that has no such banner still gets a readable message.

## Tests

All tests are synthetic, need no game files and never use the real key.

- **The C++ core** (`src/player_update.{h,cpp}`, standard C++ with the transport injected, like
  `player_portal.cpp`, in `tests/`):
  - version parsing and comparison;
  - release JSON acceptance: draft, prerelease, a bad tag, missing assets, wrong names, oversized
    fields;
  - the state file: interval, ETag, `304`, `403` and `429` with reset and `Retry-After`, damaged
    files;
  - parsing `SHA256SUMS.txt`;
  - minisign parsing and verification: test vectors, and signatures made in the test with a key it
    generates (good, bad, unknown key id, legacy `Ed`, changed trusted comment, wrong tag);
  - the redirect allowlist, size limits, and resume with `206` and with `200`.
- **The installer** (`installer/tests`, Python):
  - update mode on a synthetic MAG folder, with the build and package steps faked as existing
    layout tests do;
  - every swap phase interrupted, then recovered;
  - rollback when verification fails;
  - `home/`, `logs/` and `game/` byte-identical before and after (hash comparison);
  - lock contention: session lock and player lock;
  - each fallback in the input order;
  - disk-space refusals;
  - legacy folders: no `installs.json`, no version, layout 1;
  - `--expect-version` mismatch;
  - `installs.json` and `build_options` written and read.
- **Signing tools:**
  - `tools/sign_release.py` with a fake `gh` and a fake `minisign`, checking the commands and their
    order, and that it never reads the key;
  - `verify-set` and `publish_release.py` with and without the signature;
  - check_public_tree's new secret-key rule.

  Signatures are made with a small RFC 8032 Ed25519 reference implementation in the test helpers,
  plus `hashlib.blake2b`. No new dependency is needed.
- **A fake release server** (`tests/fake_release_server.py`): a `127.0.0.1` HTTP server that serves
  `/repos/<owner>/<repo>/releases/latest`, the assets and a redirect hop, with switches for:
  - no update and newer;
  - prerelease;
  - missing signature and bad signature;
  - wrong hash;
  - rate limited;
  - slow, truncated and resumable downloads;
  - a hostile archive.

  The C++ tests run the platform transports against it through the test-channel switch. On
  Windows, a test checks ACL preservation on a temporary folder with synthetic entries.
- **CI:**
  - The preview runs sign with an ephemeral key and verify, as described in
    [Release signing](#release-signing).
  - Window tests stay behind `MAG_INSTALLER_WINDOW_TESTS=1`, so no window opens on a developer's
    computer. Agents do not run window tests on the user's Mac.
- **Acceptance**, separate from CI and never with game files in the repository: on the Windows lab
  PC, and then on macOS and Linux, a preview installer built with the test channel installs MAG.
  The fake server then offers a newer signed preview set. The run checks:
  - the banner;
  - the download;
  - the handoff;
  - a build that reuses the cache;
  - firewall rules unchanged and no prompt;
  - the ACL diff is 0;
  - `home/`, `logs/`, `%APPDATA%\MAG` and the shader cache are byte-identical;
  - relaunch;
  - a forced build failure rolls back;
  - a cancel, then a resume.

  These are the checks `local/userbuild-*/verify` did by hand.

## Docs to update

- `installer/README.md`: a new "Updating MAG" section covering the banner, the in-place update,
  where the previous app is kept, `installs.json`, and recovery after an interrupted swap. Also
  "What goes where" (`MAG-build/previous`, the update folder) and the wizard's **Update MAG**.
- `docs/player-settings.md`: the Updates row, `check_updates`, `MAG_UPDATE_CHECK`, what the check
  sends and how often, and the macOS quarantine note.
- `installer/docs/release-packaging.md`: the signature asset, the signing flow (A or B), the
  preview-run ephemeral signing, and key rotation.
- `installer/docs/design.md`: the update transaction, its journal phases and the relaxed
  source-in-output rule.
- `installer/docs/testing.md`: what to report from an update.
- `AGENTS.md`: release assets include `SHA256SUMS.txt.minisig`, and agents never create, read or
  handle signing keys. This needs the user's approval.
- `BUILDING.md`: developer builds have no updater.
- `NOTICE.md`: Monocypher.
- `CHANGELOG.md` is generated by release-please.

## Implementation plan

Each phase is one PR from current `main`, with a conventional title. A phase is stacked with
`gh stack` only where it depends on the previous one. None of them changes behaviour until the user
adds a public key.

0. **This spec** (`docs(spec): ...`). The user reviews it.
1. **The ACL fix** (separate, already in progress): a build keeps the player's own entry on `MAG\`
   and `MAG\MAG\`. Phase A depends on it on Windows.
2. **A. `feat(installer): update an existing MAG folder in place`:**
   - update mode: the transaction, the journal, the swap, keeping the previous app, rollback,
     locks, disk check and the inputs fallback;
   - the wizard's **Update MAG**, and `build --update`;
   - `installs.json` and `build_options`;
   - `--mag-verify-install` in all three supervisors;
   - the session lock;
   - measuring the peak size.

   No network and no launcher UI. It is useful at once: it replaces the manual process, and it is
   what players who installed before this feature use once.
3. **B. `feat(release): sign the installer set`:**
   - `profiles/update-signing.json`, empty until the user adds keys;
   - `tools/sign_release.py` (option A), or the `sign` job (option B);
   - the signature in `verify-set` and `publish_release.py`;
   - ephemeral signing in preview runs;
   - the check_public_tree secret-key rule;
   - the AGENTS.md wording.

   This needs open questions 1–3 answered. The user then generates the keys and adds the public
   halves (the steps above).
4. **C. `feat(launcher): check for updates`:**
   - `src/player_update` core and the vendored Monocypher;
   - the GET-with-redirects transport method on the three platforms;
   - the Updates row and `check_updates` in the three settings windows;
   - the state file;
   - the `updates` block in packaging for release installers only;
   - the test channel and the fake release server.

   With a key present, the banner appears, but **Update now** only opens the release page.
5. **D. `feat(launcher): download, verify and hand off updates`:**
   - the download with progress, cancel and resume;
   - the supervisor's re-verification, extraction and detached start;
   - `mag-installer update --wait-pid --relaunch --expect-version`;
   - the update window, keeping the computer awake, and the relaunch notices.
6. **E. `docs: launcher updates`.** The docs above, plus the acceptance runs on all three platforms,
   recorded outside the repository. The first release with phase D is cut only after the Windows
   lab acceptance passes (open question 12).
7. **Later:** "Update required" (a separate spec, with the server repository), Go back to the previous
   version (if chosen), and an update-failure report (if chosen).

## Open questions

1. **How the key is held:** offline by you, with `tools/sign_release.py` after each release
   (recommended, because it protects against a compromised GitHub account), or an Actions secret in
   a protected `release-signing` environment (automatic, but not safe against account compromise)?
2. **A recovery key:** generate a second offline key now and build it into launchers, so a lost or
   leaked release key does not strand players (recommended)?
3. **AGENTS.md:** may `SHA256SUMS.txt.minisig` join the release assets? The rule currently says
   "Never attach anything else". `SHA256SUMS.txt` and `RELEASE-ASSETS.json` already go beyond its
   wording, so the change would name all of them.
4. **Firmware for later updates:** re-locate `libsre.prx` each time (the recorded path, then RPCS3
   detection, then ask), or keep a private owner-only copy beside `installs.json` so updates never
   ask for it again? It is the player's own file and stays on their computer either way.
5. **The previous app:** keep it in `MAG-build/previous/` (proposed), and should Settings offer **Go
   back to MAG 0.15.0**? Going back is limited by settings written by the newer version, which an
   older one may treat as unsupported.
6. **Later:** ask again at every launch (proposed), or at most once a day? Add **Skip this
   version**?
7. **Failed updates:** send an `update_failed` report to the portal when reports are on and an
   account is linked?
8. **Defaults:** is a 6-hour check interval, on by default in release builds, right?
9. **macOS signing:** accept that macOS asks for the microphone again after each update (as manual
   rebuilds do now), or invest in a stable signing identity (Developer ID with notarization, or a
   per-computer local identity)?
10. **"Update required":** advertised by the portal or by Medius, and should Training stay playable
    below the minimum?
11. **Test lanes:** confirm that `--no-prompt` and `MAG_SKIP_SETUP=1` never check (proposed), so lab
    automation never reaches GitHub.
12. **Gating:** must the first release with in-launcher updates wait for the Windows lab
    acceptance (firewall, ACL, data byte-identical) to pass (proposed)?
13. **Cleanup:** after a successful update, remove the build workspace and keep only
    `MAG-build/logs` and `MAG-build/previous` (proposed)?
