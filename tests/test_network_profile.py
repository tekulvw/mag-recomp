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


# ---- The portal block: a ticket-enforcing server's account portal (synthetic tokens and tickets) ----
import base64
import shutil
import http.server
import os
import threading

TOKEN='mgd_'+'Profile0123456789-_abcdefghijklmnopqrstuvw'


def ticket_bytes(audience,name):
    """A ticket in the version 1 layout with a dummy signature (docs/portal-ticket.md)."""
    return (b'MAGT\x01\x01'+bytes(8)+(100).to_bytes(8,'big')+(7).to_bytes(4,'big')+b'n'*16+
            bytes([len(audience)])+audience.encode()+bytes([len(name)])+name.encode()+b's'*64)


class FakePortal:
    """A local HTTP portal: /api/v1/ticket for the audiences it serves; `requests` records each call."""
    def __init__(self,test,audiences=('test.example.org',),name='Svc_Bot'):
        self.audiences=set(audiences);self.name=name;self.requests=[];self.revoked=False;self.reply_audience=None;self.redirect=False
        portal=self
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def reply(self,status,value):
                body=json.dumps(value).encode();self.send_response(status)
                self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
            def do_POST(self):
                body=json.loads(self.rfile.read(int(self.headers['Content-Length'])) or b'{}')
                portal.requests.append((self.path,self.headers.get('Authorization'),self.headers.get('Content-Type'),body))
                if portal.redirect:
                    self.send_response(307);self.send_header('Location','/elsewhere');self.send_header('Content-Length','0');self.end_headers();return
                if self.path!='/api/v1/ticket':return self.reply(404,{'error':'not_found'})
                if portal.revoked or self.headers.get('Authorization')!='Bearer '+TOKEN:return self.reply(401,{'error':'invalid_token'})
                audience=body.get('audience','play.example.net')
                if audience not in portal.audiences:return self.reply(400,{'error':'unknown_audience'})
                return self.reply(200,dict(ticket=base64.b64encode(ticket_bytes(audience,portal.name)).decode(),expires_at=1800000600,
                                           account_id=7,player_name=portal.name,audience=portal.reply_audience or audience))
        self.server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
        threading.Thread(target=self.server.serve_forever,daemon=True).start()
        test.addCleanup(self.server.server_close);test.addCleanup(self.server.shutdown)
        self.url=f'http://127.0.0.1:{self.server.server_address[1]}'


def token_file(root,text=TOKEN+'\n'):
    path=Path(root)/'service.token';path.write_text(text);path.chmod(0o600)
    return path


