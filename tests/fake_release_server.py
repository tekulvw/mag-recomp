"""A fake GitHub release server on 127.0.0.1 for the launcher's update check (src/player_update.h).

Serves `/repos/<owner>/<repo>/releases/latest` and the release's assets, with the same redirect
hop GitHub's download addresses take (to an asset host), conditional requests (ETag, 304) and
switches for the cases the check must handle. Everything it serves is synthetic: the caller hands
it the checksum file, its signature (made with a throwaway key) and archive bytes.

    server = FakeReleaseServer(tag='v0.16.0', sums=..., signature=..., archive=b'...')
    server.start(); ... server.api ...; server.stop()

Modes (`server.mode`):
    newer          a stable release with every asset (the default)
    none           no release published (404)
    prerelease     the latest release is marked prerelease
    draft          the latest release is marked draft
    unsigned       SHA256SUMS.txt.minisig is not attached yet
    rate-limited   403 with X-RateLimit-Remaining: 0 and a reset time
    retry-after    429 with Retry-After: 120
    truncated      the signature download ends early
    redirect-away  the download address redirects outside GitHub (and outside this server)
    malformed      the API answers with broken JSON
"""
from __future__ import annotations

import hashlib
import http.server
import json
import threading
import time


class FakeReleaseServer:
    def __init__(self, tag, sums, signature, archive=b'synthetic archive', platform='windows-x86_64',
                 repository='tekulvw/mag-recomp'):
        self.tag, self.sums, self.signature, self.archive = tag, sums, signature, archive
        self.platform, self.repository = platform, repository
        self.mode = 'newer'
        self.etag = '"fake-1"'
        self.requests = []  # (method, path, headers)
        self.lock = threading.Lock()
        self.server = None

    # ---- what it serves ----
    def archive_name(self):
        extension = '.tar.gz' if self.platform.startswith('linux') else '.zip'
        return f'mag-installer-{self.tag}-{self.platform}{extension}'

    def assets(self):
        files = {self.archive_name(): self.archive, 'SHA256SUMS.txt': self.sums}
        if self.mode != 'unsigned':
            files['SHA256SUMS.txt.minisig'] = self.signature
        return files

    def release(self):
        base = self.origin
        assets = []
        for index, (name, data) in enumerate(self.assets().items()):
            assets.append({
                'name': name, 'size': len(data), 'state': 'uploaded',
                'browser_download_url': f'{base}/{self.repository}/releases/download/{self.tag}/{name}',
                'url': f'{base}/repos/{self.repository}/releases/assets/{index + 1}',
                'digest': 'sha256:' + hashlib.sha256(data).hexdigest() if name == self.archive_name() else None,
                'uploader': {'login': 'github-actions[bot]', 'id': 41898282},
            })
        return {
            'tag_name': self.tag, 'name': f'MAG {self.tag[1:]}', 'draft': self.mode == 'draft',
            'prerelease': self.mode == 'prerelease',
            'html_url': f'https://github.com/{self.repository}/releases/tag/{self.tag}',
            'body': '## Changes\n\nSynthetic release notes.', 'assets': assets,
        }

    # ---- the server ----
    @property
    def origin(self):
        return f'http://127.0.0.1:{self.server.server_address[1]}'

    @property
    def api(self):
        return self.origin

    def start(self):
        fake = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *args):
                pass

            def reply(self, status, body=b'', headers=()):
                self.send_response(status)
                for name, value in headers:
                    self.send_header(name, value)
                self.send_header('Content-Length', str(len(body)))
                self.send_header('Connection', 'close')
                self.end_headers()
                if body and self.command != 'HEAD':
                    self.wfile.write(body)
                self.close_connection = True

            def do_GET(self):
                with fake.lock:
                    fake.requests.append(('GET', self.path, dict(self.headers.items())))
                    mode = fake.mode
                path = self.path.split('?', 1)[0]
                latest = f'/repos/{fake.repository}/releases/latest'
                if path == latest:
                    if mode == 'none':
                        return self.reply(404, b'{"message":"Not Found"}')
                    if mode == 'rate-limited':
                        return self.reply(403, b'{"message":"API rate limit exceeded"}',
                                          [('X-RateLimit-Remaining', '0'), ('X-RateLimit-Reset', str(int(time.time()) + 1800))])
                    if mode == 'retry-after':
                        return self.reply(429, b'{}', [('Retry-After', '120')])
                    if mode == 'malformed':
                        return self.reply(200, b'{"tag_name": "v9', [('Content-Type', 'application/json')])
                    if self.headers.get('If-None-Match') == fake.etag:
                        return self.reply(304, b'', [('ETag', fake.etag)])
                    body = json.dumps(fake.release()).encode()
                    return self.reply(200, body, [('Content-Type', 'application/json'), ('ETag', fake.etag)])
                prefix = f'/{fake.repository}/releases/download/{fake.tag}/'
                if path.startswith(prefix):
                    name = path[len(prefix):]
                    if mode == 'redirect-away':
                        return self.reply(302, b'', [('Location', f'https://example.invalid/{name}')])
                    # GitHub's download address redirects to a signed asset-host address.
                    return self.reply(302, b'', [('Location', f'{fake.origin}/assets/{name}?sig=synthetic&sp=r')])
                if path.startswith('/assets/'):
                    name = path[len('/assets/'):]
                    data = fake.assets().get(name)
                    if data is None:
                        return self.reply(404)
                    if mode == 'truncated' and name.endswith('.minisig'):
                        data = data[: len(data) // 2]
                    return self.reply(200, data, [('Content-Type', 'application/octet-stream')])
                return self.reply(404)

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        return self

    def stop(self):
        if self.server:
            self.server.shutdown()
            self.server.server_close()
            self.server = None

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

    def latest_requests(self):
        with self.lock:
            return [r for r in self.requests if r[1].startswith(f'/repos/{self.repository}/releases/latest')]
