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
