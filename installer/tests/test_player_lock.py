"""The player build's lock beside the MAG folder (installer/docs/design.md, "Player build lock")."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import ExitStack
from unittest.mock import patch

from mag_installer import player_build
from mag_installer.model import InstallerError
from mag_installer.pipeline import _exclusive_lock

BACKEND = 'windows-msvcrt' if os.name == 'nt' else 'posix-flock'
INSTALLER = Path(__file__).resolve().parents[1]


def write_inputs(root):
    for name in ('PS3_GAME/PARAM.SFO', 'PS3_GAME/USRDIR/EBOOT.BIN',
                 'PS3_GAME/USRDIR/EBOOT.elf', 'PS3_GAME/USRDIR/bin/mage_g.self',
                 'PS3_GAME/USRDIR/bin/mage_g.elf', 'firmware/libsre.prx'):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'synthetic input')


def synthetic_build(stack, fail=False):
    """Patch every tool, compile and package step with synthetic stand-ins (macOS route)."""
    kit = {'source_tree': 'source', 'sdks': {role: {'path': 'sdks/' + role, 'revision': 'a' * 40}
           for role in ('analysis', 'ppu', 'runtime', 'spu')}}
    def tools(managed, **kwargs):
        return {'root': '/managed', 'python': '/managed/python', 'cmake': '/managed/cmake', 'ninja': '/managed/ninja',
                'git': '/usr/bin/git', 'ffmpeg': {}}
    def native(config_path, native_output, **kwargs):
        (native_output / 'workspace').mkdir(parents=True)
    def package(config, workspace, destination, **kwargs):
        if fail:
            raise InstallerError('synthetic package failure')
        destination.mkdir(); (destination.parent / 'game').mkdir()
        return destination
    for item in (patch.object(player_build, 'readiness', return_value={'ready': True}),
                 patch.object(player_build, '_manifest', return_value={'kit': {}, 'host': 'macos-arm64'}),
                 patch.object(player_build, '_load_kit', return_value=kit),
                 patch('mag_installer.native_build._verify_identities'),
                 patch('mag_installer.managed_tools.ensure_apple_sdk'),
                 patch('mag_installer.managed_tools.bootstrap', side_effect=tools),
                 patch('mag_installer.managed_tools.rust_toolchain', return_value={'cargo': '/c', 'rustc': '/r'}),
                 patch('mag_installer.compile_cache.ccache_tool', return_value='/managed/ccache'),
                 patch('mag_installer.native_build.install', side_effect=native),
                 patch('mag_installer.native_package.package', side_effect=package)):
        stack.enter_context(item)


class PlayerLockTests(unittest.TestCase):
    def setUp(self):
        environment = patch.dict('os.environ')
        environment.start(); self.addCleanup(environment.stop)
        for name in ('MAG_GAME_CONTROLS', 'MAG_BUILD_CACHE'):
            os.environ.pop(name, None)
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        self.source = self.root / 'inputs'; write_inputs(self.source)
        self.output = self.root / 'MAG'; self.build = self.root / 'MAG-build'; self.cache = self.root / 'cache'
        self.lock = self.root / '.MAG.player-lock'

    def install(self, fail=False, **kwargs):
        with ExitStack() as stack:
            synthetic_build(stack, fail)
            return player_build.install(self.root, self.source, self.output, self.cache, **kwargs)

    def move_aside(self):
        """What the README asks before building again in the same location."""
        shutil.rmtree(self.output)
        shutil.rmtree(self.build)

    def test_building_again_in_the_same_location_after_a_finished_build(self):
        self.assertEqual(self.install(), self.output)
        self.assertFalse(self.lock.exists(), 'a finished build releases and removes its lock')
        self.move_aside()
        self.assertEqual(self.install(), self.output)
        self.assertFalse(self.lock.exists())

    def test_a_failed_build_removes_its_lock_and_can_be_retried_or_started_over(self):
        with self.assertRaisesRegex(InstallerError, 'synthetic package failure'):
            self.install(fail=True)
        self.assertFalse(self.lock.exists())
        self.assertEqual(self.install(retry=True), self.output)
        self.move_aside()
        with self.assertRaisesRegex(InstallerError, 'synthetic package failure'):
            self.install(fail=True)
        shutil.rmtree(self.build)
        self.assertEqual(self.install(), self.output)

    def test_lock_left_by_an_installer_that_exited_is_taken_over(self):
        # What a killed or crashed build, or an installer before this change, leaves behind.
        records = [json.dumps({'schema_version': 1, 'locking': BACKEND, 'pid': 2147483647}).encode(),
                   json.dumps({'schema_version': 1, 'locking': BACKEND, 'pid': os.getpid(),
                               'started': '2026-11-23T18:20:50Z'}).encode(),
                   b'']
        for record in records:
            with self.subTest(record=record):
                self.lock.write_bytes(record)
                self.assertEqual(self.install(), self.output)
                self.assertFalse(self.lock.exists())
                self.move_aside()

    def test_unrecognised_lock_content_is_kept_for_inspection(self):
        self.lock.write_bytes(b'{"pid": 1234}')
        with self.assertRaisesRegex(InstallerError, 'retained for manual inspection'):
            self.install()
        self.assertEqual(self.lock.read_bytes(), b'{"pid": 1234}')
        self.assertFalse(self.build.exists())

    def test_a_running_installer_in_another_process_still_excludes_a_second_build(self):
        script = ('import sys, time; from pathlib import Path; from mag_installer.pipeline import _exclusive_lock; '
                  'ctx = _exclusive_lock(Path(sys.argv[1]), False, reclaim=True); ctx.__enter__(); '
                  'print("locked", flush=True); sys.stdin.read()')
        child = subprocess.Popen([sys.executable, '-c', script, str(self.lock)], cwd=INSTALLER,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(child.stdout.readline().strip(), 'locked')
            if os.name != 'nt':  # Windows keeps the locked byte unreadable to other processes.
                record = json.loads(self.lock.read_text())
                self.assertEqual((record['pid'], record['locking']), (child.pid, BACKEND))
                self.assertRegex(record['started'], r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$')
            with self.assertRaisesRegex(InstallerError, 'Another installer holds'):
                self.install()
            self.assertFalse(self.build.exists())
            self.assertFalse(self.output.exists())
        finally:
            child.stdin.close(); child.wait(timeout=10); child.stdout.close()
        # Its owner exited without cleaning up (stdin closed ends it mid-"build"): taken over.
        self.assertEqual(self.install(), self.output)

    @unittest.skipIf(os.name == 'nt', 'Windows refuses to remove a file another handle has open')
    def test_a_lock_removed_and_recreated_while_being_taken_is_taken_again(self):
        # Another installer finished (removing the file) and a third created a new one between
        # this installer's open and its kernel lock: it must not run on the removed file.
        self.lock.write_bytes(json.dumps({'schema_version': 1, 'locking': BACKEND, 'pid': 1}).encode())
        from mag_installer import pipeline
        real_fstat = os.fstat
        replaced = []
        def fstat(handle):
            result = real_fstat(handle)
            if not replaced:
                replaced.append(result.st_ino)
                self.lock.unlink()
                self.lock.write_bytes(json.dumps({'schema_version': 1, 'locking': BACKEND, 'pid': 2}).encode())
            return result
        with patch.object(pipeline.os, 'fstat', side_effect=fstat):
            with _exclusive_lock(self.lock, False, reclaim=True):
                self.assertNotEqual(self.lock.stat().st_ino, replaced[0])
                self.assertEqual(json.loads(self.lock.read_text())['pid'], os.getpid())
        self.assertFalse(self.lock.exists())

    @unittest.skipUnless(os.name == 'nt', 'Windows removes the file after closing it')
    def test_windows_leaves_the_file_to_an_installer_that_opened_it_meanwhile(self):
        with _exclusive_lock(self.lock, False, reclaim=True):
            other = os.open(self.lock, os.O_RDWR)  # another installer, about to take the lease
        try:
            self.assertTrue(self.lock.exists(), 'Windows refuses to delete a file open elsewhere')
        finally:
            os.close(other)
        with _exclusive_lock(self.lock, False, reclaim=True):
            pass
        self.assertFalse(self.lock.exists())

    def test_wizard_build_mag_takes_over_a_leftover_lock(self):
        from mag_installer.gui import PlayerBuildController
        self.lock.write_bytes(json.dumps({'schema_version': 1, 'locking': BACKEND, 'pid': 2147483647}).encode())
        with ExitStack() as stack:
            synthetic_build(stack)
            controller = PlayerBuildController(self.root)
            self.assertFalse(controller.has_journal(self.root))
            controller.start(self.source, self.root, self.cache)
            controller.worker.join(30); controller.poll()
        self.assertEqual(controller.state, 'complete', controller.error)
        self.assertTrue((self.output / 'MAG.app').is_dir())
        self.assertFalse(self.lock.exists())


if __name__ == '__main__':
    unittest.main()
