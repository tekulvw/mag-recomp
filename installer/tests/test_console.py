"""Installer output never fails on text the console or a redirected file cannot encode.

Windows encodes redirected stdout and stderr with the ANSI code page (cp1252 here), which has no
arrow; the voice chat line names MAG Settings → General → Voice."""
import io
import json
import sys
import unittest
from contextlib import ExitStack
from unittest.mock import patch

from mag_installer import console, voice_option
from mag_installer.model import InstallerError

VOICE_LINE = voice_option.describe(True, 'chosen')


def cp1252_stream():
    """A stream like Windows' redirected stdout: cp1252, strict errors, over bytes we can inspect."""
    raw = io.BytesIO()
    return io.TextIOWrapper(raw, encoding='cp1252', errors='strict', newline='\n', write_through=True), raw


class WriteLineTests(unittest.TestCase):
    def test_the_voice_line_is_not_encodable_as_cp1252(self):
        self.assertIn('→', VOICE_LINE)
        stream, _ = cp1252_stream()
        with self.assertRaises(UnicodeEncodeError):
            stream.write(VOICE_LINE)

    def test_write_line_replaces_what_the_stream_cannot_encode(self):
        stream, raw = cp1252_stream()
        console.write_line(stream, VOICE_LINE)
        written = raw.getvalue().decode('cp1252')
        self.assertEqual(written, VOICE_LINE.replace('→', '?') + '\n')

    def test_write_line_without_a_stream_does_nothing(self):
        console.write_line(None, VOICE_LINE)

    def test_streams_become_utf8(self):
        stdout, out_raw = cp1252_stream()
        stderr, err_raw = cp1252_stream()
        with patch.object(sys, 'stdout', stdout), patch.object(sys, 'stderr', stderr):
            console.use_utf8_streams()
            self.assertEqual(sys.stdout.encoding, 'utf-8')
            console.write_line(sys.stdout, VOICE_LINE)
            print(VOICE_LINE, file=sys.stderr, flush=True)
        self.assertEqual(out_raw.getvalue().decode('utf-8'), VOICE_LINE + '\n')
        self.assertEqual(err_raw.getvalue().decode('utf-8'), VOICE_LINE + '\n')

    def test_missing_or_fixed_streams_are_left_alone(self):
        # A windowed executable has no streams; StringIO cannot be reconfigured.
        with patch.object(sys, 'stdout', None), patch.object(sys, 'stderr', io.StringIO()):
            console.use_utf8_streams()
            self.assertIsNone(sys.stdout)


class CommandLineTests(unittest.TestCase):
    """mag-installer build --voice with both streams redirected to cp1252 files."""

    def run_build(self, *mode, fail=None):
        stdout, out_raw = cp1252_stream()
        stderr, err_raw = cp1252_stream()

        def install(*args, events, **kwargs):
            self.assertIs(kwargs['voice'], True)
            events({'event': 'build-voice', 'voice': True, 'source': 'chosen', 'message': VOICE_LINE})
            if fail:
                raise InstallerError(fail)

        from mag_installer.__main__ import main
        with ExitStack() as stack:
            stack.enter_context(patch('mag_installer.player_build.install', side_effect=install))
            stack.enter_context(patch.object(sys, 'stdout', stdout))
            stack.enter_context(patch.object(sys, 'stderr', stderr))
            code = main([*mode, 'build', '--source', '/input', '--parent', '/destination', '--voice'])
        return code, out_raw.getvalue().decode('utf-8'), err_raw.getvalue().decode('utf-8')

    def test_human_output_carries_the_voice_line(self):
        code, out, err = self.run_build('--human')
        self.assertEqual(code, 0)
        self.assertIn(VOICE_LINE, out)
        self.assertEqual(err, '')

    def test_json_output_carries_the_voice_line(self):
        code, out, _ = self.run_build('--json')
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out.splitlines()[0])['message'], VOICE_LINE)

    def test_errors_with_any_text_reach_stderr(self):
        code, out, err = self.run_build('--human', fail='Stopped → see the log')
        self.assertEqual(code, 2)
        self.assertIn(VOICE_LINE, out)
        self.assertIn('Stopped → see the log', err)


if __name__ == '__main__':
    unittest.main()
