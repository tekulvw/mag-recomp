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
- and, recorded when the entry was made, the content of every other file generation read (game
  files, for example), the absence of files it looked for, and the names in every other folder it
  listed. An entry is reused only when all of those are unchanged.

What generation reads is observed, not assumed: each generation process runs with a small Python
audit hook (written into the build folder; ``PYTHONPATH``) that records the files it opens, the
folders it lists and the programs it starts. If generation read a file outside the cases above,
or started a program other than Python or git (which only reads the pinned checkouts), its
results are not stored, and the next build generates again.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import time

from .build_cache import BuildCache, digest, duration_text, file_hash
from .model import InstallerError

SCHEMA = 1
# Hashed whole: the recipe's tools and the reviewed profiles (with the ps3recomp pins).
STATIC = ('tools', 'profiles')
TRACE_VARIABLE = 'MAG_GENERATION_TRACE'
# Programs generation may start besides Python: git, which only reads the pinned checkouts.
ALLOWED_PROGRAMS = ('git', 'git.exe')

TRACER = r'''"""MAG installer: record what this Python process opens, lists and starts (build cache keys)."""
import os, sys, threading
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
                    argv = args[1]
                    argv = [argv] if isinstance(argv, (str, bytes)) else list(argv or [])
                    line = 'X\t' + '\x1f'.join(os.fsdecode(a) for a in [args[0] or (argv[0] if argv else '')] + argv[1:])
                else:
                    line = 'X\t' + os.fsdecode(str(args[0]))
                if line in _state['seen']:
                    return
                _state['seen'].add(line)
                _state['busy'] = False
                _write(line)
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


def key_material(config, workspace, stages, token):
    from .native_recipe import config_host
    return {'kind': 'generation', 'schema': SCHEMA, 'host': config_host(config),
            'build_folder': str(config['build_cache']['base_dir']), 'workspace': str(workspace),
            'stages': [{'id': stage['id'], 'argv': [str(a) for a in stage['argv']]} for stage in stages],
            'python': _python_identity(config),
            'inputs': {key: file_hash(config[key], token) for key in ('eboot_elf', 'engine_elf', 'firmware')},
            'static': _static_digest(workspace, token)}


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
            elif kind == 'X': programs.append(value.split('\x1f'))
            elif kind == 'S': prefixes.update(value.split('\x1f'))
    return reads, writes, lists, programs, prefixes


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
    for argv in programs:
        program = Path(argv[0]).name.lower() if argv else ''
        if not argv:
            return None, None, 'generation started a program it did not name'
        if _norm(Path(argv[0]).resolve()) == python or _norm(argv[0]) == _norm(config['python']):
            if any(flag in argv[1:2] for flag in ('-I', '-E', '-S')):
                return None, None, f'generation started Python without its trace: {argv[:2]}'
            continue
        if program in ALLOWED_PROGRAMS or _norm(argv[0]) == _norm(config['git']):
            continue
        return None, None, f'generation started {argv[0]}'
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
    material = key_material(config, workspace, stages, token)
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
