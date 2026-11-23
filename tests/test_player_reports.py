"""Performance and error reports (src/player_reports.cpp): triggers and thresholds, the report body
against the portal's contract, redaction, payload caps, the queue and retries, and an upload through
the platform's HTTPS transport to a fake portal. Every input is synthetic."""
import base64
import gzip
import http.server
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPILER = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')
SOURCES = ['tests/player_reports.cpp', 'src/player_reports.cpp', 'src/player_gzip.cpp', 'src/player_portal.cpp']
TOKEN = 'mgd_SyntheticDeviceToken0123456789abcdef'
UUID = re.compile(r'^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$')
LOG_NAME = re.compile(r'^[a-z0-9][a-z0-9._-]{0,63}$')


def transport():
    """The platform transport's sources and link flags, or None where none builds here."""
    if sys.platform == 'darwin':
        return ['src/player_portal_macos.mm'], ['-fobjc-arc', '-framework', 'AppKit']
    if sys.platform == 'linux' and pathlib.Path('/usr/include/openssl/ssl.h').is_file():
        return ['src/player_portal_openssl.cpp'], ['-lssl', '-lcrypto']
    return None


def check_contract(test, body):
    """The portal's rules (POST /api/v1/reports, schema_version 1)."""
    test.assertLessEqual(len(body), 4 << 20)
    report = json.loads(body)
    test.assertLessEqual(set(report), {'schema_version', 'report_id', 'trigger', 'client', 'session', 'metrics', 'logs'})
    test.assertEqual(report['schema_version'], 1)
    test.assertRegex(report['report_id'], UUID)
    test.assertIn(report['trigger'], {'crash', 'launch_error', 'halt', 'slow_frames', 'audio', 'manual'})
    client = report['client']
    test.assertTrue(1 <= len(client['version']) <= 64)
    test.assertIn(client['platform'], {'macos', 'linux', 'windows'})
    test.assertLessEqual(len(json.dumps(client).encode()), 16 << 10)
    test.assertLessEqual(len(json.dumps(report.get('session', {})).encode()), 4 << 10)
    test.assertLessEqual(len(json.dumps(report.get('metrics', {})).encode()), 64 << 10)
    logs = report.get('logs', {})
    test.assertLessEqual(len(logs), 8)
    total, decoded = 0, {}
    for name, blob in logs.items():
        test.assertRegex(name, LOG_NAME)
        test.assertEqual(set(blob), {'encoding', 'data'})
        test.assertEqual(blob['encoding'], 'gzip+base64')
        raw = base64.b64decode(blob['data'], validate=True)
        test.assertEqual(raw[:2], b'\x1f\x8b')
        test.assertLessEqual(len(raw), 2 << 20)
        total += len(raw)
        decoded[name] = gzip.decompress(raw).decode('utf-8', 'replace')
    test.assertLessEqual(total, 3 << 20)
    return report, decoded


@unittest.skipUnless(COMPILER, 'C++ compiler')
class PlayerReportsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = pathlib.Path(cls.temp.name) / 'player_reports'
        platform = transport()
        sources, flags = platform if platform else ([], [])
        cls.can_send = platform is not None
        main = SOURCES if platform else SOURCES + ['tests/player_reports_no_transport.cpp']
        build = subprocess.run([COMPILER, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                                *[str(ROOT / s) for s in main + sources], *flags, '-o', str(cls.binary)],
                               capture_output=True, text=True, timeout=300)
        if build.returncode:
            raise AssertionError(build.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_binary(self, *args, timeout=300):
        return subprocess.run([str(self.binary), *map(str, args)], capture_output=True, text=True, timeout=timeout)

    def test_triggers_redaction_caps_and_queue(self):
        with tempfile.TemporaryDirectory() as scratch:
            result = self.run_binary('selftest', scratch)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('[player_reports] PASS', result.stdout)

    def test_report_body_follows_the_contract_and_hides_secrets(self):
        with tempfile.TemporaryDirectory() as scratch:
            result = self.run_binary('build', scratch)
            self.assertEqual(result.returncode, 0, result.stderr)
            body = pathlib.Path(result.stdout.strip()).read_bytes()
            report, logs = check_contract(self, body)
            self.assertEqual(report['trigger'], 'crash')
            self.assertEqual(report['session']['engine_exit'], 139)
            self.assertEqual(report['client']['render']['output'], '1280x720')
            self.assertEqual(report['metrics']['engine']['role'], 'engine')
            self.assertEqual(set(logs), {'engine.log', 'launcher.log', 'supervisor.log'})
            everything = body.decode() + ''.join(logs.values())
            for secret in (TOKEN, 'SyntheticDeviceToken', 'synthetic-player', 'synthetic.player', 'portal-link', '/home' + '/someone'):
                self.assertNotIn(secret.lower(), everything.lower())
            self.assertIn('~/MAG/game', logs['launcher.log'])
            self.assertIn('Bearer [redacted]', logs['launcher.log'])
            self.assertIn('~\\MAG\\game', logs['engine.log'])
            self.assertIn('C:\\Users\\Public\\ok', logs['engine.log'])

    def serve(self, replies):
        seen = []
        class Portal(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def log_message(self, *args): pass
            def do_POST(self):
                body = self.rfile.read(int(self.headers['Content-Length']))
                seen.append((self.path, self.headers.get('Authorization'), self.headers.get('Content-Type'), body))
                status, headers = replies.pop(0) if replies else (201, {})
                report_id = json.loads(body).get('report_id', '')
                reply = json.dumps({'report_id': report_id, 'received_at': '2026-11-23T00:00:00Z'}).encode()
                self.send_response(status)
                for name, value in headers.items(): self.send_header(name, value)
                self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(reply)))
                self.send_header('Connection', 'close'); self.end_headers(); self.wfile.write(reply)
                self.close_connection = True
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Portal)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        return f'http://127.0.0.1:{server.server_address[1]}', seen

    def test_upload_to_a_fake_portal(self):
        if not self.can_send:
            self.skipTest('no platform transport builds here')
        with tempfile.TemporaryDirectory() as scratch:
            scratch = pathlib.Path(scratch)
            queued = pathlib.Path(self.run_binary('build', scratch).stdout.strip())
            self.assertTrue(queued.is_file())
            replies = [(429, {'Retry-After': '60'})]
            portal, seen = self.serve(replies)
            config = scratch / 'config'
            (config / 'portal-link.json').write_text(json.dumps(
                {'schema_version': 1, 'portal': portal, 'device_token': TOKEN, 'player_name': 'Synthetic'}))
            queue = scratch / 'home/reports'
            # Rate limited: the report stays queued until Retry-After has passed.
            result = self.run_binary('send', queue, config / 'settings.json', timeout=120)
            self.assertIn('kept 1', result.stdout)
            self.assertTrue(queued.is_file())
            state = (queue / (queued.stem + '.state')).read_text().split()
            self.assertEqual(state[0], 'crash')
            state[2] = '0'  # due now
            (queue / (queued.stem + '.state')).write_text(' '.join(state) + '\n')
            result = self.run_binary('send', queue, config / 'settings.json', timeout=120)
            self.assertIn('sent 1', result.stdout)
            self.assertFalse(queued.exists())
            self.assertEqual(len(seen), 2)
            path, authorization, content_type, body = seen[1]
            self.assertEqual((path, authorization, content_type), ('/api/v1/reports', 'Bearer ' + TOKEN, 'application/json'))
            self.assertEqual(seen[0][3], body)  # the same report, with the same id, on retry
            check_contract(self, body)


if __name__ == '__main__':
    unittest.main()
