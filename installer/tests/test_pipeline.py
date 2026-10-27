import re
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zipfile
from unittest.mock import patch

from mag_installer.model import InstallerError, Profile, host_key
import mag_installer.pipeline as pipeline_module
from mag_installer.pipeline import install, publish_directory, run_command, _exclusive_lock
from mag_installer.safety import Cancellation, Cancelled
from test_model import profile_data


class PipelineTests(unittest.TestCase):
    def setup_source(self, temp):
        root = Path(temp)
        source = root / 'owned input 日本語'
        path = source / 'game data/入力.txt'
        path.parent.mkdir(parents=True)
        path.write_bytes(b'synthetic input\n')
        return root, source, Profile.from_dict(profile_data())

    def test_synthetic_pipeline_stages_local_output_and_preserves_input(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            events = []
            output = root / 'new output 日本語'
            result = install(profile, source, output, root / 'cache', synthetic=True, events=events.append)
            self.assertEqual(result, output)
            self.assertIn('SYNTHETIC', (output / 'SYNTHETIC-RESULT.txt').read_text())
            self.assertFalse((output / 'game data').exists())
            self.assertEqual((source / 'game data/入力.txt').read_bytes(), b'synthetic input\n')
            self.assertEqual([e['stage'] for e in events if e['event'] == 'stage-start'], ['validate', 'acquire', 'generate', 'build', 'package'])
            self.assertEqual(events[-1]['event'], 'synthetic-complete')
            journal = json.loads((root / '.new output 日本語.mag-staging/journal.json').read_text())
            self.assertEqual(journal['state'], 'complete')

    def test_success_cleanup_retains_logs_and_keep_work_opts_out(self):
        for keep in (False, True):
            with self.subTest(keep=keep), tempfile.TemporaryDirectory() as temp:
                root, source, profile = self.setup_source(temp)
                events = []
                install(profile, source, root / 'out', root / 'cache', synthetic=True,
                        keep_work=keep, events=events.append)
                stage = root / '.out.mag-staging'
                self.assertEqual((stage / 'inputs').exists(), keep)
                self.assertEqual((stage / 'work').exists(), keep)
                self.assertTrue((stage / 'logs/events.jsonl').is_file())
                self.assertEqual(events[-1]['output'], str(root / 'out'))
                self.assertEqual(events[-1]['logs'], str(stage / 'logs'))

    def test_expected_version_error_names_identity_label(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            (source / 'game data/入力.txt').write_bytes(b'bad')
            with self.assertRaisesRegex(InstallerError, 'Synthetic test fixture'):
                install(profile, source, root / 'out', root / 'cache', synthetic=True)

    def test_install_checks_capacity_before_lock_or_staging_writes(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            with patch('mag_installer.capacity.volume', return_value=(1, 0)):
                with self.assertRaisesRegex(InstallerError, 'space'):
                    install(profile, source, root / 'out', root / 'cache', synthetic=True)
            self.assertFalse((root / '.out.mag-lock').exists())
            self.assertFalse((root / '.out.mag-staging').exists())

    def test_cleanup_failure_does_not_relabel_committed_success(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            with patch('mag_installer.pipeline._remove_owned', side_effect=[None, PermissionError('denied')]):
                self.assertEqual(install(profile, source, root / 'out', root / 'cache', synthetic=True), root / 'out')
            self.assertEqual(json.loads((root / '.out.mag-staging/journal.json').read_text())['state'], 'complete')

    def test_long_command_emits_activity_and_error_names_log(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            events = []
            with patch('mag_installer.pipeline.ACTIVITY_INTERVAL', 0.01):
                with self.assertRaisesRegex(InstallerError, re.escape(str(root / 'build.log'))):
                    run_command([sys.executable, '-c', 'import time; time.sleep(.25); raise SystemExit(3)'],
                                root, root / 'build.log', Cancellation(), {}, events.append, 'build')
            self.assertTrue(any(e['event'] == 'stage-activity' and e['stage'] == 'build' for e in events))

    def test_progress_sink_failure_after_commit_does_not_misreport_failed_install(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            output = root / 'out'
            def broken_sink(event):
                if event['event'] == 'synthetic-complete':
                    raise BrokenPipeError('viewer closed after publication')
            result = install(profile, source, output, root / 'cache', synthetic=True, events=broken_sink)
            self.assertEqual(result, output)
            journal = json.loads((root / '.out.mag-staging/journal.json').read_text())
            self.assertEqual(journal['state'], 'complete')
            self.assertTrue((output / 'SYNTHETIC-RESULT.txt').is_file())

    def test_blocked_foreign_and_synthetic_without_opt_in_have_no_side_effects(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            with self.assertRaises(InstallerError): install(profile, source, root / 'out', root / 'cache')
            data = profile_data(host='windows-x86_64' if host_key() != 'windows-x86_64' else 'linux-x86_64')
            with self.assertRaises(InstallerError): install(Profile.from_dict(data), source, root / 'out', root / 'cache', synthetic=True)
            data = profile_data()
            data['platforms'][host_key()]['status'] = 'blocked'
            with self.assertRaises(InstallerError): install(Profile.from_dict(data), source, root / 'out', root / 'cache', synthetic=True)
            self.assertFalse((root / 'cache').exists())
            self.assertFalse((root / 'out').exists())

    def test_existing_output_nested_source_and_cache_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            output = root / 'out'
            output.mkdir()
            (output / 'keep.txt').write_text('keep')
            with self.assertRaises(InstallerError): install(profile, source, output, root / 'cache', synthetic=True)
            self.assertEqual((output / 'keep.txt').read_text(), 'keep')
            for output, cache in [(source / 'new', root / 'cache'), (root / 'new', source / 'cache'), (root, root / 'cache')]:
                with self.subTest(output=output), self.assertRaises(InstallerError): install(profile, source, output, cache, synthetic=True)

    def test_cancelled_job_requires_explicit_resume_and_rejects_stale_profile(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            output = root / 'out'
            token = Cancellation()
            def cancel(event):
                if event.get('stage') == 'build': token.cancel()
            with self.assertRaises(Cancelled): install(profile, source, output, root / 'cache', synthetic=True, token=token, events=cancel)
            self.assertFalse(output.exists())
            with self.assertRaises(InstallerError): install(profile, source, output, root / 'cache', synthetic=True)
            changed = profile_data()
            changed['revision'] = 2
            with self.assertRaises(InstallerError): install(Profile.from_dict(changed), source, output, root / 'cache', resume=True, synthetic=True)
            install(profile, source, output, root / 'cache', resume=True, synthetic=True)
            self.assertTrue((output / 'SYNTHETIC-RESULT.txt').is_file())

    @unittest.skipIf(os.name == 'nt', 'POSIX stable-inode locking regression')
    def test_two_explicit_resumers_never_share_or_clobber_a_transaction(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            output = root / 'out'
            token = Cancellation()
            def stop_seed(event):
                if event.get('stage') == 'build': token.cancel()
            with self.assertRaises(Cancelled):
                install(profile, source, output, root / 'cache', synthetic=True, token=token, events=stop_seed)
            lock = root / '.out.mag-lock'
            lock.write_text(json.dumps({'schema_version': 1, 'locking': 'posix-flock', 'pid': 2147483647}))
            original_inode = lock.stat().st_ino
            stale_probes = threading.Barrier(2)
            first_entered = threading.Event()
            both_attempted = threading.Event()
            release = threading.Event()
            start = threading.Barrier(3)
            state_lock = threading.Lock()
            state = {'active': 0, 'maximum': 0, 'attempted': 0}
            results = []
            real_kill = os.kill
            def scheduled_stale_probe(pid, signum):
                # Reproduce the former read-old-PID/unlink-new-lock race. The
                # kernel-lock implementation never needs this PID probe.
                if pid != 2147483647: return real_kill(pid, signum)
                stale_probes.wait(timeout=3)
                if threading.current_thread().name == 'resumer-two':
                    if not first_entered.wait(timeout=3): raise AssertionError('first resumer did not enter')
                raise ProcessLookupError()
            def attempted():
                state['attempted'] += 1
                if state['attempted'] == 2: both_attempted.set()
            def worker():
                entered = False
                name = threading.current_thread().name
                def observe(event):
                    nonlocal entered
                    if event.get('stage') != 'acquire': return
                    entered = True
                    with state_lock:
                        state['active'] += 1
                        state['maximum'] = max(state['maximum'], state['active'])
                        attempted()
                    (root / '.out.mag-staging/work/protected.txt').write_text(name)
                    first_entered.set()
                    if not release.wait(timeout=4): raise AssertionError('test did not release transaction')
                    with state_lock: state['active'] -= 1
                try:
                    start.wait(timeout=3)
                    result = install(profile, source, output, root / 'cache', resume=True, synthetic=True, events=observe, keep_work=True)
                    with state_lock: results.append(('success', name, str(result)))
                except Exception as exc:
                    with state_lock:
                        if not entered: attempted()
                        results.append(('error', name, exc))
            threads = [threading.Thread(target=worker, name=name) for name in ('resumer-one', 'resumer-two')]
            with patch('mag_installer.pipeline.os.kill', side_effect=scheduled_stale_probe):
                for thread in threads: thread.start()
                start.wait(timeout=3)
                try:
                    self.assertTrue(both_attempted.wait(timeout=4), results)
                finally:
                    release.set()
                    for thread in threads: thread.join(timeout=5)
            self.assertTrue(all(not thread.is_alive() for thread in threads))
            self.assertEqual(state['maximum'], 1, results)
            successes = [result for result in results if result[0] == 'success']
            failures = [result for result in results if result[0] == 'error']
            self.assertEqual(len(successes), 1, results)
            self.assertEqual(len(failures), 1, results)
            self.assertIsInstance(failures[0][2], InstallerError)
            self.assertEqual((root / '.out.mag-staging/work/protected.txt').read_text(), successes[0][1])
            self.assertEqual(lock.stat().st_ino, original_inode)
            # Released kernel locks permit explicit recovery on the same inode;
            # the pathname is never unlinked/recreated as a PID lease.
            with _exclusive_lock(lock, resume=True):
                with self.assertRaises(InstallerError):
                    with _exclusive_lock(lock, resume=True): pass
            self.assertEqual(lock.stat().st_ino, original_inode)

    def test_lock_release_failure_after_commit_retains_success_and_reports_notice(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            output = root / 'out'
            events = []
            def observe(event):
                events.append(event)
                if event['event'] == 'lock-cleanup-notice': raise BrokenPipeError('cleanup viewer closed')
            real_unlink = Path.unlink
            def deny_old_lock_cleanup(path, *args, **kwargs):
                if path == root / '.out.mag-lock': raise PermissionError('release denied')
                return real_unlink(path, *args, **kwargs)
            with patch('mag_installer.pipeline._release_lock', create=True, side_effect=PermissionError('release denied')), \
                    patch('mag_installer.pipeline.Path.unlink', autospec=True, side_effect=deny_old_lock_cleanup):
                result = install(profile, source, output, root / 'cache', synthetic=True, events=observe)
            self.assertEqual(result, output)
            self.assertTrue((output / 'SYNTHETIC-RESULT.txt').is_file())
            self.assertEqual(json.loads((root / '.out.mag-staging/journal.json').read_text())['state'], 'complete')
            self.assertTrue(any(event['event'] == 'synthetic-complete' for event in events))
            self.assertTrue(any(event['event'] == 'lock-cleanup-notice' for event in events))

    def test_precommit_failure_remains_original_when_lock_release_also_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            output = root / 'out'
            original = InstallerError('original generation failure')
            events = []
            def fail_generation(event):
                events.append(event)
                if event.get('stage') == 'generate': raise original
            real_unlink = Path.unlink
            def deny_old_lock_cleanup(path, *args, **kwargs):
                if path == root / '.out.mag-lock': raise PermissionError('release denied')
                return real_unlink(path, *args, **kwargs)
            with patch('mag_installer.pipeline._release_lock', create=True, side_effect=PermissionError('release denied')), \
                    patch('mag_installer.pipeline.Path.unlink', autospec=True, side_effect=deny_old_lock_cleanup):
                with self.assertRaises(InstallerError) as caught:
                    install(profile, source, output, root / 'cache', synthetic=True, events=fail_generation)
            self.assertIs(caught.exception, original)
            self.assertFalse(output.exists())
            self.assertEqual(json.loads((root / '.out.mag-staging/journal.json').read_text())['state'], 'failed')
            self.assertTrue(any(event['event'] == 'lock-cleanup-notice' for event in events))

    def test_precommit_original_survives_failure_journal_and_cleanup_reporting_errors(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            original = InstallerError('original generation failure')
            real_write = pipeline_module._write_json
            def disk_full_on_failure(path, data):
                if data.get('state') == 'failed': raise OSError('failure journal disk full')
                return real_write(path, data)
            def fail_generation_and_notices(event):
                if event.get('stage') == 'generate': raise original
                if event['event'].endswith('notice'): raise PermissionError('notice viewer unavailable')
            with patch('mag_installer.pipeline._write_json', side_effect=disk_full_on_failure), \
                    patch('mag_installer.pipeline._release_lock', side_effect=PermissionError('release denied')):
                with self.assertRaises(InstallerError) as caught:
                    install(profile, source, root / 'out', root / 'cache', synthetic=True, events=fail_generation_and_notices)
            self.assertIs(caught.exception, original)
            self.assertFalse((root / 'out').exists())

    @unittest.skipIf(os.name == 'nt', 'POSIX preexisting-empty-lock boundary')
    def test_preexisting_empty_lock_resume_refuses_without_mutating_lock_or_transaction(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            output = root / 'out'
            token = Cancellation()
            def stop_seed(event):
                if event.get('stage') == 'build': token.cancel()
            with self.assertRaises(Cancelled):
                install(profile, source, output, root / 'cache', synthetic=True, token=token, events=stop_seed)
            lock = root / '.out.mag-lock'
            lock.write_bytes(b'')
            lock_before = lock.stat()
            stage = root / '.out.mag-staging'
            journal_before = (stage / 'journal.json').read_bytes()
            marker = stage / 'work/preserve.txt'
            marker.write_bytes(b'preserve interrupted transaction')
            events = []
            with self.assertRaises(InstallerError):
                install(profile, source, output, root / 'cache', resume=True, synthetic=True, events=events.append)
            self.assertEqual(lock.read_bytes(), b'')
            self.assertEqual(lock.stat().st_ino, lock_before.st_ino)
            self.assertEqual(lock.stat().st_mtime_ns, lock_before.st_mtime_ns)
            self.assertEqual((stage / 'journal.json').read_bytes(), journal_before)
            self.assertEqual(marker.read_bytes(), b'preserve interrupted transaction')
            self.assertEqual([event.get('stage') for event in events], ['validate'])
            self.assertFalse(output.exists())

    @unittest.skipIf(os.name == 'nt', 'POSIX atomic creator distinction')
    def test_legacy_lock_materializing_at_create_boundary_is_never_reinitialized(self):
        for payload in (b'', b'{"pid": 2147483647}'):
            for resume in (False, True):
                with self.subTest(payload=payload, resume=resume), tempfile.TemporaryDirectory() as temp:
                    root, source, profile = self.setup_source(temp)
                    output = root / 'out'
                    token = Cancellation()
                    def stop_seed(event):
                        if event.get('stage') == 'build': token.cancel()
                    with self.assertRaises(Cancelled):
                        install(profile, source, output, root / 'cache', synthetic=True, token=token, events=stop_seed)
                    lock = root / '.out.mag-lock'
                    lock.unlink()  # Test setup only: the legacy process arrives at open below.
                    stage = root / '.out.mag-staging'
                    journal_before = (stage / 'journal.json').read_bytes()
                    marker = stage / 'work/preserve.txt'
                    marker.write_bytes(b'preserve interrupted transaction')
                    real_open = os.open
                    injected = []
                    def legacy_arrives(path, flags, *args, **kwargs):
                        if os.fspath(path) == os.fspath(lock) and not injected:
                            lock.write_bytes(payload)
                            injected.append(lock.stat())
                        return real_open(path, flags, *args, **kwargs)
                    events = []
                    with patch('mag_installer.pipeline.os.open', side_effect=legacy_arrives):
                        with self.assertRaises(InstallerError):
                            install(profile, source, output, root / 'cache', resume=resume, synthetic=True, events=events.append)
                    self.assertEqual(lock.read_bytes(), payload)
                    self.assertEqual(lock.stat().st_ino, injected[0].st_ino)
                    self.assertEqual(lock.stat().st_mtime_ns, injected[0].st_mtime_ns)
                    self.assertEqual((stage / 'journal.json').read_bytes(), journal_before)
                    self.assertEqual(marker.read_bytes(), b'preserve interrupted transaction')
                    self.assertEqual([event.get('stage') for event in events], ['validate'])
                    self.assertFalse(output.exists())

    def test_windows_stale_lock_is_retained_without_a_posix_signal_probe(self):
        with tempfile.TemporaryDirectory() as temp:
            lock = Path(temp) / 'lock'
            lock.write_text(json.dumps({'pid': 2147483647}))
            with patch('mag_installer.pipeline.os.kill', side_effect=AssertionError('PID probe forbidden')):
                with self.assertRaises(InstallerError):
                    with _exclusive_lock(lock, resume=True):
                        pass
            self.assertTrue(lock.exists())

    @unittest.skipUnless(os.name == 'nt', 'Native Windows OS locking requires Windows')
    def test_windows_process_lock_recovers_same_file_after_holder_exit(self):
        with tempfile.TemporaryDirectory() as temp:
            lock = Path(temp) / 'lock'
            script = ("from pathlib import Path; from mag_installer.pipeline import _exclusive_lock; "
                      "import sys,time; "
                      "ctx=_exclusive_lock(Path(sys.argv[1]),False); ctx.__enter__(); "
                      "print('locked',flush=True); time.sleep(30)")
            child = subprocess.Popen([sys.executable, '-c', script, str(lock)], stdout=subprocess.PIPE, text=True)
            try:
                self.assertEqual(child.stdout.readline().strip(), 'locked')
                original = lock.stat().st_ino
                with self.assertRaises(InstallerError):
                    with _exclusive_lock(lock, True): pass
            finally:
                child.terminate(); child.wait(timeout=5); child.stdout.close()
            with _exclusive_lock(lock, True): pass
            self.assertEqual(lock.stat().st_ino, original)

    def test_resume_revalidates_input_and_refuses_active_lock(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, profile = self.setup_source(temp)
            output = root / 'out'
            lock = root / '.out.mag-lock'
            lock.write_text(json.dumps({'pid': os.getpid()}))
            with self.assertRaises(InstallerError): install(profile, source, output, root / 'cache', resume=True, synthetic=True)
            self.assertTrue(lock.exists())
            lock.unlink()
            token = Cancellation()
            def cancel(event):
                if event.get('stage') == 'build': token.cancel()
            with self.assertRaises(Cancelled): install(profile, source, output, root / 'cache', synthetic=True, token=token, events=cancel)
            (source / 'game data/入力.txt').write_bytes(b'wrong version')
            with self.assertRaises(InstallerError): install(profile, source, output, root / 'cache', resume=True, synthetic=True)
            self.assertFalse(output.exists())

    @unittest.skipIf(os.name == 'nt', 'Fixture is a POSIX Python script; Windows needs native fixture executable certification')
    def test_managed_generic_recipe_uses_snapshot_pinned_tools_and_package_allowlist(self):
        with tempfile.TemporaryDirectory() as temp:
            root, source, _ = self.setup_source(temp)
            script = (f'#!{sys.executable}\n' + "import pathlib,sys\n"
                      "stage=sys.argv[1]\n"
                      "if stage=='generate':\n"
                      " data=(pathlib.Path(sys.argv[2])/'game data/入力.txt').read_bytes()\n"
                      " pathlib.Path('generated.txt').write_bytes(data)\n"
                      "if stage=='build': pathlib.Path('application.txt').write_text('Generic synthetic tool artifact')\n"
                      "if stage=='package':\n"
                      " target=pathlib.Path(sys.argv[2]); target.mkdir(exist_ok=True)\n"
                      " (target/'UNLISTED-PRIVATE.bin').write_text('must not publish')\n")
            stream = io.BytesIO()
            with zipfile.ZipFile(stream, 'w') as archive: archive.writestr('bin/tool', script.encode())
            payload = stream.getvalue()
            digest = hashlib.sha256(payload).hexdigest()
            cache = root / 'cache'
            cache.mkdir()
            (cache / f'{digest}.blob').write_bytes(payload)
            data = profile_data()
            data['kind'] = 'production'  # Generic reviewed recipe fixture; never a MAG profile.
            adapter = data['platforms'][host_key()]
            adapter['tools'] = [{'id': 'fixture', 'version': '1-test', 'url': 'https://not-contacted.invalid/tool.zip',
                                 'sha256': digest, 'size': len(payload), 'format': 'zip', 'max_expanded_size': 10000,
                                 'max_entries': 4, 'executable': 'bin/tool', 'license': 'Synthetic test only'}]
            adapter['stages'] = [
                {'name': 'generate', 'tool': 'fixture', 'argv': ['generate', '{input}'], 'outputs': ['generated.txt']},
                {'name': 'build', 'tool': 'fixture', 'argv': ['build'], 'outputs': ['application.txt']},
                {'name': 'package', 'tool': 'fixture', 'argv': ['package', '{package}'], 'outputs': []}]
            adapter['package'] = ['application.txt']
            output = root / 'out'
            install(Profile.from_dict(data), source, output, cache)
            self.assertEqual((output / 'application.txt').read_text(), 'Generic synthetic tool artifact')
            self.assertFalse((output / 'UNLISTED-PRIVATE.bin').exists())
            self.assertFalse((output / 'generated.txt').exists())

    @unittest.skipIf(os.name == 'nt', 'POSIX process group boundary; Windows needs native certification')
    def test_cancellation_kills_nested_child_even_if_it_ignores_term(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            token = Cancellation()
            child = "import signal,time,pathlib; signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(1); pathlib.Path('BAD-CHILD').touch(); time.sleep(30)"
            parent = f"import subprocess,sys,time,pathlib; p=subprocess.Popen([sys.executable,'-c',{child!r}]); pathlib.Path('child-started').touch(); time.sleep(30)"
            timer = threading.Timer(0.4, token.cancel)
            timer.start()
            try:
                with self.assertRaises(Cancelled): run_command([sys.executable, '-c', parent], root, root / 'log', token, {})
            finally: timer.join()
            time.sleep(1.1)
            self.assertTrue((root / 'child-started').exists())
            self.assertFalse((root / 'BAD-CHILD').exists())

    def test_atomic_publish_never_replaces_even_empty_existing_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'staged'
            source.mkdir()
            (source / 'new').write_text('new')
            target = root / 'existing'
            target.mkdir()
            with self.assertRaises(InstallerError): publish_directory(source, target)
            self.assertEqual(list(target.iterdir()), [])
            publish_directory(source, root / 'published')
            self.assertEqual((root / 'published/new').read_text(), 'new')

    def test_subprocess_argv_preserves_spaces_and_shell_text_as_data(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            message = '日本語 spaced; $(touch BAD) `touch BAD`'
            script = "import pathlib,sys; pathlib.Path('observed.txt').write_text(sys.argv[1], encoding='utf-8')"
            run_command([sys.executable, '-c', script, message], root, root / 'log', Cancellation(), {})
            self.assertEqual((root / 'observed.txt').read_text(encoding='utf-8'), message)
            self.assertFalse((root / 'BAD').exists())

    def test_cancellation_terminates_running_command(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            token = Cancellation()
            timer = threading.Timer(0.3, token.cancel)
            timer.start()
            started = time.monotonic()
            try:
                with self.assertRaises(Cancelled):
                    run_command([sys.executable, '-c', "import time,pathlib; pathlib.Path('started').write_text('yes'); time.sleep(30); pathlib.Path('BAD').touch()"], root, root / 'log', token, {})
            finally: timer.join()
            self.assertLess(time.monotonic() - started, 6)
            self.assertTrue((root / 'started').exists())
            self.assertFalse((root / 'BAD').exists())


if __name__ == '__main__': unittest.main()
