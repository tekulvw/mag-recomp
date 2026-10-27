"""Transactional local pipeline. Production recipes are maintainer-owned only."""
from __future__ import annotations

from contextlib import contextmanager
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import time

from .model import InstallerError, host_key
from .capacity import estimate
from .platforms import publish_directory, stop_process_tree
from .safety import Cancellation, contained, file_digest, validate_inputs
from .tools import acquire, extract_archive


ACTIVITY_INTERVAL = 2.0


def run_command(argv, cwd, log, token, env, events=None, stage=None):
    """Run explicit argv without a shell, with bounded cancellation polling."""
    token.check()
    if not argv or not Path(argv[0]).is_absolute():
        raise InstallerError('Stage executable must be an absolute pinned tool path')
    if os.name == 'nt' and Path(argv[0]).suffix.lower() in ('.bat', '.cmd'):
        raise InstallerError('Windows batch files are unsupported; pin a native executable')
    started = last_activity = time.monotonic()
    with Path(log).open('ab') as output:
        try:
            options = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' else {'start_new_session': True}
            process = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                       stdout=output, stderr=subprocess.STDOUT, shell=False, **options)
            try:
                while process.poll() is None:
                    token.check()
                    now = time.monotonic()
                    if events is not None and now - last_activity >= ACTIVITY_INTERVAL:
                        events({'event': 'stage-activity', 'stage': stage, 'elapsed': now - started, 'log': str(log)})
                        last_activity = now
                    try: process.wait(timeout=0.1)
                    except subprocess.TimeoutExpired: pass
                token.check()
            except BaseException:
                stop_process_tree(process, output)
                raise
            if process.returncode:
                raise InstallerError(f'Local stage exited with code {process.returncode}; inspect {log}')
        except OSError as exc:
            raise InstallerError(f'Cannot start pinned tool ({type(exc).__name__}); inspect {log}') from exc


def _write_json(path, data):
    temporary = path.with_suffix('.pending')
    if temporary.is_symlink() or path.is_symlink():
        raise InstallerError('Journal path must not be a symlink')
    with temporary.open('w', encoding='utf-8') as output:
        json.dump(data, output, ensure_ascii=False, indent=2)
        output.write('\n')
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(path)


def _release_lock(handle, backend):
    if backend == 'posix-flock':
        import fcntl
        fcntl.flock(handle, fcntl.LOCK_UN)
    else:
        import msvcrt
        os.lseek(handle, 0, os.SEEK_SET)
        msvcrt.locking(handle, msvcrt.LK_UNLCK, 1)


def _cleanup_notice(notices, event):
    # Reporting cleanup is secondary to the transaction's existing outcome.
    if notices is not None:
        try: notices(event)
        except BaseException: pass


