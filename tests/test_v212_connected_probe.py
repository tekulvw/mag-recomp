"""Synthetic input and process tests only; never launch a native game."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools import v212_connected_probe as connected
from tools import v212_boot_probe as offline
from tests import test_v212_boot_probe as fixtures


def readiness():
    resource = dict(namespace='isolated-v212', name='fixture-v212', uid='fixture-uid', target='retail-v2.12')
    return dict(schema_version=1, ready=True, target='retail-v2.12', protocol=1793,
        application_id=20231, integration_branch='codex/retail-v2.12', server_source='a'*40,
        source_pr=440, image_sha256='b'*64, data_sha256='c'*64, pod=resource,
        service=dict(resource), endpoint=dict(host='198.51.100.212', tcp_port=10073), auth_only=True)


SETTINGS = '''MAGClientNetConfig = {
 EnableNetwork = true,
 EnableServerOnClient = false,
 UseTestClient = false,
 UseNPTicket = false,
 MAPSServerAddr = "198.51.100.212",
 MAPSServerTCPPort = 10073,
 MAPSServerUDPPort = 10072,
}
'''


class ConnectedProbeTests(unittest.TestCase):
    def test_character_name_requires_qualified_single_field_literal_table(self):
        character = 'MAGCharacterData = {\n PlayerName = "v212probe",\n}\n'
        text = SETTINGS+character
        self.assertEqual(connected.parse_connected_settings(text, '198.51.100.212')['PlayerName'], 'v212probe')
        loopback = text.replace('198.51.100.212', '127.0.0.1').replace('v212probe', 'Port212Test')
        self.assertEqual(offline.parse_loopback_settings(loopback)['PlayerName'], 'Port212Test')
        self.assertNotIn('PlayerName', connected.parse_connected_settings(SETTINGS, '198.51.100.212'))
        for name in ['Alpha', 'A_b-12', 'A'*15]:
            self.assertEqual(offline.parse_literal_settings(text.replace('v212probe', name))['PlayerName'], name)
        for name in ['ab', 'A'*16, '1abc', 'has space', 'Caf\u00e9', 'A.b']:
            with self.subTest(name=name), self.assertRaises(ValueError):
                offline.parse_literal_settings(text.replace('v212probe', name))
        bad = [SETTINGS+'PlayerName = "v212probe"\n',
               SETTINGS+'MAGClientNetConfig.PlayerName = "v212probe"\n',
               SETTINGS+'MAGCharacterData.PlayerName = "v212probe"\n',
               SETTINGS.replace(' EnableNetwork', ' PlayerName = "v212probe",\n EnableNetwork'),
               text+character,
               SETTINGS+'MAGCharacterData = {\n}\n',
               SETTINGS+'MAGCharacterData = {\n PlayerName = chooseName(),\n}\n',
               SETTINGS+'MAGCharacterData = {\n PlayerName = "v212probe",\n Other = true,\n}\n',
               SETTINGS+'MAGCharacterData = {\n PlayerName = "v212probe",\n PlayerName = "v212probe",\n}\n',
               SETTINGS+'MAGCharacterData = {\n PlayerName = "v212probe",\n']
        for payload in bad:
            with self.subTest(payload=payload[-100:]), self.assertRaises(ValueError):
                offline.parse_literal_settings(payload)
        with self.assertRaises(ValueError):
            connected.parse_connected_settings(text.replace('v212probe', 'AnotherName'), '198.51.100.212')
        with self.assertRaises(ValueError):
            offline.parse_loopback_settings(loopback.replace('Port212Test', 'AnotherName'))

    def test_archive_character_name_must_match_selected_probe_identity(self):
        fixture = self.fixture()
        receipt = json.loads(fixture.settings.read_text())
        for name, accepted in [('v212probe', True), ('AnotherName', False)]:
            text = SETTINGS+'MAGCharacterData = {\n PlayerName = "'+name+'",\n}\n'
            fixture.archive.write_bytes(fixtures.synthetic_archive(text.encode()))
            receipt.update(output_sha256=offline.sha256_file(fixture.archive),
                           after_member_sha256=hashlib.sha256(text.encode()).hexdigest())
            fixture.settings.write_text(json.dumps(receipt))
            if accepted:
                self.assertEqual(connected.validate_settings(fixture.hdd, fixture.settings,
                    '198.51.100.212')['values']['PlayerName'], name)
            else:
                with self.assertRaises(ValueError):
                    connected.validate_settings(fixture.hdd, fixture.settings, '198.51.100.212')

    def test_readiness_selects_non_global_literal_ipv4_and_settings_must_match_it(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td)/'ready.json'
            for host in ['198.51.100.212', '10.23.0.2']:
                data = readiness(); data['endpoint']['host'] = host
                path.write_text(json.dumps(data))
                selected = connected.load_readiness(path)['endpoint']['host']
                self.assertEqual(selected, host)
                connected.parse_connected_settings(SETTINGS.replace('198.51.100.212', host), selected)
            for host in ['8.8.8.8', '127.0.0.1', '169.254.1.2', '224.0.0.1', '0.0.0.0',
                         '255.255.255.255', 'probe.example.invalid', '::1', '010.0.0.2']:
                data = readiness(); data['endpoint']['host'] = host
                path.write_text(json.dumps(data))
                with self.subTest(host=host), self.assertRaises(ValueError):
                    connected.load_readiness(path)
            with self.assertRaises(ValueError):
                connected.parse_connected_settings(SETTINGS, '10.23.0.2')

    def fixture(self):
        fixture = fixtures.InputValidationTests()
        self.addCleanup(fixture.doCleanups)
        fixture.setUp()
        fixture.archive.write_bytes(fixtures.synthetic_archive(SETTINGS.encode()))
        fixture.settings.write_text(json.dumps(dict(archive=offline.ARCHIVE,
            output_sha256=offline.sha256_file(fixture.archive), member=offline.MEMBER,
            after_member_sha256=hashlib.sha256(SETTINGS.encode()).hexdigest(),
            members_verified=2, unrelated_members_unchanged=1, account_name='v212probe',
            use_np_ticket=False, destination=dict(host='probe.example.invalid', ip='198.51.100.212',
                maps_tcp=10073, legacy_maps_udp_field=10072))))
        fixture.ready = fixture.root/'owner-ready.json'
        fixture.ready.write_text(json.dumps(readiness()))
        return fixture

    def test_native_engine_build_inputs_reject_missing_nonlocal_extension(self):
        fixture = self.fixture()
        receipt = dict(fixture.receipt, image_name='mage_g',
                       elf_sha256=fixture.manifest['images']['mage_g']['elf_sha256'])
        fixture.binary.with_suffix('.receipt.json').write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'requires the reviewed nonlocal'):
            offline.validate_build_inputs(fixture.binary, fixture.root/'mage_g.elf',
                                          'mage_g', fixture.disc, fixture.hdd)

    def test_owner_ready_identity_contract_rejects_wrong_or_stable_target(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td)/'ready.json'
            path.write_text(json.dumps(readiness()))
            self.assertEqual(connected.load_readiness(path)['server_source'], 'a'*40)
            wrong = [('schema_version', True), ('ready', False), ('target', 'retail-v1.0'),
                ('protocol', 1792), ('application_id', 20230), ('integration_branch', 'main'),
                ('source_pr', 439), ('server_source', 'short'), ('image_sha256', 'tag:latest'),
                ('data_sha256', ''), ('auth_only', False)]
            for field, value in wrong:
                data = readiness(); data[field] = value
                path.write_text(json.dumps(data))
                with self.subTest(field=field), self.assertRaises(ValueError):
                    connected.load_readiness(path)
            for field in ('pod', 'service'):
                data = readiness(); data[field]['target'] = 'retail-v1.0'
                path.write_text(json.dumps(data))
                with self.subTest(field=field), self.assertRaises(ValueError):
                    connected.load_readiness(path)
            for endpoint in [dict(host='127.0.0.1', tcp_port=10073),
                             dict(host='198.51.100.212', tcp_port=10471),
                             dict(host='198.51.100.212', tcp_port=True),
                             dict(host='198.51.100.212', tcp_port=10073, udp_port=10474)]:
                data = readiness(); data['endpoint'] = endpoint
                path.write_text(json.dumps(data))
                with self.assertRaises(ValueError): connected.load_readiness(path)
            for field in readiness():
                data = readiness(); del data[field]
                path.write_text(json.dumps(data))
                with self.subTest(missing=field), self.assertRaises(ValueError):
                    connected.load_readiness(path)
            path.write_text(json.dumps(readiness())[:-1]+',"ready":false}')
            with self.assertRaises(ValueError): connected.load_readiness(path)

    def test_missing_readiness_rejects_before_network_input_validation_or_run(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            with patch.object(connected, 'validate_build_inputs') as validate, \
                 patch.object(connected, 'run_owned') as run, \
                 patch('socket.socket', side_effect=AssertionError('no preflight')):
                with self.assertRaises((ValueError, FileNotFoundError)):
                    connected.probe(root/'missing-binary', root/'missing-elf', root/'disc', root/'hdd',
                        root/'settings', root/'missing-readiness', root/'output', seconds=1)
                validate.assert_not_called(); run.assert_not_called()
            self.assertFalse((root/'output').exists())

    def test_environment_has_fixed_account_host_sockets_trace_and_no_inherited_controls(self):
        env = connected.clean_environment({'HOME':'/fixture', 'PS3_NETCTL_IPV4_OVERRIDE':'127.0.0.1',
            'PAD_SCRIPT':'input', 'PS3RECOMP_NP_TICKET_FILE':'ticket', 'PSNR_SERVER':'remote',
            'LD_PRELOAD':'inject', 'MAG_LOCAL_ACCOUNT_NAME':'production'},
            Path('/disc'), Path('/hdd'), Path('/output'))
        self.assertEqual(env['MAG_LOCAL_ACCOUNT_NAME'], 'v212probe')
        self.assertEqual(env['MAG_LOCAL_NO_TICKET'], '1')
        self.assertEqual(env['PS3_NET_ONLINE'], '1'); self.assertEqual(env['PS3_NET_TRACE'], '1')
        self.assertEqual(env['MAG_TRACE_NETWORK'], '1')
        for key in ['PS3_NETCTL_IPV4_OVERRIDE', 'PSNR_SERVER', 'PAD_SCRIPT', 'PS3RECOMP_NP_TICKET_FILE', 'LD_PRELOAD']:
            self.assertNotIn(key, env)
        self.assertEqual(env['PS3RECOMP_METAL_HEADLESS'], '1')
        self.assertEqual(env['PAD_NO_KEYBOARD'], '1')
        self.assertNotIn('PS3_ARGV', env)

    def test_connected_settings_bind_exact_archive_bytes_and_keep_legacy_udp(self):
        fixture = self.fixture()
        identity = connected.validate_settings(fixture.hdd, fixture.settings, '198.51.100.212')
        self.assertEqual(identity['values']['MAPSServerUDPPort'], 10072)
        with self.assertRaises(ValueError): offline.validate_settings(fixture.hdd, fixture.settings)
        original_receipt = fixture.settings.read_bytes()
        for field, value in [('account_name', 'another-account'), ('use_np_ticket', True),
                             ('destination', dict(ip='198.51.100.212', maps_tcp=10073, legacy_maps_udp_field=10474))]:
            receipt = json.loads(original_receipt); receipt[field] = value
            fixture.settings.write_text(json.dumps(receipt))
            with self.subTest(field=field), self.assertRaises(ValueError):
                connected.validate_settings(fixture.hdd, fixture.settings, '198.51.100.212')
        fixture.settings.write_bytes(original_receipt)
        for text in [SETTINGS.replace('10072', '10474'), SETTINGS.replace('10073', '10471'),
                     SETTINGS.replace('198.51.100.212', '127.0.0.1'),
                     SETTINGS.replace('UseNPTicket = false', 'UseNPTicket = true'),
                     SETTINGS+'dofile("override.lua")\n']:
            with self.assertRaises(ValueError): connected.parse_connected_settings(text, '198.51.100.212')
        fixture.archive.write_bytes(fixture.archive.read_bytes()+b'tamper')
        with self.assertRaises(ValueError): connected.validate_settings(fixture.hdd, fixture.settings, '198.51.100.212')

    @unittest.skipUnless(os.name == 'posix' and hasattr(os, 'waitid'), 'POSIX ownership required')
    def test_validated_synthetic_child_runs_without_network_preflight_and_is_reaped(self):
        fixture = self.fixture()
        with patch('socket.socket', side_effect=AssertionError('no preflight or reservation')):
            result = connected.probe(fixture.binary, fixture.elf, fixture.disc, fixture.hdd,
                fixture.settings, fixture.ready, fixture.root/'connected-run', seconds=1)
        self.assertEqual(result['exit_code'], 88)
        self.assertTrue(result['reaped']); self.assertTrue(result['group_cleanup_verified'])
        self.assertEqual(result['input_changes_after_run'], [])
        self.assertEqual(result['owner_readiness']['endpoint']['tcp_port'], 10073)
        self.assertEqual(result['account_name'], 'v212probe')
        self.assertEqual(result['image'], 'EBOOT')
        self.assertTrue(result['auth_only'])

    def test_receipt_tamper_after_run_is_reported(self):
        fixture = self.fixture()
        output = fixture.root/'connected-run'
        def fake_run(command, env, path, seconds, identity):
            path.mkdir(); fixture.ready.write_text('{}')
            return dict(identity, errors=[])
        with patch.object(connected, 'run_owned', side_effect=fake_run):
            result = connected.probe(fixture.binary, fixture.elf, fixture.disc, fixture.hdd,
                fixture.settings, fixture.ready, output, seconds=1)
        self.assertIn(str(fixture.ready), result['input_changes_after_run'])
        self.assertTrue(result['errors'])

    def test_readiness_symlinks_and_oversized_documents_are_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td)/'ready.json'; path.write_text(json.dumps(readiness()))
            link = Path(td)/'linked.json'; link.symlink_to(path)
            with self.assertRaises(ValueError): connected.load_readiness(link)
            path.write_text(' ' * 65537)
            with self.assertRaises(ValueError): connected.load_readiness(path)
