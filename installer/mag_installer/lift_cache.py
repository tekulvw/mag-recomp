"""Reuse of the generation steps (prepare, inventory, lift, splices, SPU, firmware and program lifts).

Generation is deterministic: the same commands, run by the same Python on the same inputs, write
the same files. An entry in the build cache records what generation wrote, keyed by:

- the build folder and the generation commands. Generated records name the build folder and some
  record the SHA-256 of such records (a splice records its base's provenance), so results are
  reused only in the same build folder, where they are byte-for-byte what generation would write.
  Building again in the same installation location reuses them; elsewhere generation runs again,
  and its sources, which name no folder, still meet their compiled objects in the compile cache;
- the Python that runs them (the managed interpreter's verified inventory, wheels included);
- the identities of the decrypted executables and firmware module the build uses;
- the content of every file below ``tools/`` and ``profiles/`` (the recipe, the reviewed profiles
  and the pinned ps3recomp revisions; the SDK checkouts are clean clones of those revisions);
- what each pinned checkout's HEAD, pinned commits and pinned tags resolve to, and whether the
  workspace lies in a Git repository, asked of git by the installer before generation runs;
- and, recorded when the entry was made, the content of every other file generation read (game
  files, for example), the absence of files it looked for, and the names in every other folder it
  listed. An entry is reused only when all of those are unchanged.

What generation reads is observed, not assumed: each generation process runs with a small Python
audit hook (written into the build folder; ``PYTHONPATH``) that records the files it opens, the
folders it lists and the programs it starts. If generation read a file outside the cases above,
or started any program other than the traced Python and these read-only git questions, its
results are not stored, and the next build generates again:

- about a pinned checkout (``git -C <checkout or a folder in it>``): ``rev-parse`` of HEAD, of one
  of that checkout's pinned commits or of one of its pinned tags (``refs/tags/<tag>``), optionally
  peeled with ``^{commit}`` or ``^{tree}`` and with ``--verify``/``--quiet``; ``status --porcelain
  --untracked-files=no``; and ``show <pinned commit>:<path>``. Their answers follow from the
  pinned commits and the resolutions in the key (a pinned commit names fixed content);
- about the output location (``git -C <folder in the workspace>``): ``rev-parse --show-toplevel``
  and ``rev-parse --is-inside-git-dir``, which the key answers for the workspace.

Python on Windows reports a started program to audit hooks as one command line rather than a list
of arguments; it is split back with the rules Windows programs use to read their command line.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time

from .build_cache import BuildCache, digest, duration_text, file_hash
from .model import InstallerError

SCHEMA = 2
# Hashed whole: the recipe's tools and the reviewed profiles (with the ps3recomp pins).
STATIC = ('tools', 'profiles')
TRACE_VARIABLE = 'MAG_GENERATION_TRACE'
# Programs generation may start besides Python: git, for the read-only questions above.
ALLOWED_PROGRAMS = ('git', 'git.exe')
# Options of `git rev-parse <revision>` that change only how an answer is reported.
REV_PARSE_OPTIONS = ('--verify', '--quiet', '-q')
# What the output-location checks ask git about a folder in the workspace (the key holds the answers).
LOCATION_QUESTIONS = ('--show-toplevel', '--is-inside-git-dir')

TRACER = r'''"""MAG installer: record what this Python process opens, lists and starts (build cache keys)."""
import json, os, sys, threading
_folder = os.environ.get('MAG_GENERATION_TRACE')
if _folder:
    _lock = threading.RLock()
    _state = {'pid': None, 'fd': None, 'busy': False, 'seen': set()}
    def _write(line):
        if _state['pid'] != os.getpid():
            _state['busy'] = True
            try:
                _state['pid'] = os.getpid(); _state['seen'] = set()
                _state['fd'] = os.open(os.path.join(_folder, '%d-%d.trace' % (os.getpid(), threading.get_ident())),
                                       os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
            finally:
                _state['busy'] = False
        os.write(_state['fd'], (line + '\n').encode('utf-8', 'surrogateescape'))
    def _hook(event, args):
        if _state['busy'] or event not in ('open', 'os.listdir', 'os.scandir', 'subprocess.Popen',
                                           'os.exec', 'os.posix_spawn', 'os.spawn', 'os.system'):
            return
        with _lock:
            if _state['busy']:
                return
            _state['busy'] = True
            try:
                if event == 'open':
                    path, mode, flags = args[0], args[1], args[2]
                    if isinstance(path, int) or path is None:
                        return
                    if mode is None and isinstance(flags, int) and flags & getattr(os, 'O_DIRECTORY', 0):
                        return   # a folder handle (shutil.rmtree, dir_fd paths), not a file's content
                    writing = (mode is not None and any(c in str(mode) for c in 'wax+')) or \
                              (mode is None and isinstance(flags, int) and flags & 3)
                    line = ('W\t' if writing else 'R\t') + os.path.abspath(os.fsdecode(path))
                elif event in ('os.listdir', 'os.scandir'):
                    path = args[0]
                    if isinstance(path, int):
                        return
                    line = 'L\t' + os.path.abspath(os.fsdecode('.' if path is None else path))
                elif event == 'subprocess.Popen':
                    executable, argv, cwd = args[0], args[1], args[2]
                    # POSIX reports the argument list; Windows the command line it will start.
                    start = ({'command_line': os.fsdecode(argv)} if isinstance(argv, (str, bytes, os.PathLike))
                             else {'argv': [os.fsdecode(a) for a in argv or []]})
                    start['executable'] = None if executable is None else os.fsdecode(executable)
                    start['cwd'] = os.getcwd() if cwd is None else os.path.abspath(os.fsdecode(cwd))
                    line = 'X\t' + json.dumps(start, sort_keys=True)
                elif event == 'os.system':
                    line = 'X\t' + json.dumps({'shell': os.fsdecode(args[0])})
                else:   # os.exec(path, args, env), os.posix_spawn(path, argv, env), os.spawn(mode, path, args, env)
                    path, argv = (args[1], args[2]) if event == 'os.spawn' else (args[0], args[1])
                    line = 'X\t' + json.dumps({'executable': os.fsdecode(path), 'argv': [os.fsdecode(a) for a in argv],
                                               'cwd': os.getcwd()}, sort_keys=True)
                if line in _state['seen']:
                    return
                _state['seen'].add(line)
                _state['busy'] = False
                _write(line)
            except Exception:
                # Unrecorded: the trace says so, and the results are not kept.
                try:
                    _state['busy'] = False
                    _write('E\t' + event)
                except Exception:
                    pass
            finally:
                _state['busy'] = False
    _write('P\t' + '\x1f'.join([sys.executable] + list(sys.argv)))
    _write('S\t' + '\x1f'.join(sorted({sys.prefix, sys.exec_prefix, sys.base_prefix, sys.base_exec_prefix})))
    sys.addaudithook(_hook)
'''


def _norm(path):
    return os.path.normcase(os.path.normpath(str(path)))


def _inside(path, folder):
    path, folder = _norm(path), _norm(folder)
    return path == folder or path.startswith(folder.rstrip(os.sep) + os.sep)


def split_command_line(line):
    """The arguments of a Windows command line, read as Windows programs read theirs: whitespace
    separates arguments outside double quotes; backslashes are literal except before a double
    quote, where each pair is one backslash and an odd one makes the quote literal. This reverses
    subprocess.list2cmdline, which is how Python on Windows joins an argument list."""
    arguments, index, size = [], 0, len(line)
    while True:
        while index < size and line[index] in ' \t':
            index += 1
        if index >= size:
            return arguments
        argument, quoted = [], False
        while index < size and (quoted or line[index] not in ' \t'):
            if line[index] == '\\':
                end = index
                while end < size and line[end] == '\\':
                    end += 1
                if end < size and line[end] == '"':
                    argument.append('\\' * ((end - index) // 2))
                    if (end - index) % 2:
                        argument.append('"'); end += 1
                else:
                    argument.append('\\' * (end - index))
                index = end
            elif line[index] == '"':
                quoted = not quoted; index += 1
            else:
                argument.append(line[index]); index += 1
        arguments.append(''.join(argument))


def _pinned(pin):
    """A checkout's pinned commits and tags: its own and its fixed lifters' (splice_after)."""
    from .native_recipe import splice_pins
    extras = splice_pins(pin)
    return [pin['commit'], *(e['commit'] for e in extras)], [pin['tag'], *(e['tag'] for e in extras)]


def _git(config, env, folder, *args, stdin=None):
    """git's answer in a folder, or None when it gives none."""
    try:
        result = subprocess.run([config.get('git') or 'git', '-C', str(folder), *args], input=stdin,
                                capture_output=True, text=True, env=env, timeout=300)
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def _resolved_pins(config, workspace, pins, env):
    """What HEAD, the pinned commits and the pinned tags name in each pinned checkout (None: nothing).
    Generation may ask git about exactly these, so its answers are part of the key."""
    resolved = {}
    for role, pin in sorted(pins.items()):
        commits, tags = _pinned(pin)
        questions = ['HEAD', *commits, *('refs/tags/' + tag for tag in tags)]
        answers = dict.fromkeys(questions)
        checkout = workspace / pin['path']
        if (checkout / '.git').exists():   # never a repository around it
            found = _git(config, env, checkout, 'cat-file', '--batch-check=%(objectname)',
                         stdin=''.join(q + '^{commit}\n' for q in questions))
            lines = found.splitlines() if found is not None else []
            if len(lines) == len(questions):
                answers = {q: line if re.fullmatch('[0-9a-f]{40}|[0-9a-f]{64}', line) else None
                           for q, line in zip(questions, lines)}
        resolved[role] = answers
    return resolved