class PortalProfileTests(unittest.TestCase):
    def fixture(self,root,**changes):
        root=Path(root);source=root/'source.psarc';source.write_bytes(archive())
        vfs=root/'vfs';target=vfs/network_profile.ARCHIVE_PATH
        target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(b'original encrypted fixture')
        profile={'format':1,'elf_sha256':network_profile.RETAIL_ELF_SHA256,
                 'archive':str(source),'archive_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
                 'source_sdat_sha256':hashlib.sha256(target.read_bytes()).hexdigest(),
                 'member':'/settings.lua','member_sha256':hashlib.sha256(b'UseNPTicket = true\n'*30).hexdigest(),
                 'maps_host':'127.0.0.1','maps_tcp_port':36475,'maps_udp_port':36476,
                 'host_sockets':True,'restore_ticket_setting':False,
                 'portal':{'url':'https://Portal.Example.org/','token_file':'service.token','audience':'Test.Example.org'}}
        profile.update(changes)
        path=root/'profile.json';path.write_text(json.dumps(profile))
        return vfs,path,target

    def test_portal_profile_is_the_ticket_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            vfs,path,target=self.fixture(tmp)
            result=network_profile.prepare_overlay(vfs,path,expected_member='/settings.lua')
            self.assertEqual(result['portal'],{'url':'https://portal.example.org','token_file':str(Path(tmp)/'service.token'),
                                               'audience':'test.example.org'})
            self.assertEqual(result['np_status_policy'],'portal ticket for test.example.org; no PSN authentication')
            self.assertFalse(result['restore_ticket_setting']);self.assertIsNone(result['local_account_name'])
            # Without an explicit audience, the ticket is for the profile's server host.
            vfs,path,target=self.fixture(tmp,portal={'url':'http://localhost:8080','token_file':'/abs/service.token'})
            self.assertEqual(network_profile.prepare_overlay(vfs,path,expected_member='/settings.lua')['portal'],
                             {'url':'http://localhost:8080','token_file':'/abs/service.token','audience':'127.0.0.1'})

    def test_portal_profile_reaches_lan_servers_but_never_public_ones(self):
        with tempfile.TemporaryDirectory() as tmp:
            vfs,path,target=self.fixture(tmp,maps_host='192.168.40.20')
            network_profile.prepare_overlay(vfs,path,expected_member="/settings.lua")
            _,_,extract,*_=psarc.parse(target.read_bytes())
            self.assertIn(b'MAPSServerAddr = "192.168.40.20"',extract(1))
            self.assertIn(b'UseNPTicket = false',extract(1))  # the game's own launcher init turns it on again
            vfs,path,target=self.fixture(tmp,maps_host='localhost',portal={'url':'https://p.example.org','token_file':'t'})
            result=network_profile.prepare_overlay(vfs,path,expected_member='/settings.lua')
            self.assertEqual(result['portal']['audience'],'localhost')
            for host in ('8.8.8.8','0.0.0.0','https://x.org','bad host',None):
                vfs,path,target=self.fixture(tmp,maps_host=host)
                with self.assertRaises(ValueError):network_profile.prepare_overlay(vfs,path,expected_member='/settings.lua')
                self.assertEqual(target.read_bytes(),b'original encrypted fixture')
            # Without a portal block the profile stays loopback-only.
            vfs,path,target=self.fixture(tmp,maps_host='192.168.40.20',portal=None)
            profile=json.loads(path.read_text());del profile['portal'];path.write_text(json.dumps(profile))
            with self.assertRaisesRegex(ValueError,'loopback'):network_profile.prepare_overlay(vfs,path,expected_member='/settings.lua')

    def test_portal_profile_rejects_mixed_identities_and_bad_blocks_before_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            invalid=[dict(restore_ticket_setting=True),dict(host_sockets=False),dict(local_account_name='Player',restore_ticket_setting=True),
                     dict(local_np_ticket=True),
                     dict(portal={'url':'http://portal.example.org','token_file':'t'}),
                     dict(portal={'url':'https://portal.example.org/?x=1','token_file':'t'}),
                     dict(portal={'url':'https://user@portal.example.org','token_file':'t'}),
                     dict(portal={'url':'https://portal.example.org:','token_file':'t'}),
                     dict(portal={'url':'https://portal.example.org','token_file':''}),
                     dict(portal={'url':'https://portal.example.org'}),
                     dict(portal={'url':'https://portal.example.org','token_file':'t','token':TOKEN}),
                     dict(portal={'url':'https://portal.example.org','token_file':'t','audience':'x'*64}),
                     dict(portal={'url':'https://portal.example.org','token_file':'t','audience':'bad host'}),
                     dict(portal='https://portal.example.org')]
            for change in invalid:
                with self.subTest(change=change):
                    vfs,path,target=self.fixture(tmp,**change)
                    with self.assertRaises(ValueError):network_profile.prepare_overlay(vfs,path,expected_member='/settings.lua')
                    self.assertEqual(target.read_bytes(),b'original encrypted fixture')


