# Installer design

Players select their own game directory and a new output directory. The installer verifies an approved version identity, obtains pinned build tools, generates locally, builds locally, and packages a local application. Distribution contains tooling, profiles, and permissible recipes only. Game files, decrypted ELF files, generated game code, built game applications, captures, and account data are excluded.

This foundation uses Python 3.11+ standard library modules. This is an implementer choice to keep acquisition and validation auditable, not a requirement that players install Python or compilers: eventual releases must bundle a Python runtime/frozen launcher and pinned tools. Alternatives considered were three native installers (too much duplicated security logic) and a large web UI framework (unnecessary dependency surface before integration facts exist).

A versioned JSON profile identifies inputs by relative path, exact byte size, SHA-256, and public/provenance metadata. All production platform entries initially report blocked. No private game identity is populated. Windows x86_64/arm64, Linux x86_64/arm64, and macOS x86_64/arm64 adapters select exact host matches; adapters do not imply successful native builds. Generation/build/package commands are enabled only for a verified profile whose precise recipe and pinned tools have been reviewed. User input never supplies executable commands.

The shared core treats input as read-only; rejects symlinks, traversal, drive paths, unsupported profiles, foreign host recipes, and existing destinations; downloads only pinned HTTPS archives; verifies size and SHA-256 before atomic cache promotion; extracts only regular files/directories with entry and expanded-size limits; executes argv arrays from verified tool roots with a controlled environment and working directory; and emits local JSON events. Cancellation kills the process group on POSIX and a process tree on Windows. ZIP/TAR links, devices, duplicate/case-colliding names, and executables outside pinned tools are rejected. No telemetry or upload exists.

Output uses a sibling staging directory and exclusive lock. POSIX locking keeps a stable lock inode and descriptor with nonblocking kernel `flock` ownership throughout the transaction; it never unlinks/recreates a lock based on PID metadata. Windows uses a native OS-held byte-range lock on a stable file; legacy and unknown formats fail closed. Native Windows execution remains unverified. Interrupted downloads restart from zero without trusting partial content. Interrupted pipelines retain a state journal and logs; explicit resume checks profile digest, input identity, host, destination, and lock ownership. Recovery revalidates inputs and reruns stages into a fresh work directory, so completed stages are not trusted blindly. Existing final output is never overwritten. Atomic directory rename publishes only a completed local result. Lock release, close and cleanup-notice failures cannot turn a committed result into failure, or replace an existing precommit exception; reporting is secondary to the transaction outcome. Installer distribution packaging uses an explicit allowlist independent of build outputs and input paths.

### Player build lock

Building MAG, a Retry build and the move from the earlier layout hold a lock on `.MAG.player-lock`
beside the MAG folder (`.<name>.player-lock` for another folder name), so that only one installer
works on an installation location at a time. The specification:

- Ownership is the operating system's lock on that open file (`flock` on macOS and Linux, a
  byte-range lock on its first byte on Windows), taken without waiting and held for the whole build.
  The operating system drops it when its owner closes the file or exits, however the process ends:
  finished, failed, cancelled, crashed or killed.
- A second installer that cannot take the lock stops before changing anything, with "Another
  installer holds this output". This is the protection against two builds at once.
- A lock file whose lock can be taken belongs to an installer that has exited. Build MAG and Retry
  build take it over without asking for a resume, so a lock file never has to be deleted by hand.
  An empty file is taken over too (its creator stopped between creating and recording it). Content
  this installer does not recognise stays fail-closed for inspection, as before.
- The file records the owner's process id and the time it took the lock (UTC), for people
  reading it. They are never used to decide whether the owner is alive: a process id can be reused
  by an unrelated process, and comparing process start times needs per-platform process APIs and
  clock agreement. The operating system's lock has neither weakness.
- When a build ends, successfully or not, the installer releases the lock and removes the file. On
  macOS and Linux it removes the file while it still holds the lock; an installer that opened the
  old file meanwhile checks, once it gets the lock, that the path still names the file it locked,
  and otherwise starts over (a few attempts). Windows cannot delete a file the process has open, so
  the installer closes it first and then removes it; if another installer opened it in between,
  Windows refuses the deletion and that installer owns the file. Failing to remove the file is never
  a build failure: the next installer takes it over.