def _location(config, workspace, env):
    """What git says about the workspace's place: the repository around it, if any."""
    return {question: _git(config, env, workspace, 'rev-parse', question) for question in LOCATION_QUESTIONS}


def _snapshot(root):
    """{relative posix path: (size, mtime_ns, inode)} of the files below root; folders separately."""
    files, folders = {}, set()
    root = Path(root)
    for base, directories, names in os.walk(root, followlinks=False):
        relative = Path(base).relative_to(root).as_posix()
        if relative != '.':
            folders.add(relative)
        for name in names:
            path = Path(base) / name
            info = path.lstat()
            key = (Path(relative) / name).as_posix() if relative != '.' else name
            files[key] = (info.st_size, info.st_mtime_ns, info.st_ino, stat.S_ISLNK(info.st_mode))
    return files, folders


def _python_root(config):
    """The managed Python's root: the folder whose verified inventory covers it and its wheels."""
    python = Path(config['python']).resolve()
    for parent in list(python.parents)[:4]:
        if (parent / 'MANAGED-INVENTORY.json').is_file():
            return parent
    return None


def _python_identity(config):
    root = _python_root(config)
    if root is not None:
        return {'inventory': file_hash(root / 'MANAGED-INVENTORY.json')}
    return {'executable': file_hash(Path(config['python']).resolve()), 'path': str(config['python'])}