class PortalTicketTests(unittest.TestCase):
    def portal_block(self,root,portal,audience='test.example.org'):
        return {'url':portal.url,'token_file':str(token_file(root)),'audience':audience}

    def test_fetch_writes_a_private_ticket_for_the_audience(self):
        portal=FakePortal(self)
        with tempfile.TemporaryDirectory() as tmp:
            destination=Path(tmp)/'run'/'portal-ticket.bin';destination.parent.mkdir()
            result=network_profile.fetch_portal_ticket(self.portal_block(tmp,portal),destination)
            self.assertEqual(result,{'file':str(destination),'player_name':'Svc_Bot','audience':'test.example.org',
                                     'expires_at':1800000600,'account_id':7})
            self.assertEqual(destination.read_bytes(),ticket_bytes('test.example.org','Svc_Bot'))
            if os.name=='posix':self.assertEqual(destination.stat().st_mode&0o777,0o600)
            self.assertEqual(portal.requests,[('/api/v1/ticket','Bearer '+TOKEN,'application/json',{'audience':'test.example.org'})])
            self.assertEqual(sorted(p.name for p in destination.parent.iterdir()),['portal-ticket.bin'])
            self.assertNotIn(TOKEN,json.dumps(result))

    def test_refusals_say_why_without_the_token(self):
        portal=FakePortal(self)
        with tempfile.TemporaryDirectory() as tmp:
            destination=Path(tmp)/'portal-ticket.bin'
            def refused(pattern,**block):
                with self.assertRaisesRegex(network_profile.PortalError,pattern) as caught:
                    network_profile.fetch_portal_ticket({**self.portal_block(tmp,portal),**block},destination)
                self.assertNotIn(TOKEN,str(caught.exception));self.assertFalse(destination.exists())
            refused(r"This account portal \(http://127\.0\.0\.1:\d+\) doesn't issue tickets for other\.example\.org",audience='other.example.org')
            portal.reply_audience='play.example.net'
            refused(r"sent a ticket for 'play\.example\.net', not test\.example\.org")
            portal.reply_audience=None;portal.revoked=True
            refused('does not accept the token')
            portal.revoked=False;portal.redirect=True
            refused(r'unexpected reply \(HTTP 307\)')
            self.assertEqual(portal.requests[-1][0],'/api/v1/ticket')  # the redirect was not followed
            portal.redirect=False
            closed=http.server.ThreadingHTTPServer(('127.0.0.1',0),http.server.BaseHTTPRequestHandler)
            port=closed.server_address[1];closed.server_close()
            refused('Cannot reach the account portal',url=f'http://127.0.0.1:{port}')
            # Token files: missing, not a token, or readable by others.
            refused('Cannot read the portal token file',token_file=str(Path(tmp)/'missing'))
            bad=Path(tmp)/'bad.token';bad.write_text('hunter2\n');bad.chmod(0o600)
            refused('holds no device token',token_file=str(bad))
            if os.name=='posix':
                shared=Path(tmp)/'shared.token';shared.write_text(TOKEN);shared.chmod(0o644)
                refused('private to its owner',token_file=str(shared))

    @unittest.skipUnless(shutil.which('openssl'),'openssl')
    def test_untrusted_certificates_are_refused(self):
        import ssl,subprocess
        portal=FakePortal(self)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost',
                            '-addext','subjectAltName=DNS:localhost','-keyout',str(root/'key.pem'),'-out',str(root/'cert.pem')],
                           check=True,capture_output=True,timeout=60)
            context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(root/'cert.pem',root/'key.pem')
            portal.server.socket=context.wrap_socket(portal.server.socket,server_side=True)
            url='https://localhost:'+str(portal.server.server_address[1])
            with self.assertRaisesRegex(network_profile.PortalError,'Cannot reach the account portal.*CERTIFICATE_VERIFY_FAILED|Cannot reach the account portal.*certificate'):
                network_profile.fetch_portal_ticket({**self.portal_block(tmp,portal),'url':url},root/'portal-ticket.bin')
            self.assertEqual(portal.requests,[])