@contextmanager
def _exclusive_lock(path, resume, notices=None):
    """Keep a stable lock file and kernel-held lease; never reclaim by PID."""
    if path.is_symlink(): raise InstallerError('Install lock is a symlink')
    backend = 'windows-msvcrt' if os.name == 'nt' else 'posix-flock'
    flags = os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0)
    created = True
    try:
        try:
            # Only atomic exclusive creation grants fresh initialization. A
            # pre-open pathname check cannot establish which process created it.
            handle = os.open(path, flags | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            created = False
            if not resume:
                raise InstallerError('Installation lock exists; explicit resume is required')
            # Never recreate a file that disappears between EEXIST and this open.
            handle = os.open(path, flags)
    except OSError as exc:
        raise InstallerError('Cannot open the local installation lock') from exc
    acquired = False
    succeeded = False
    try:
        info = os.fstat(handle)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise InstallerError('Installation lock must be an ordinary file with one hard link')
        if backend == 'posix-flock':
            import fcntl
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as exc:
                raise InstallerError('Another installer holds this output, or kernel locking is unavailable') from exc
        else:
            import msvcrt
            try:
                os.lseek(handle, 0, os.SEEK_SET)
                msvcrt.locking(handle, msvcrt.LK_NBLCK, 1)
            except OSError as exc:
                raise InstallerError('Another installer holds this output, or Windows locking is unavailable') from exc
        acquired = True
        current = path.stat(follow_symlinks=False)
        if (current.st_dev, current.st_ino) != (info.st_dev, info.st_ino):
            raise InstallerError('Installation lock pathname changed; refusing staging access')
        if not created:
            os.lseek(handle, 0, os.SEEK_SET)
            metadata = os.read(handle, 4097)
            if not metadata or len(metadata) > 4096:
                raise InstallerError('Empty or unrecognized legacy lock; retained for manual inspection')
            try: recorded = json.loads(metadata)
            except (ValueError, UnicodeError) as exc:
                raise InstallerError('Unrecognized legacy lock; retained for manual inspection') from exc
            if not isinstance(recorded, dict) or recorded.get('locking') != backend:
                raise InstallerError('Legacy PID lock is not a kernel lock; retained for manual inspection')
        os.lseek(handle, 0, os.SEEK_SET)
        os.ftruncate(handle, 0)
        metadata = json.dumps({'schema_version': 1, 'locking': backend, 'pid': os.getpid()}).encode()
        with os.fdopen(os.dup(handle), 'wb') as stream:
            stream.write(metadata)
            stream.flush()
            os.fsync(stream.fileno())
        yield
        succeeded = True
    finally:
        failures = []
        if acquired:
            try: _release_lock(handle, backend)
            except BaseException as exc: failures.append(exc)
        # Closing still releases the kernel lock if explicit unlock failed.
        try: os.close(handle)
        except BaseException as exc: failures.append(exc)
        for failure in failures:
            _cleanup_notice(notices, {'event': 'lock-cleanup-notice',
                                     'failure_type': type(failure).__name__,
                                     'transaction_outcome': 'complete' if succeeded else 'failed'})


def _separate(source, output, cache):
    if output.exists() or output.is_symlink():
        raise InstallerError('Output already exists; choose a new directory')
    if not output.parent.is_dir() or output.parent.is_symlink():
        raise InstallerError('Choose an output under an existing ordinary parent directory')
    source_real, output_real, cache_real = source.resolve(), output.resolve(), cache.resolve()
    for writable in (output_real, cache_real):
        if writable.is_relative_to(source_real) or source_real.is_relative_to(writable):
            raise InstallerError('Output and cache must be separate from the owned input tree')
    if cache_real.is_relative_to(output_real) or output_real.is_relative_to(cache_real):
        raise InstallerError('Output and cache must be separate directories')


def preflight(profile, source, output, cache):
    source, output, cache = (Path(p).absolute() for p in (source, output, cache))
    _separate(source, output, cache)
    if not source.is_dir() or source.is_symlink():
        raise InstallerError('Select an existing ordinary game directory')
    if cache.is_symlink(): raise InstallerError('Cache root must not be a symlink')
    return estimate(profile, source, output, cache)


def _snapshot(profile, source, destination, token):
    destination.mkdir()
    for item in profile.data['identity']['inputs']:
        token.check()
        original = contained(source, item['path'])
        target = contained(destination, item['path'])
        target.parent.mkdir(parents=True, exist_ok=True)
        with original.open('rb') as source_file, target.open('xb') as output:
            copied = 0
            while block := source_file.read(1024 * 1024):
                token.check()
                copied += len(block)
                if copied > item['size']: raise InstallerError('Input changed during local snapshot')
                output.write(block)
        size, digest = file_digest(target, token, item['size'])
        if size != item['size'] or digest != item['sha256']:
            raise InstallerError('Input changed during local snapshot')
        target.chmod(0o444)


def _environment(stage, tool_roots):
    home, temporary = stage / 'home', stage / 'tmp'
    home.mkdir(exist_ok=True)
    temporary.mkdir(exist_ok=True)
    env = {'PATH': os.pathsep.join(str(p) for p in tool_roots),
           'HOME': str(home), 'USERPROFILE': str(home),
           'TMP': str(temporary), 'TEMP': str(temporary), 'TMPDIR': str(temporary),
           'LANG': 'C', 'LC_ALL': 'C'}
    if os.name == 'nt' and 'SystemRoot' in os.environ:
        env['SystemRoot'] = os.environ['SystemRoot']
    return env


def _remove_owned(stage):
    def writable_retry(function, path, exc_info):
        exc = exc_info[1]
        # Only regular read-only files in the recognized tree are chmodded.
        if Path(path).is_symlink() or not Path(path).is_file(): raise exc
        Path(path).chmod(stat.S_IWRITE | stat.S_IREAD)
        function(path)
    for name in ('work', 'tools', 'inputs', 'application', 'home', 'tmp'):
        path = contained(stage, name)
        if path.exists():
            if not path.is_dir(): raise InstallerError('Unexpected file occupies a staging directory')
            shutil.rmtree(path, onerror=writable_retry)


def install(profile, source, output, cache, resume=False, synthetic=False, token=None, events=None, keep_work=False):
    token = token or Cancellation()
    events = events or (lambda event: None)
    source, output, cache = (Path(p).absolute() for p in (source, output, cache))
    host = host_key()
    adapter = profile.adapter(host)
    if (profile.data['kind'] == 'synthetic') != synthetic:
        raise InstallerError('Synthetic demonstrations require explicit --synthetic and cannot run production recipes')
    if synthetic and adapter['stages']:
        raise InstallerError('Synthetic mode never executes profile-supplied commands')
    _separate(source, output, cache)
    token.check()
    events({'event': 'stage-start', 'stage': 'validate'})
    identity = validate_inputs(profile, source, token)
    preflight(profile, source, output, cache)
    stage = output.parent / f'.{output.name}.mag-staging'
    lock = output.parent / f'.{output.name}.mag-lock'
    if stage.is_symlink() or cache.is_symlink():
        raise InstallerError('Staging/cache symlinks are unsupported')
    fingerprint = {'schema_version': 1, 'profile': profile.digest, 'host': host,
                   'source': str(source.resolve()), 'output': str(output.resolve()),
                   'identity': identity, 'synthetic': synthetic}
    def lock_notice(event):
        try:
            logs = contained(stage, 'logs')
            if logs.is_dir():
                with (logs / 'events.jsonl').open('a', encoding='utf-8') as stream:
                    stream.write(json.dumps(event) + '\n')
        except Exception:
            pass
        _cleanup_notice(events, event)
    with _exclusive_lock(lock, resume, lock_notice):
        # Recheck under the kernel lock: a contender may have validated paths
        # before another transaction committed and then released this inode.
        if output.exists() or output.is_symlink():
            raise InstallerError('Output already exists; choose a new directory')
        journal_path = stage / 'journal.json'
        if stage.exists():
            if not resume: raise InstallerError('An interrupted staging directory exists; use explicit --resume')
            if journal_path.is_symlink(): raise InstallerError('Journal cannot be a symlink')
            try:
                journal = json.loads(journal_path.read_text(encoding='utf-8'))
            except (OSError, ValueError) as exc:
                raise InstallerError('Staging journal is missing or invalid; retained for inspection') from exc
            if journal.get('fingerprint') != fingerprint:
                raise InstallerError('Cannot resume: profile, input identity, host, source or destination changed')
        else:
            if resume: raise InstallerError('No prior local staging exists to resume')
            stage.mkdir(mode=0o700)
            journal = {'fingerprint': fingerprint, 'state': 'starting'}
        _write_json(journal_path, journal)
        logs = contained(stage, 'logs')
        logs.mkdir(exist_ok=True)
        def emit(event):
            with (logs / 'events.jsonl').open('a', encoding='utf-8') as stream:
                stream.write(json.dumps(event, ensure_ascii=False) + '\n')
            events(event)
            token.check()
        def begin(name):
            journal['state'] = name
            _write_json(journal_path, journal)
            emit({'event': 'stage-start', 'stage': name})
        try:
            # Recovery replays all stages from reverified inputs, preserving old logs.
            _remove_owned(stage)
            work, tools, application = stage / 'work', stage / 'tools', stage / 'application'
            work.mkdir()
            tools.mkdir()
            application.mkdir()
            _snapshot(profile, source, stage / 'inputs', token)
            begin('acquire')
            executables = {}
            for tool in adapter['tools']:
                blob = acquire(tool, cache, token, emit)
                tool_root = tools / tool['id']
                extract_archive(blob, tool_root, tool, token)
                executables[tool['id']] = contained(tool_root, tool['executable'])
            env = _environment(stage, [path.parent for path in executables.values()])
            if synthetic:
                for name in ('generate', 'build', 'package'):
                    begin(name)
                    (work / f'{name}.txt').write_text(f'SYNTHETIC {name}; no game build occurred.\n', encoding='utf-8')
                (application / 'SYNTHETIC-RESULT.txt').write_text('SYNTHETIC installer demonstration only. This is not a MAG application.\n', encoding='utf-8')
            else:
                substitutions = {'input': str(stage / 'inputs'), 'work': str(work),
                                 'tools': str(tools), 'package': str(work / 'recipe-package')}
                for recipe in adapter['stages']:
                    begin(recipe['name'])
                    try: arguments = [a.format_map(substitutions) for a in recipe['argv']]
                    except (ValueError, KeyError) as exc: raise InstallerError('Recipe contains unknown placeholders') from exc
                    run_command([str(executables[recipe['tool']]), *arguments], work,
                                logs / f'{recipe["name"]}.log', token, env, emit, recipe["name"])
                    for expected in recipe['outputs']:
                        if not contained(work, expected).is_file(): raise InstallerError(f'Stage did not create a required output; inspect {logs / (recipe["name"] + ".log")}')
                # Only explicit ordinary files enter the local final application.
                for name in adapter['package']:
                    original, target = contained(work, name), contained(application, name)
                    if not original.is_file(): raise InstallerError('Required application output is absent')
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(original, target)
                    target.chmod(original.stat().st_mode & 0o777)
            validate_inputs(profile, source, token)
            token.check()
            journal['state'] = 'publishing'
            _write_json(journal_path, journal)
            emit({'event': 'publish-start', 'synthetic': synthetic})
            publish_directory(application, output)
            journal['state'] = 'complete'
            # Publication is the commit point. Late journal/viewer failures cannot
            # undo it or turn a completed transaction into a failed/cancelled one.
            try:
                _write_json(journal_path, journal)
                if not keep_work:
                    try: _remove_owned(stage)
                    except Exception as cleanup_error:
                        lock_notice({'event': 'work-cleanup-notice', 'failure_type': type(cleanup_error).__name__,
                                     'transaction_outcome': 'complete', 'logs': str(logs)})
                event = {'event': 'synthetic-complete' if synthetic else 'local-build-complete', 'synthetic': synthetic,
                         'output': str(output), 'logs': str(logs)}
                with (logs / 'events.jsonl').open('a', encoding='utf-8') as stream: stream.write(json.dumps(event) + '\n')
                events(event)
            except Exception as exc:
                try:
                    with (logs / 'events.jsonl').open('a', encoding='utf-8') as stream:
                        stream.write(json.dumps({'event': 'post-commit-notice-failed', 'failure_type': type(exc).__name__}) + '\n')
                except OSError:
                    pass
            return output
        except BaseException as exc:
            journal['state'] = 'cancelled' if token._event.is_set() else 'failed'
            journal['failure_type'] = type(exc).__name__
            try:
                _write_json(journal_path, journal)
            except BaseException as reporting_failure:
                _cleanup_notice(lock_notice, {'event': 'failure-reporting-notice',
                                             'failure_type': type(reporting_failure).__name__,
                                             'transaction_outcome': 'failed'})
            raise