The transaction pipeline below (`.<output>.mag-lock`) and the native build lock keep their stable
lock files and explicit resume.

### Updating a MAG folder in place

`player_build.update` (the wizard's **Update MAG**, `update`, and `build` on an existing MAG folder)
rebuilds an existing MAG folder without moving it. Its specification is
[the launcher updates design](../../docs/superpowers/specs/2026-11-28-launcher-updates-design.md),
"In-place update in the installer"; this phase has no network, signing or launcher check.

- Under the player build lock, a swap an earlier run left open is finished or undone first. Then
  the MAG folder must be complete (its app, `game/`, and only `home/`, `logs/` and
  `launch-error.log` besides), MAG must not run from it (`home/.session-lock` held, or a process
  whose executable is inside the app folder), and the drive must have room.
- The inputs resolve in order: the player's choice; the recorded paths in `installs.json`; the MAG
  folder's own `game/` (the rule that the source is separate from the output is relaxed for exactly
  `<output>/game`, which the update only reads); the private firmware copy; RPCS3 detection. Every
  candidate passes the wizard's checks before it is used, and the build checks the kit's identities.
- The build folder is reused when it holds this update's own failed attempt with the same binding
  (the retry rules), else its journal and logs are archived under `logs/attempts/<n>` and its
  workspace is removed. Unknown entries, a journal for another folder or an unknown state stop it.
- The build and packaging steps are those of a first build. Packaging writes `version` and
  `build_options` (voice, game controls, x86-64 level) into the app's `runtime.json`.
- The swap (update.py) is journaled in `build.json` as `swap.phase`: `staging` (the new app to
  `<MAG>/.MAG-new`; if the staged game files differ from `game/`, they are swapped as well, through
  `.game-previous`), `swapping` (the app folder to `.MAG-previous`, `.MAG-new` to the app folder's
  name), `verifying` (the installer's self-check of the receipts, the launcher and the game
  executables' identities, then the previous app folder's permissions: its mode on macOS and
  Linux, its explicit DACL entries and protection on Windows, verified entry by entry with the
  player's full access on every file), `keeping` (the previous app to `MAG-build/previous` with
  `previous.json`) and `cleaning` (the workspace removed, logs kept). Renames never replace
  anything. A failure up to `verifying` reverses the renames that happened; a later run that finds
  one of these phases open reverses them and runs the swap again from the staged app. From
  `keeping` on, the new app is in place and the next run only finishes keeping and cleaning.
- `--restore-previous` runs the same swap with `MAG-build/previous` as the staged app.

The synthetic demonstration (`install --synthetic`) exercises this transaction core and creates a clearly labelled text artifact; it is not a MAG build. Building MAG is the separate player route (`player_build.py`), which runs the BUILDING.md recipe from the embedded source kit with pinned managed tools.

A lightweight Tkinter wizard now consumes the shared pipeline through a background worker and main-thread event queue. No arguments opens the wizard; explicit CLI commands stay accessible without Tk. The wizard selects source and an existing parent, shows blocked-host and exact-input information, reports known storage lower bounds with unknown build scratch clearly identified, and offers safe cancellation/retry and output/log access. Existing output is protected and never treated as verified merely because it exists. Synthetic mode remains explicitly labelled.

CLI output defaults to readable text on a terminal and JSON when redirected, with explicit overrides. The pure ux module supplies presentation/default paths without importing Tk. Preflight is read-only and groups known storage by filesystem; it cannot promise sufficient space for unspecified future build recipes. Successful transactions remove recognized owned work unless keep_work is requested and retain logs/journal; cleanup failures cannot relabel committed success. Periodic stage activity indicates elapsed work without claiming measured progress.

Later builds on the same computer reuse earlier work through a per-user build cache in the installer's cache folder (`build_cache.py`): managed dependency prefixes, generated sources and compiled objects (a pinned, hash-verified ccache). Every entry is keyed by everything that determines its result (recipe and commands, pinned source, tool and SDK identities, compiler identity and target, and the content of what generation read, which is observed through a Python audit hook while it runs), files are content-addressed and verified on restore, and the cache is bounded with least-recently-used eviction. It holds content derived from the player's game files, so it never leaves the player's machine: it is not part of a MAG folder, a release, an installer or the source bundle, and nothing uploads it. The wizard and CLI can clear it, and `MAG_BUILD_CACHE=0` builds without it.

