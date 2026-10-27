import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from tests.test_psarc import archive
from tools import network_profile, psarc


class NetworkProfileTests(unittest.TestCase):
    def test_private_overlay_and_original_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); source=root/'source.psarc'; source.write_bytes(archive())
            vfs=root/'vfs'; target=vfs/network_profile.ARCHIVE_PATH
            target.parent.mkdir(parents=True); target.write_bytes(b'original encrypted fixture')
            profile={'format':1,'elf_sha256':network_profile.RETAIL_ELF_SHA256,
                     'archive':str(source),'archive_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
                     'source_sdat_sha256':hashlib.sha256(target.read_bytes()).hexdigest(),
                     'member':'/settings.lua','member_sha256':hashlib.sha256(b'UseNPTicket = true\n'*30).hexdigest(),
                     'maps_host':'127.0.0.1','maps_tcp_port':36475,'maps_udp_port':36476,
                     'host_sockets':True,'restore_ticket_setting':True,'local_account_name':'Player'}
            path=root/'profile.json';path.write_text(json.dumps(profile))
            result=network_profile.prepare_overlay(vfs,path,expected_member='/settings.lua')
            self.assertEqual(source.read_bytes(),archive())
            _,_,extract,*_=psarc.parse(target.read_bytes())
            self.assertIn(b'UseNPTicket = false',extract(1))
            self.assertIn(b'127.0.0.1',extract(1))
            self.assertEqual(extract(2),bytes(range(200)))
            self.assertTrue(result['restore_ticket_setting'])
            self.assertEqual(result['local_account_name'],'Player')
            self.assertEqual(result['archive_change']['unrelated_members_unchanged'],2)
            retail=dict(profile,local_account_name='PlayerRetail')
            path.write_text(json.dumps(retail))
            target.write_bytes(b'original encrypted fixture')
            result=network_profile.prepare_overlay(vfs,path,expected_member='/settings.lua')
            self.assertEqual(result['local_account_name'],'PlayerRetail')
            ticket_profile=dict(retail,local_np_ticket=True)
            path.write_text(json.dumps(ticket_profile))
            target.write_bytes(b'original encrypted fixture')
            result=network_profile.prepare_overlay(vfs,path,expected_member='/settings.lua')
            self.assertTrue(result['local_np_ticket'])
            self.assertEqual(result['np_status_policy'],'local development ticket; no PSN authentication')
            saved=target.read_bytes()
            for invalid_flag in ('true',1,None):
                path.write_text(json.dumps(dict(retail,local_np_ticket=invalid_flag)))
                with self.assertRaisesRegex(ValueError,'Local NP ticket'):
                    network_profile.prepare_overlay(vfs,path,expected_member='/settings.lua')
                self.assertEqual(target.read_bytes(),saved)
            path.write_text(json.dumps(dict(retail,local_np_ticket=True,local_account_name=None)))
            with self.assertRaisesRegex(ValueError,'Local NP ticket'):
                network_profile.prepare_overlay(vfs,path,expected_member='/settings.lua')
            for account in ('', 'someone', True):
                invalid=dict(profile,local_account_name=account)
                path.write_text(json.dumps(invalid))
                with self.assertRaisesRegex(ValueError,'Local development identity'):
                    network_profile.prepare_overlay(vfs,path,expected_member='/settings.lua')
                self.assertEqual(target.read_bytes(),saved)
            path.write_text(json.dumps(profile))
            target.write_bytes(b'wrong')
            with self.assertRaises(ValueError):network_profile.prepare_overlay(vfs,path,expected_member='/settings.lua')

    def test_reject_nonlocal_and_wrong_build_before_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'profile.json'
            for host,elf in [('8.8.8.8',network_profile.RETAIL_ELF_SHA256),('127.0.0.1','wrong')]:
                path.write_text(json.dumps({'format':1,'elf_sha256':elf,'maps_host':host}))
                with self.assertRaises(ValueError):network_profile.prepare_overlay(Path(tmp)/'vfs',path)