def _static_digest(workspace, token):
    hasher = hashlib.sha256()
    for name in STATIC:
        root = workspace / name
        paths = sorted(p for p in root.rglob('*') if p.is_file() and '__pycache__' not in p.parts) if root.is_dir() else []
        for path in paths:
            token.check()
            hasher.update(path.relative_to(workspace).as_posix().encode() + b'\0' + file_hash(path).encode() + b'\n')
    return hasher.hexdigest()


def key_material(config, workspace, stages, token, env=None):
    from .native_recipe import config_host, framework_pins
    return {'kind': 'generation', 'schema': SCHEMA, 'host': config_host(config),
            'build_folder': str(config['build_cache']['base_dir']), 'workspace': str(workspace),
            'stages': [{'id': stage['id'], 'argv': [str(a) for a in stage['argv']]} for stage in stages],
            'python': _python_identity(config),
            'inputs': {key: file_hash(config[key], token) for key in ('eboot_elf', 'engine_elf', 'firmware')},
            'static': _static_digest(workspace, token),
            'checkouts': _resolved_pins(config, workspace, framework_pins(workspace), env),
            'location': _location(config, workspace, env)}


def _names_digest(names):
    return hashlib.sha256('\n'.join(sorted(names)).encode('utf-8', 'surrogateescape')).hexdigest()


def _listing(path):
    try:
        return _names_digest(os.listdir(path))
    except OSError:
        return None


def _children(relative, files, folders):
    """The names directly inside a folder of a snapshot ('.' is the root)."""
    prefix = '' if relative == '.' else relative + '/'
    return {name[len(prefix):].split('/', 1)[0] for name in (*files, *folders)
            if name.startswith(prefix) and name != relative}


def _still_valid(record, workspace, token):
    """Every file and folder listing generation read beyond the key is unchanged."""
    for name, expected in record.get('reads', {}).items():
        token.check()
        path = workspace / name
        if expected is None:
            if path.exists() or path.is_symlink():
                return False
        elif not path.is_file() or path.is_symlink() or file_hash(path, token) != expected:
            return False
    for name, expected in record.get('lists', {}).items():
        if _listing(workspace / name if name != '.' else workspace) != expected:
            return False
    return True


def _read_traces(folder):
    reads, writes, lists, programs, prefixes = set(), set(), set(), [], set()
    for path in sorted(Path(folder).glob('*.trace')):
        for line in path.read_text(encoding='utf-8', errors='surrogateescape').splitlines():
            kind, _, value = line.partition('\t')
            if kind == 'R': reads.add(value)
            elif kind == 'W': writes.add(value)
            elif kind == 'L': lists.add(value)
            elif kind == 'X': programs.append(json.loads(value))
            elif kind == 'E': programs.append({'unrecorded': value})
            elif kind == 'S': prefixes.update(value.split('\x1f'))
    return reads, writes, lists, programs, prefixes


