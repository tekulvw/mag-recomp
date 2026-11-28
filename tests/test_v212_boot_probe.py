"""Synthetic process fixtures only; never launch the native game."""
import json
import hashlib
import struct
import zlib
from dataclasses import replace
from unittest.mock import patch
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import unittest

from tools import v212_boot_probe as probe


SETTINGS = '''MAGClientNetConfig = {
  EnableNetwork = true,
  EnableServerOnClient = false,
  UseTestClient = false,
  UseNPTicket = true,
  MAPSServerAddr = "example.invalid",
  MAPSServerTCPPort = 10073,
  MAPSServerUDPPort = 10072,
}
MAGClientNetConfig = MAGClientNetConfig or {}
MAGClientNetConfig.EnableNetwork = true
MAGClientNetConfig.EnableServerOnClient = false
MAGClientNetConfig.UseTestClient = false
MAGClientNetConfig.UseNPTicket = false
MAGClientNetConfig.MAPSServerAddr = "127.0.0.1"
MAGClientNetConfig.MAPSServerTCPPort = 51648
MAGClientNetConfig.MAPSServerUDPPort = 63326
'''


class BootProbeTests(unittest.TestCase):
    def test_movie_environment_is_explicit_engine_only_and_enables_three_flags_together(self):
        switches = ('PS3RECOMP_SAIL_TIMED_THX', 'PS3RECOMP_SAIL_ATRAC_AUDIO', 'PS3RECOMP_GCM_CHECKED_REF')
        inherited = {key: '1' for key in switches}
        args = (inherited, Path('/disc'), Path('/hdd'), Path('/output'))
        default = probe.clean_environment(*args, image='mage_g')
        self.assertTrue(all(key not in default for key in switches))
        movies = probe.clean_environment(*args, image='mage_g', movies=True)
        self.assertTrue(all(movies[key] == '1' for key in switches))
        self.assertEqual(movies['PS3RECOMP_METAL_HEADLESS'], '1')
        self.assertEqual(movies['PAD_NO_KEYBOARD'], '1')
        for image, selected in [('EBOOT', True), ('mage_g', '1')]:
            with self.assertRaises(ValueError):
                probe.clean_environment(*args, image=image, movies=selected)

    def test_movie_probe_rejects_wrong_image_or_unqualified_receipt_before_run(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            files = [root/name for name in ('runner', 'engine.elf', 'settings.json')]
            for path in files: path.write_text('synthetic')
            disc, hdd = root/'disc', root/'hdd'; disc.mkdir(); hdd.mkdir()
            for image, available in [('EBOOT', True), ('mage_g', False), ('mage_g', None), ('mage_g', 'true')]:
                identity = dict(movie_playback_available=available, settings={'values': {}})
                with patch.object(probe, 'validate_inputs', return_value=identity), \
                     patch.object(probe, 'ClosedLoopback') as ports, patch.object(probe, 'run_owned') as run:
                    with self.subTest(image=image, available=available), self.assertRaises(ValueError):
                        probe.probe(files[0], files[1], image, disc, hdd, files[2], root/'output', movies=True)
                    ports.assert_not_called(); run.assert_not_called()
            self.assertFalse((root/'output').exists())

    def test_environment_scrubs_inherited_game_input_network_and_loader_controls(self):
        source = {'PATH': '/unexpected', 'HOME': '/synthetic-home', 'MAG_LOCAL_ACCOUNT_NAME': 'wrong',
                  'PS3_NET_ONLINE': '1', 'PAD_SCRIPT': '0:CROSS', 'PAD_FILE': '/input',
                  'PS3RECOMP_NP_TICKET_FILE': '/ticket', 'DYLD_INSERT_LIBRARIES': '/inject',
                  'LD_PRELOAD': '/inject', 'MAG_NATIVE_CONSUMER': '1'}
        env = probe.clean_environment(source, Path('/disc'), Path('/hdd'), Path('/output'))
        for key in ['PS3_NET_ONLINE', 'PAD_SCRIPT', 'PAD_FILE', 'PS3RECOMP_NP_TICKET_FILE',
                    'DYLD_INSERT_LIBRARIES', 'LD_PRELOAD']:
            self.assertNotIn(key, env)
        self.assertEqual(env['MAG_LOCAL_ACCOUNT_NAME'], 'Port212Test')
        self.assertEqual(env['MAG_LOCAL_NO_TICKET'], '1')
        self.assertEqual(env['PS3RECOMP_METAL_HEADLESS'], '1')
        self.assertEqual(env['PS3RECOMP_METAL_FRAME_DUMP_FIRST_NONBLACK'], '1')
        latest = probe.clean_environment(source, Path('/disc'), Path('/hdd'), Path('/output'), latest_frame=True)
        self.assertEqual(latest['PS3RECOMP_METAL_FRAME_DUMP_FIRST_NONBLACK'], '0')
        self.assertEqual(latest['PS3RECOMP_METAL_FRAME_DUMP'], '/output/frame.ppm')
        self.assertEqual(env['PS3RECOMP_METAL_FRAME_DUMP'], '/output/frame.ppm')
        self.assertEqual(env['PS3RECOMP_METAL_FRAME_DUMP_FIRST_NONBLACK'], '1')
        self.assertEqual(env['SDL_AUDIODRIVER'], 'dummy')
        self.assertEqual(env['PAD_NO_KEYBOARD'], '1')
        self.assertEqual(env['PS3_BOOT_PATH'], '/dev_hdd0/game/BCUS98110/USRDIR/EBOOT.BIN')
        self.assertEqual(env['PS3_HLE_UNRESOLVED'], 'halt')
        self.assertEqual(env['PS3_SYSCALL_UNRESOLVED'], 'halt')
        engine = probe.clean_environment(source, Path('/disc'), Path('/hdd'), Path('/output'), image='mage_g')
        self.assertEqual(engine['PS3_BOOT_PATH'], '/dev_hdd0/game/BCUS98110/USRDIR/bin/mage_g.self')
        online = probe.clean_environment(source, Path('/disc'), Path('/hdd'), Path('/output'), loopback_sockets=True)
        self.assertEqual(online['PS3_NET_ONLINE'], '1')

    def test_settings_must_end_in_literal_loopback_no_ticket_configuration(self):
        values = probe.parse_loopback_settings(SETTINGS)
        self.assertEqual(values['MAPSServerAddr'], '127.0.0.1')
        self.assertEqual(values['MAPSServerTCPPort'], 51648)
        for bad in [SETTINGS + 'MAGClientNetConfig.MAPSServerAddr = "192.0.2.5"\n',
                    SETTINGS + 'dofile("override.lua")\n',
                    SETTINGS + 'MAGClientNetConfig.UseNPTicket = true\n',
                    SETTINGS.replace('= 63326', '= 0')]:
            with self.subTest(bad=bad[-70:]), self.assertRaises(ValueError):
                probe.parse_loopback_settings(bad)

    def test_static_debug_tag_before_original_shaped_network_table_is_allowed(self):
        original_shape = '''MAGDebugSettings = {
  dbgTag = "synthetic-debug-tag",
}
MAGClientNetConfig = {
  EnableNetwork = true,
  EnableServerOnClient = false,
  UseTestClient = false,
  UseNPTicket = false,
  MAPSServerAddr = "127.0.0.1",
  MAPSServerTCPPort = 51648,
  MAPSServerUDPPort = 63326,
}
'''
        values = probe.parse_loopback_settings(original_shape)
        self.assertEqual(values['MAPSServerAddr'], '127.0.0.1')
        for prefix in ['MAGDebugSettings = {\n dbgTag = makeTag()\n}\n',
                       'MAGDebugSettings = {\n dbgTag = "safe",\n another = "field",\n}\n']:
            with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                probe.parse_loopback_settings(prefix + SETTINGS)

    def test_port_reservations_prevent_another_loopback_listener(self):
        with probe.ClosedLoopback(0, 0) as held:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as other:
                with self.assertRaises(OSError):
                    other.bind(('127.0.0.1', held.tcp_port))
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as client:
                client.settimeout(0.2)
                self.assertNotEqual(client.connect_ex(('127.0.0.1', held.tcp_port)), 0)
            self.assertTrue(held.proof()['tcp_bound_without_listen'])

    @unittest.skipUnless(os.name == 'posix' and hasattr(os, 'waitid'), 'POSIX waitid required')
    def test_natural_exit_retains_owned_identity_output_and_status(self):
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / 'probe'
            result = probe.run_owned([sys.executable, '-c', 'print("synthetic boot"); raise SystemExit(88)'],
                                     {}, output, seconds=1, identity={'binary_sha256': 'synthetic'})
            self.assertEqual(result['exit_code'], 88)
            self.assertFalse(result['timed_out'])
            self.assertTrue(result['reaped'])
            self.assertEqual(result['pid'], result['owned_process_group'])
            self.assertNotEqual(result['owned_process_group'], os.getpgrp())
            self.assertIn('synthetic boot', (output / 'console.log').read_text())
            self.assertEqual(json.loads((output / 'result.json').read_text())['exit_code'], 88)
            with self.assertRaises(ValueError):
                probe.run_owned([sys.executable, '-c', 'raise SystemExit(0)'], {}, output, seconds=1)

    @unittest.skipUnless(os.name == 'posix' and hasattr(os, 'waitid'), 'POSIX waitid required')
    def test_timeout_escalates_only_owned_group_and_reaps_stubborn_child(self):
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / 'probe'
            code = 'import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); print("ready",flush=True); time.sleep(30)'
            result = probe.run_owned([sys.executable, '-c', code], {}, output, seconds=1)
            self.assertTrue(result['timed_out'])
            self.assertTrue(result['kill_sent'])
            self.assertEqual(result['exit_code'], -signal.SIGKILL)
            self.assertTrue(result['reaped'])
            with self.assertRaises(ProcessLookupError):
                os.kill(result['pid'], 0)

    @unittest.skipUnless(os.name == 'posix' and hasattr(os, 'waitid'), 'POSIX waitid required')
    def test_log_budget_stops_owned_process_and_caps_private_output(self):
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / 'probe'
            result = probe.run_owned([sys.executable, '-c', 'import os;\nwhile True: os.write(1,b"x"*4096)'],
                                     {}, output, seconds=1, log_limit=1024)
            self.assertTrue(result['log_limit_reached'])
            self.assertEqual((output / 'console.log').stat().st_size, 1024)
            self.assertTrue(result['reaped'])

    @unittest.skipUnless(os.name == 'posix' and hasattr(os, 'fork'), 'POSIX fork required')
    def test_natural_leader_exit_still_cleans_stubborn_owned_descendant(self):
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / 'probe'
            code = """import os,signal,time
child=os.fork()
if child==0:
 signal.signal(signal.SIGTERM,signal.SIG_IGN)
 print('descendant='+str(os.getpid()),flush=True)
 time.sleep(30)
else:
 time.sleep(0.1)
 raise SystemExit(0)
"""
            result = probe.run_owned([sys.executable, '-c', code], {}, output, seconds=1)
            self.assertEqual(result['exit_code'], 0)
            self.assertFalse(result['timed_out'])
            self.assertTrue(result['kill_sent'])
            self.assertTrue(result['group_cleanup_verified'])
            self.assertEqual(result['live_group_after_cleanup'], [])

    @unittest.skipUnless(os.name == 'posix' and hasattr(os, 'waitid'), 'POSIX waitid required')
    def test_signal_denial_is_reported_and_direct_child_is_still_reaped(self):
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / 'probe'
            with patch('tools.v212_boot_probe.os.killpg', side_effect=PermissionError('synthetic signal denial')):
                result = probe.run_owned([sys.executable, '-c', 'import time; time.sleep(30)'], {}, output, seconds=1)
            self.assertTrue(result['reaped'])
            self.assertFalse(result['group_cleanup_verified'])
            self.assertTrue(any('signal denial' in error for error in result['errors']))

    def test_invalid_budget_rejects_before_process_or_output(self):
        with tempfile.TemporaryDirectory() as td:
            for seconds in [0, 61, True, float('nan')]:
                output = Path(td) / str(seconds)
                with self.subTest(seconds=seconds), self.assertRaises(ValueError):
                    probe.run_owned([sys.executable, '-c', 'raise SystemExit(0)'], {}, output, seconds=seconds)
                self.assertFalse(output.exists())


def synthetic_archive(payload):
    chunks = [(probe.MEMBER + '\n').encode(), payload]
    stored = [zlib.compress(raw) for raw in chunks]
    toc = 32 + 30 * 2 + 2 * 2
    data = bytearray(struct.pack('>4sI4sIIIII', b'PSAR', 0x10003, b'zlib', toc, 30, 2, 65536, 2))
    offset = toc
    for index, (raw, encoded) in enumerate(zip(chunks, stored)):
        data += bytes(16) + index.to_bytes(4, 'big') + len(raw).to_bytes(5, 'big') + offset.to_bytes(5, 'big')
        offset += len(encoded)
    data += b''.join(len(raw).to_bytes(2, 'big') for raw in stored) + b''.join(stored)
    return bytes(data)


class InputValidationTests(unittest.TestCase):
    def setUp(self):
        from tools import targets
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.disc, self.hdd = self.root / 'disc', self.root / 'hdd'
        (self.disc / 'PS3_GAME').mkdir(parents=True)
        (self.hdd / 'game/BCUS98110/USRDIR/bin').mkdir(parents=True)
        (self.hdd / 'game/BCUS98110/PARAM.SFO').write_text('synthetic metadata')
        (self.root / 'profiles').mkdir()
        self.binary = self.root / 'runner'
        self.binary.write_text('#!' + sys.executable + '\nprint("synthetic fixture only")\nraise SystemExit(88)\n')
        self.binary.chmod(0o700)
        images, identities = {}, {}
        for name, relative in [('EBOOT', 'EBOOT.BIN'), ('mage_g', 'bin/mage_g.self')]:
            elf = self.root / (name + '.elf')
            elf.write_text('synthetic ' + name)
            original = self.hdd / 'game/BCUS98110/USRDIR' / relative
            original.write_text('synthetic original ' + name)
            pair = (probe.sha256_file(original), probe.sha256_file(elf))
            identities[relative] = pair
            images[name] = dict(elf=str(elf), elf_sha256=pair[1], source_sha256=pair[0])
        self.elf = self.root / 'EBOOT.elf'
        self.manifest = dict(target='retail-v2.12', title_id='BCUS98110', app_version='02.12', images=images)
        (self.root / 'profiles/retail-v2.12.json').write_text(json.dumps(self.manifest))
        self.sdk = self.root / 'sdk'
        self.sdk.mkdir()
        self.git('init', '-q')
        (self.sdk / 'runtime.c').write_text('synthetic baseline')
        self.git('add', 'runtime.c')
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'baseline')
        self.base = self.git('rev-parse', 'HEAD').strip()
        framework = dict(allowed_runtime_revisions=[self.base], strict_runtime_revisions=[self.base],
                         strict_hle_runtime_revisions=[self.base], ps3recomp={
                             'runtime': {'commit': self.base}, 'lifter-ppu-v212': {'commit': 'lifter'}})
        self.framework_path = self.root / 'profiles/framework.json'
        self.framework_path.write_text(json.dumps(framework))
        self.receipt = dict(target='retail-v2.12', experimental=True, not_runtime_verified=True, image_name='EBOOT',
                            binary_sha256=probe.sha256_file(self.binary), elf_sha256=images['EBOOT']['elf_sha256'],
                            sdk_revision=self.base, sdk_path=str(self.sdk), lift_provenance=dict(target='retail-v2.12',
                                lifter_role='lifter-ppu-v212', sdk_revision='lifter'))
        self.binary.with_suffix('.receipt.json').write_text(json.dumps(self.receipt))
        # Keep synthetic endpoints separate from the real bootstrap's fixed
        # ports even when this suite runs alongside an owned native probe.
        ports = []
        for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
            while True:
                with socket.socket(socket.AF_INET, kind) as chosen:
                    chosen.bind(('127.0.0.1', 0))
                    port = chosen.getsockname()[1]
                if port not in (51648, 63326):
                    ports.append(port)
                    break
        settings_text = SETTINGS.replace('= 51648', '= ' + str(ports[0])).replace('= 63326', '= ' + str(ports[1]))
        self.archive = self.hdd / probe.ARCHIVE
        self.archive.write_bytes(synthetic_archive(settings_text.encode()))
        self.settings = self.root / 'offline-settings.json'
        self.settings.write_text(json.dumps(dict(archive='game/BCUS98110/USRDIR/tpps.psarc.sdat', output_sha256=probe.sha256_file(self.archive), member=probe.MEMBER,
            after_member_sha256=hashlib.sha256(settings_text.encode()).hexdigest(), members_verified=2,
            unrelated_members_unchanged=1, loopback_tcp=ports[0], loopback_udp=ports[1])))
        selected = replace(targets.get_target('retail-v2.12'), executables=identities)
        self.addCleanup(patch.stopall)
        patch('tools.targets._TARGETS', {**targets._TARGETS, 'retail-v2.12': selected}).start()
        patch('tools.prepare_inputs.WORKSPACE', self.root).start()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.sdk), *args], text=True)

    def test_source_bound_candidate_accepts_then_rejects_changed_manifest_or_dirty_sdk(self):
        from tools.provenance import runtime_identity
        (self.sdk / 'runtime.c').write_text('synthetic candidate')
        self.git('add', 'runtime.c')
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'candidate')
        revision = self.git('rev-parse', 'HEAD').strip()
        manifest = self.root / 'candidate.json'
        manifest.write_text(json.dumps(dict(schema_version=1, purpose='synthetic candidate',
            sdk_revision=revision, sdk_tree=self.git('rev-parse', 'HEAD^{tree}').strip(),
            framework_sha256=probe.sha256_file(self.framework_path), target='retail-v2.12',
            base_runtime_revision=self.base, integration_branch='codex/retail-v2.12',
            source_pr='https://github.com/tekulvw/ps3recomp/pull/123')))
        framework = json.loads(self.framework_path.read_text())
        identity = runtime_identity(self.sdk, framework, self.framework_path, manifest, 'retail-v2.12')
        self.receipt.update(identity)
        self.binary.with_suffix('.receipt.json').write_text(json.dumps(self.receipt))
        self.validate()
        original = manifest.read_bytes()
        manifest.write_bytes(original + b' ')
        with self.assertRaises(ValueError):
            self.validate()
        manifest.write_bytes(original)
        (self.sdk / 'untracked.c').write_text('unreviewed')
        with self.assertRaises(ValueError):
            self.validate()
        (self.sdk / 'untracked.c').unlink()
        framework['strict_hle_runtime_revisions'] = []
        self.framework_path.write_text(json.dumps(framework))
        with self.assertRaises(ValueError):
            self.validate()

    def validate(self):
        return probe.validate_inputs(self.binary, self.elf, 'EBOOT', self.disc, self.hdd, self.settings)

    def test_exact_synthetic_receipt_pair_archive_and_member_validate(self):
        result = self.validate()
        self.assertEqual(result['settings']['values']['MAPSServerAddr'], '127.0.0.1')
        self.assertEqual(result['binary_sha256'], probe.sha256_file(self.binary))
        self.assertTrue(result['not_runtime_verified'])

    def test_movie_capability_uses_exact_boolean_build_receipt_field(self):
        for value in [None, False, 'true', 1, True]:
            self.receipt['movie_playback'] = value
            self.binary.with_suffix('.receipt.json').write_text(json.dumps(self.receipt))
            self.assertIs(self.validate()['movie_playback_available'], value is True)

    def test_qualified_synthetic_engine_movie_probe_records_mode_and_keeps_owned_cleanup(self):
        self.binary.write_text('#!'+sys.executable+'\nimport os\n'
            'assert all(os.environ.get(k)=="1" for k in '
            '("PS3RECOMP_SAIL_TIMED_THX","PS3RECOMP_SAIL_ATRAC_AUDIO","PS3RECOMP_GCM_CHECKED_REF"))\n'
            'assert os.environ["PAD_NO_KEYBOARD"]=="1"\n'
            'print("synthetic movie mode only")\nraise SystemExit(88)\n')
        self.receipt.update(image_name='mage_g', movie_playback=True,
            binary_sha256=probe.sha256_file(self.binary),
            elf_sha256=self.manifest['images']['mage_g']['elf_sha256'])
        self.binary.with_suffix('.receipt.json').write_text(json.dumps(self.receipt))
        # Nonlocal extension identity is covered by its own source-bound tests;
        # this synthetic executable contains no guest unwind implementation.
        with patch.object(probe, 'validate_v212_lift_extensions') as extensions:
            result = probe.probe(self.binary, self.root/'mage_g.elf', 'mage_g', self.disc,
                self.hdd, self.settings, self.root/'movie-probe', seconds=1, movies=True)
        self.assertTrue(extensions.call_args.kwargs['require_nonlocal'])
        self.assertTrue(result['movies']); self.assertTrue(result['movie_playback_available'])
        self.assertEqual(result['exit_code'], 88)
        self.assertTrue(result['reaped']); self.assertTrue(result['group_cleanup_verified'])
        self.assertTrue(result['reservations_released'])

    def test_lower_priority_game_archive_proof_cannot_authorize_probe(self):
        lower = self.hdd / 'game/BCUS98110/USRDIR/game.psarc.sdat'
        lower.write_bytes(self.archive.read_bytes())
        receipt = json.loads(self.settings.read_text())
        receipt['archive'] = 'game/BCUS98110/USRDIR/game.psarc.sdat'
        self.settings.write_text(json.dumps(receipt))
        with self.assertRaises(ValueError):
            self.validate()

    def test_wrong_receipt_changed_elf_or_archive_rejects_before_spawn(self):
        self.receipt['target'] = 'retail-v1.0'
        self.binary.with_suffix('.receipt.json').write_text(json.dumps(self.receipt))
        with self.assertRaises(ValueError):
            self.validate()
        self.receipt['target'] = 'retail-v2.12'
        self.binary.with_suffix('.receipt.json').write_text(json.dumps(self.receipt))
        original = self.elf.read_bytes()
        self.elf.write_bytes(b'changed')
        with self.assertRaises(ValueError):
            self.validate()
        self.elf.write_bytes(original)
        self.archive.write_bytes(b'changed')
        with self.assertRaises(ValueError):
            self.validate()

    def test_complete_probe_launches_only_synthetic_fixture_and_releases_ports(self):
        endpoints = json.loads(self.settings.read_text())
        self.assertNotEqual(endpoints['loopback_tcp'], 51648)
        self.assertNotEqual(endpoints['loopback_udp'], 63326)
        output = self.root / 'owned-probe'
        result = probe.probe(self.binary, self.elf, 'EBOOT', self.disc, self.hdd, self.settings,
                             output=output, seconds=1, loopback_sockets=False)
        self.assertEqual(result['exit_code'], 88)
        self.assertTrue(result['reaped'])
        self.assertTrue(result['reservations_released'])
        self.assertFalse(result['loopback_sockets'])
        self.assertIn('synthetic fixture only', (output / 'console.log').read_text())
        self.assertEqual(result['input_changes_after_run'], [])
        for kind, field in [(socket.SOCK_STREAM, 'loopback_tcp'), (socket.SOCK_DGRAM, 'loopback_udp')]:
            with socket.socket(socket.AF_INET, kind) as reopened:
                reopened.bind(('127.0.0.1', endpoints[field]))


if __name__ == '__main__':
    unittest.main()
