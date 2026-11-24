"""Portal account linking and ticket fetch (src/player_portal.cpp) against a fake transport.

All inputs are synthetic: link codes, device tokens and tickets are made up by the test.
"""
import http.server
import pathlib
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPILER = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')


@unittest.skipUnless(COMPILER, 'C++ compiler')
class PortalCoreTests(unittest.TestCase):
    def test_link_ticket_storage_and_launch_flows(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'portal'
            # C++17: the packaged supervisors compile this core too.
            build = subprocess.run([COMPILER, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                                    str(ROOT / 'tests/player_portal.cpp'), str(ROOT / 'src/player_portal.cpp'),
                                    '-pthread', '-o', str(binary)], capture_output=True, text=True, timeout=300)
            self.assertEqual(build.returncode, 0, build.stderr)
            scratch = pathlib.Path(tmp) / 'scratch'; scratch.mkdir()
            result = subprocess.run([str(binary), str(scratch)], capture_output=True, text=True, timeout=300)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('portal link and ticket checks passed', result.stdout)



def openssl_headers():
    return sys.platform == 'linux' and COMPILER and shutil.which('openssl') and pathlib.Path('/usr/include/openssl/ssl.h').is_file()


class LocalServer:
    """A local HTTP(S) test server and the transport driver built by setUpClass."""

    def serve(self, certificate, chunked=False):
        seen = []
        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def log_message(self, *args): pass
            def do_POST(self):
                seen.append((self.path, self.headers.get('Content-Type'), self.headers.get('X-Test'),
                             self.rfile.read(int(self.headers['Content-Length']))))
                body = b'{"ok":true}'
                self.send_response(201); self.send_header('Content-Type', 'application/json')
                if chunked:
                    self.send_header('Transfer-Encoding', 'chunked'); self.send_header('Connection', 'close'); self.end_headers()
                    self.wfile.write(b'4;x=y\r\n{"ok\r\n7\r\n":true}\r\n0\r\n\r\n')
                else:
                    self.send_header('Content-Length', str(len(body))); self.send_header('Connection', 'close'); self.end_headers()
                    self.wfile.write(body)
                self.close_connection = True
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        if certificate:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            root = pathlib.Path(self.temp.name)
            context.load_cert_chain(root / (certificate + '.pem'), root / (certificate + '.key'))
            server.socket = context.wrap_socket(server.socket, server_side=True)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        return server.server_address[1], seen

    def post(self, url, trust=None):
        env = {'PATH': '/usr/bin:/bin'}
        if trust:
            env['SSL_CERT_FILE'] = str(pathlib.Path(self.temp.name) / (trust + '.pem'))
        result = subprocess.run([str(self.binary), url, '{"a":1}', 'value'], capture_output=True, text=True, timeout=60, env=env)
        return result.returncode, result.stdout


@unittest.skipUnless(openssl_headers(), 'Linux with OpenSSL development headers')
class OpenSslTransportTests(LocalServer, unittest.TestCase):
    """The Linux transport against a local TLS server: verified certificates only."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(); root = pathlib.Path(cls.temp.name)
        cls.binary = root / 'https'
        build = subprocess.run([COMPILER, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                                str(ROOT / 'tests/player_portal_https.cpp'), str(ROOT / 'src/player_portal.cpp'),
                                str(ROOT / 'src/player_portal_openssl.cpp'), '-lssl', '-lcrypto', '-o', str(cls.binary)],
                               capture_output=True, text=True, timeout=120)
        if build.returncode:
            raise AssertionError(build.stderr)
        for name, host in (('good', 'localhost'), ('wrong', 'other.example')):
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=' + host,
                            '-addext', 'subjectAltName=DNS:' + host, '-keyout', str(root / (name + '.key')),
                            '-out', str(root / (name + '.pem'))], check=True, capture_output=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_verified_https_request(self):
        port, seen = self.serve('good')
        self.assertEqual(self.post(f'https://localhost:{port}/api/v1/ticket', 'good'), (0, '201\n{"ok":true}'))
        self.assertEqual(seen, [('/api/v1/ticket', 'application/json', 'value', b'{"a":1}')])

    def test_chunked_reply(self):
        port, _ = self.serve('good', chunked=True)
        self.assertEqual(self.post(f'https://localhost:{port}/x', 'good'), (0, '201\n{"ok":true}'))

    def test_untrusted_and_mismatched_certificates_are_refused(self):
        port, seen = self.serve('good')
        code, out = self.post(f'https://localhost:{port}/x')  # only the system store: not trusted
        self.assertEqual(code, 1); self.assertIn("certificate was refused", out)
        port, seen = self.serve('wrong')
        code, out = self.post(f'https://localhost:{port}/x', 'wrong')  # trusted, but for another name
        self.assertEqual(code, 1); self.assertIn("certificate was refused", out)
        self.assertEqual(seen, [])  # nothing was sent

    def test_plain_http_and_unreachable(self):
        port, _ = self.serve(None)
        self.assertEqual(self.post(f'http://127.0.0.1:{port}/x'), (0, '201\n{"ok":true}'))
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), http.server.BaseHTTPRequestHandler)
        closed = server.server_address[1]; server.server_close()
        code, out = self.post(f'http://127.0.0.1:{closed}/x')
        self.assertEqual(code, 1); self.assertIn('cannot connect', out)



@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun') and shutil.which('openssl'), 'macOS')
class SessionTransportTests(LocalServer, unittest.TestCase):
    """The macOS NSURLSession transport, headless: no window or browser is opened."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(); root = pathlib.Path(cls.temp.name)
        cls.binary = root / 'https'
        build = subprocess.run(['xcrun', 'clang++', '-std=c++20', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                                '-I' + str(ROOT / 'src'), str(ROOT / 'tests/player_portal_https.cpp'), str(ROOT / 'src/player_portal.cpp'),
                                str(ROOT / 'src/player_portal_macos.mm'), '-framework', 'AppKit', '-o', str(cls.binary)], capture_output=True, text=True, timeout=120)
        if build.returncode:
            raise AssertionError(build.stderr)
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=localhost',
                        '-addext', 'subjectAltName=DNS:localhost', '-keyout', str(root / 'good.key'), '-out', str(root / 'good.pem')],
                       check=True, capture_output=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    # The system trust store cannot be pointed at a test certificate here, so only refusals are checked.
    def test_untrusted_and_mismatched_certificates_are_refused(self):
        port, seen = self.serve('good')
        code, out = self.post(f'https://localhost:{port}/x')
        self.assertEqual(code, 1, out); self.assertEqual(seen, [])

    def test_plain_http_and_unreachable(self):
        port, seen = self.serve(None)
        self.assertEqual(self.post(f'http://127.0.0.1:{port}/x'), (0, '201\n{"ok":true}'))
        self.assertEqual(seen, [('/x', 'application/json', 'value', b'{"a":1}')])
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), http.server.BaseHTTPRequestHandler)
        closed = server.server_address[1]; server.server_close()
        self.assertEqual(self.post(f'http://127.0.0.1:{closed}/x')[0], 1)

    def test_redirects_are_not_followed(self):
        hits = []
        class Redirect(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def do_POST(self):
                hits.append(self.path); self.send_response(307); self.send_header('Location', '/elsewhere')
                self.send_header('Content-Length', '0'); self.end_headers()
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Redirect)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        code, out = self.post(f'http://127.0.0.1:{server.server_address[1]}/x')
        self.assertEqual((code, out.split('\n')[0]), (0, '307')); self.assertEqual(hits, ['/x'])


if __name__ == '__main__':
    unittest.main()