def _git_problem(arguments, cwd, workspace, pins):
    """Why a git command is not one of the read-only questions of the module docstring, or None."""
    if len(arguments) < 3 or arguments[0] != '-C':
        return 'outside a pinned checkout'
    # Real paths: the tools find the checkouts from their own resolved location.
    folder = os.path.realpath(os.path.join(cwd or str(workspace), arguments[1]))
    command, rest = arguments[2], arguments[3:]
    for pin in pins.values():
        if _inside(folder, os.path.realpath(workspace / pin['path'])):
            commits, tags = _pinned(pin)
            if command == 'rev-parse':
                revisions = [a for a in rest if a not in REV_PARSE_OPTIONS]
                base = re.sub(r'\^\{(commit|tree)\}$', '', revisions[0]) if len(revisions) == 1 else None
                if base == 'HEAD' or base in commits or base in {'refs/tags/' + tag for tag in tags}:
                    return None
            elif command == 'status' and sorted(rest) == ['--porcelain', '--untracked-files=no']:
                return None
            elif command == 'show' and len(rest) == 1:
                commit, _, path = rest[0].partition(':')
                if commit in commits and path:
                    return None
            return 'with a question about its checkout that is not a pinned revision check'
    if _inside(folder, os.path.realpath(workspace)) and command == 'rev-parse' and len(rest) == 1 and rest[0] in LOCATION_QUESTIONS:
        return None
    return 'outside a pinned checkout'


def _allowed_outside(path, config, prefixes=()):
    """Reads that are not inputs of generation: the Python that runs it (part of the key: the managed
    Python's inventory, its wheels included) and the operating system."""
    normalized = _norm(path)
    root = _python_root(config)
    for folder in ([root] if root is not None else []) + [Path(p) for p in prefixes if p]:
        # The folder itself too: Windows Python lists its prefix, which is on sys.path.
        if normalized == _norm(folder) or normalized.startswith(_norm(folder) + os.sep):
            return True
    if os.name == 'nt':
        windows = os.environ.get('SystemRoot', 'C:\\Windows')
        return normalized == _norm(windows) or normalized.startswith(_norm(windows) + os.sep) or Path(path).name.lower() == 'nul' or path.startswith('\\\\.\\')
    system = ('/dev/', '/etc/', '/usr/lib/', '/usr/share/', '/proc/', '/sys/')
    if sys.platform == 'darwin':
        system += ('/System/', '/private/etc/')
    return normalized.startswith(tuple(_norm(prefix) for prefix in system))