### What generation may run (generated-sources cache)

Generation (the prepare, inventory, lift, splice, SPU, firmware and program steps) is kept for a
later build only when everything it ran is accounted for (`lift_cache.py`):

- **Python:** the build's own interpreter, started with the audit hook in place (never with `-I`,
  `-E` or `-S`, which would drop it). Its reads, listings and program starts are recorded too.
- **git, read-only questions about a pinned ps3recomp checkout** (`git -C <checkout>`, or a folder
  in it): `rev-parse` of `HEAD`, of a commit pinned for that checkout, or of a tag pinned for it
  (`refs/tags/<tag>`), optionally peeled with `^{commit}` or `^{tree}` and with `--verify` or
  `--quiet`; `status --porcelain --untracked-files=no`; and `show <pinned commit>:<path>`. A
  checkout's pinned commits and tags are its own and its fixed lifters' (`splice_after`) in
  `profiles/framework.json`.
- **git, the output-location check** (`git -C <folder in the workspace>`): `rev-parse
  --show-toplevel` and `rev-parse --is-inside-git-dir`, which ask whether the workspace lies in a
  Git repository.

Anything else (another program, a shell command, git in another folder, a git command that writes
or asks about other revisions, or an event the hook could not record) keeps nothing, and the next
build generates again. The answers to the allowed questions are part of the key: the pins
themselves (`profiles/` is hashed whole), and what the installer gets when it asks git, before
generation runs, what each checkout's `HEAD`, pinned commits and pinned tags resolve to and where
the workspace lies. A pinned commit names fixed content, so `show` and `rev-parse` of it follow
from the pin; a moved pin, a missing or moved tag, or another `HEAD` gives another key. The
checkouts are fresh clones the installer checks out at their pins in the same build, so `status`
reports them clean.

Python on Windows reports a started program to audit hooks as the single command line it will run,
not as the argument list (POSIX reports the list). The recorded command line is split back into
arguments with the rules Windows programs use to read theirs (the reverse of
`subprocess.list2cmdline`), so the same checks apply on every platform.

### Finding RPCS3's files and the preparation guide

The player prepares every input with their own RPCS3. The installer never downloads RPCS3 or
firmware, never extracts or decrypts anything and never runs RPCS3: it links to rpcs3.net and to
Sony's system software page and prints the commands for the player to run. `rpcs3_detect.py` looks
for what RPCS3 already has; `input_guide.py` turns that and the chosen inputs into the checklist the
window and the `guide` command show. The specification:

- The search is read-only: it lists folders and reads small text files (`vfs.yml`, `games.yml`,
  `config.yml`, `GuiConfigs/CurrentSettings.ini`, `dev_flash/vsh/etc/version.txt`) and `PARAM.SFO`
  headers. It never writes, copies or moves a file, and runs in the background.
- It is bounded: fixed starting places per host (installer/README.md, "Getting your files ready"),
  at most two folder levels below each on Windows and none elsewhere, at most 20,000 folder entries
  and 3 seconds. A search stuck in one slow file system call is abandoned shortly after the time
  limit with what it found so far. Links and junctions are not followed; on Windows only fixed
  drives are searched, and `AppData`, `Windows` and other system folders are never entered. On macOS
  the privacy-protected Downloads, Desktop and Documents folders are not read.
- A MAG game folder is recognised by the `TITLE_ID` (BCUS98110), `CATEGORY` (DG, a disc) and
  `APP_VER` (01.00) of its `PARAM.SFO`. Other MAG folders RPCS3 has, such as an installed update,
  are named with the reason they cannot be used.
- Found paths only pre-fill empty fields. Each chosen input is checked at once against the reviewed
  SHA-256 identities: the installer manifest carries a copy of its kit's (`reviewed_inputs`, written
  by `scripts/prepare_player_kit.py`), and a source checkout reads the repository's. Build MAG
  checks the kit's own identities again before anything is downloaded, so the immediate check is
  advice, never a gate.
