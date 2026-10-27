import contextlib
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
from pathlib import Path
import socket
import tarfile
import tempfile
import threading
import unittest
import zipfile

from mag_installer.model import InstallerError
from mag_installer.safety import Cancellation, Cancelled
from mag_installer.tools import acquire, extract_archive


def zip_bytes(entries):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, value in entries:
            archive.writestr(name, value)
    return out.getvalue()


def tool_spec(payload, url='https://example.invalid/pinned.zip'):
    return {'id': 'fixture-tool', 'version': '1.0-test', 'url': url,
            'sha256': hashlib.sha256(payload).hexdigest(), 'size': len(payload),
            'format': 'zip', 'max_expanded_size': 1024 * 1024,
            'max_entries': 20, 'executable': 'bin/tool', 'license': 'Synthetic fixture'}


@contextlib.contextmanager
def serve(payload, truncate=False, redirect=None):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(302 if redirect else 200)
            if redirect: self.send_header('Location', redirect)
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            if not redirect:
                self.wfile.write(payload[:len(payload) // 2] if truncate else payload)
                self.wfile.flush()
                if truncate:
                    self.connection.shutdown(socket.SHUT_WR)
        def log_message(self, *args): pass
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try: yield f'http://127.0.0.1:{server.server_port}/tool.zip'
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


class ToolTests(unittest.TestCase):
    def test_verified_download_cache_reuse_and_tamper_refusal(self):
        payload = zip_bytes([('bin/tool', b'synthetic executable')])
        with tempfile.TemporaryDirectory() as temp, serve(payload) as url:
            tool = tool_spec(payload, url)
            events = []
            cache = Path(temp) / 'cache space 日本語'
            blob = acquire(tool, cache, Cancellation(), events.append, allow_loopback=True)
            self.assertEqual(blob.read_bytes(), payload)
            self.assertTrue(any(event['event'] == 'download' for event in events))
        # Cache is usable offline only while its bytes still match the pin.
        with tempfile.TemporaryDirectory() as temp, serve(payload) as url:
            tool = tool_spec(payload, url)
            cache = Path(temp)
            blob = acquire(tool, cache, Cancellation(), lambda e: None, allow_loopback=True)
            tool['url'] = 'https://unreachable.invalid/no-network-needed'
            self.assertEqual(acquire(tool, cache, Cancellation(), lambda e: None), blob)
            blob.write_bytes(b'poison')
            with self.assertRaises(InstallerError): acquire(tool, cache, Cancellation(), lambda e: None)

    def test_mismatch_and_interrupted_download_never_promote_cache(self):
        payload = zip_bytes([('bin/tool', b'fixture')])
        for failure in ('checksum', 'size', 'interrupt'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp, serve(payload, truncate=failure == 'interrupt') as url:
                tool = tool_spec(payload, url)
                if failure == 'checksum': tool['sha256'] = '0' * 64
                if failure == 'size': tool['size'] += 1
                with self.assertRaises(InstallerError): acquire(tool, Path(temp), Cancellation(), lambda e: None, allow_loopback=True)
                self.assertEqual(list(Path(temp).rglob('*.blob')), [])
                self.assertEqual(list(Path(temp).rglob('*.part')), [])
                # Interrupted transfers can be retried; partial bytes are never reused.
                if failure == 'interrupt':
                    with serve(payload) as retry_url:
                        tool['url'] = retry_url
                        self.assertEqual(acquire(tool, Path(temp), Cancellation(), lambda e: None, allow_loopback=True).read_bytes(), payload)

    def test_download_cancellation_and_untrusted_schemes_refused(self):
        payload = zip_bytes([('bin/tool', b'fixture')])
        with tempfile.TemporaryDirectory() as temp, serve(payload) as url:
            token = Cancellation()
            def events(event):
                if event['event'] == 'download': token.cancel()
            with self.assertRaises(Cancelled): acquire(tool_spec(payload, url), Path(temp), token, events, allow_loopback=True)
            self.assertEqual(list(Path(temp).rglob('*.blob')), [])
        for url in ('file:///etc/passwd', 'http://example.com/tool', 'https://user:secret@example.com/tool'):
            with self.subTest(url=url), tempfile.TemporaryDirectory() as temp, self.assertRaises(InstallerError):
                acquire(tool_spec(payload, url), Path(temp), Cancellation(), lambda e: None)

    def test_redirect_cannot_escape_transport_policy(self):
        payload = zip_bytes([('bin/tool', b'fixture')])
        with tempfile.TemporaryDirectory() as temp, serve(payload, redirect='http://example.invalid/tool') as url:
            with self.assertRaises(InstallerError): acquire(tool_spec(payload, url), Path(temp), Cancellation(), lambda e: None, allow_loopback=True)

    def test_zip_traversal_links_case_collision_and_bombs_refused(self):
        symlink = zipfile.ZipInfo('bin/tool')
        symlink.create_system = 3
        symlink.external_attr = 0o120777 << 16
        fixtures = [('../escape', [('bin/tool', b'ok'), ('../escape', b'bad')]),
                    ('drive', [('bin/tool', b'ok'), ('C:/escape', b'bad')]),
                    ('case', [('bin/tool', b'ok'), ('BIN/other', b'bad')]),
                    ('link', [(symlink, b'../../escape')]),
                    ('bomb', [('bin/tool', b'x' * 2048)])]
        for label, entries in fixtures:
            payload = zip_bytes(entries)
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                blob = root / 'blob'
                blob.write_bytes(payload)
                tool = tool_spec(payload)
                if label == 'bomb': tool['max_expanded_size'] = 32
                with self.assertRaises(InstallerError): extract_archive(blob, root / 'unpack', tool, Cancellation())
                self.assertFalse((root / 'escape').exists())
                self.assertFalse((root / 'unpack').exists())

    def test_tar_hardlinks_and_devices_refused(self):
        for kind in (tarfile.LNKTYPE, tarfile.SYMTYPE, tarfile.CHRTYPE):
            out = io.BytesIO()
            with tarfile.open(fileobj=out, mode='w:gz') as archive:
                info = tarfile.TarInfo('bin/tool')
                info.type = kind
                info.linkname = '../../escape'
                archive.addfile(info)
            payload = out.getvalue()
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                blob = root / 'blob'
                blob.write_bytes(payload)
                tool = tool_spec(payload)
                tool['format'] = 'tar.gz'
                with self.assertRaises(InstallerError): extract_archive(blob, root / 'unpack', tool, Cancellation())

    def test_tar_metadata_expansion_is_bounded_before_tar_parser(self):
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode='w:gz') as archive:
            info = tarfile.TarInfo('bin/tool')
            info.size = 7
            info.pax_headers = {'comment': 'x' * 1000000}
            archive.addfile(info, io.BytesIO(b'fixture'))
        payload = stream.getvalue()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            blob = root / 'blob'
            blob.write_bytes(payload)
            tool = tool_spec(payload)
            tool['format'] = 'tar.gz'
            tool['max_expanded_size'] = 1024
            with self.assertRaises(InstallerError): extract_archive(blob, root / 'unpack', tool, Cancellation())
            self.assertFalse((root / 'unpack').exists())

    def test_extract_success_limits_and_existing_destination_refusal(self):
        payload = zip_bytes([('bin/tool', b'synthetic executable'), ('license.txt', b'Test only')])
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            blob = root / 'blob'
            blob.write_bytes(payload)
            tool = tool_spec(payload)
            extract_archive(blob, root / 'unpack', tool, Cancellation())
            self.assertEqual((root / 'unpack/bin/tool').read_bytes(), b'synthetic executable')
            with self.assertRaises(InstallerError): extract_archive(blob, root / 'unpack', tool, Cancellation())
            tool['max_entries'] = 1
            with self.assertRaises(InstallerError): extract_archive(blob, root / 'limited', tool, Cancellation())


if __name__ == '__main__': unittest.main()