def _classify(config, workspace, output, before, after_files, traces, pins):
    """The reads beyond the key ({relative: sha256}), listings, and why the result cannot be stored."""
    reads, writes, lists, programs, prefixes = traces
    files_before, folders_before = before
    python = _norm(Path(config['python']).resolve())
    for start in programs:
        if 'unrecorded' in start:
            return None, None, f'generation did something its trace could not record ({start["unrecorded"]})'
        if 'shell' in start:
            return None, None, f'generation ran a shell command: {start["shell"]}'
        argv = split_command_line(start['command_line']) if 'command_line' in start else list(start.get('argv') or [])
        program = start.get('executable') or (argv[0] if argv else '')
        if not program or not argv:
            return None, None, 'generation started a program it did not name'
        if _norm(Path(program).resolve()) == python or _norm(program) == _norm(config['python']):
            if any(flag in argv[1:2] for flag in ('-I', '-E', '-S')):
                return None, None, f'generation started Python without its trace: {argv[:2]}'
            continue
        if Path(program).name.lower() in ALLOWED_PROGRAMS or _norm(program) == _norm(config.get('git') or 'git'):
            problem = _git_problem(argv[1:], start.get('cwd'), workspace, pins)
            if problem is None:
                continue
            return None, None, f'generation started git {problem}: {" ".join(argv[1:])}'
        return None, None, f'generation started {program}'
    workspace_prefix = _norm(workspace) + os.sep
    output_prefix = _norm(output) + os.sep
    checkouts = [_norm(workspace / pin['path']) + os.sep for pin in pins.values()]
    recorded, listed = {}, {}
    for path in sorted(reads):
        normalized = _norm(path)
        if normalized.startswith(workspace_prefix):
            relative = Path(path).relative_to(workspace).as_posix() if Path(path).is_relative_to(workspace) else None
            if relative is None:
                return None, None, f'generation read {path}'
            if relative.split('/', 1)[0] in STATIC or any(normalized.startswith(c) for c in checkouts):
                continue
            if relative not in files_before:
                parent = relative.rsplit('/', 1)[0] if '/' in relative else '.'
                if relative not in after_files and not (workspace / relative).exists() \
                        and (parent == '.' or parent in folders_before):
                    recorded[relative] = None   # looked for and absent: its absence is an input
                continue   # written by generation itself (in folders it made, or left behind)
            if relative in after_files and after_files[relative] != files_before[relative]:
                return None, None, f'generation changed {relative}, which existed before it ran'
            recorded[relative] = file_hash(workspace / relative)
        elif normalized.startswith(output_prefix):
            continue   # the build's own logs, home and bytecode, written during this build
        elif not _allowed_outside(path, config, prefixes):
            return None, None, f'generation read {path}'
    for path in sorted(lists):
        normalized = _norm(path)
        if normalized.startswith(workspace_prefix) or normalized == _norm(workspace):
            if not Path(path).is_relative_to(workspace):
                return None, None, f'generation listed {path}'
            relative = Path(path).relative_to(workspace).as_posix()
            if relative.split('/', 1)[0] in STATIC or any(normalized.startswith(c) for c in checkouts) \
                    or (relative != '.' and relative not in folders_before):
                continue   # pinned, or made by generation itself
            # The folder as it was before generation added anything to it.
            listed[relative] = _names_digest(_children(relative, files_before, folders_before))
        elif normalized.startswith(output_prefix) or _allowed_outside(path, config, prefixes):
            continue
        else:
            return None, None, f'generation listed {path}'
    return recorded, listed, None


def run_generation(config, workspace, stages, run_stage, env, token, events):
    """Run the generation stages, or restore what an identical earlier run wrote."""
    from .native_recipe import framework_pins
    workspace = Path(workspace)
    output = workspace.parent
    cache = BuildCache(config['build_cache']['root'], token, events)
    material = key_material(config, workspace, stages, token, env)
    key = digest(material)
    record = cache.lookup('lift', key)
    if record is not None and _still_valid(record, workspace, token):
        restored = cache.restore('lift', key, workspace, check=lambda found: _still_valid(found, workspace, token))
        if restored is not None:
            for folder in restored.get('folders', []):
                (workspace / folder).mkdir(parents=True, exist_ok=True)
            saved = restored.get('seconds', 0)
            events({'event': 'build-cache-hit', 'kind': 'lift', 'saved_seconds': saved, 'stages': [s['id'] for s in stages],
                    'message': f'Build cache: reused the generated sources of an earlier build in this location '
                               f'({len(restored["files"])} files; about {duration_text(saved)} saved).'})
            return restored
    events({'event': 'build-cache-miss', 'kind': 'lift',
            'message': 'Build cache: no earlier build in this location generated these sources; generating them now.'})
    before = _snapshot(workspace)
    traces = output / 'home' / 'generation-trace'
    tracer = output / 'home' / 'generation-tracer'
    for folder in (traces, tracer):
        if folder.exists():
            import shutil
            shutil.rmtree(folder)
        folder.mkdir(parents=True)
    (tracer / 'sitecustomize.py').write_text(TRACER, encoding='utf-8')
    traced = {**env, 'PYTHONPATH': str(tracer), TRACE_VARIABLE: str(traces)}
    started = time.monotonic()
    for stage in stages:
        run_stage(stage, traced)
    seconds = time.monotonic() - started
    after_files, after_folders = _snapshot(workspace)
    recorded, listed, reason = _classify(config, workspace, output, before, after_files,
                                         _read_traces(traces), framework_pins(workspace))
    if reason is not None:
        events({'event': 'build-cache-skipped', 'kind': 'lift', 'reason': reason,
                'message': f'Build cache: the generated sources were not kept ({reason}); later builds generate them again.'})
        return None
    files_before, folders_before = before
    outputs = sorted(name for name, info in after_files.items() if files_before.get(name) != info)
    deleted = sorted(set(files_before) - set(after_files))
    folders = sorted(after_folders - folders_before)
    cache.store('lift', key, workspace, outputs, material, seconds, deleted=deleted,
                extra={'reads': recorded, 'lists': listed, 'folders': folders})
    events({'event': 'build-cache-stored', 'kind': 'lift', 'files': len(outputs),
            'message': f'Build cache: kept the {len(outputs)} generated files for later builds on this computer.'})
    return None
