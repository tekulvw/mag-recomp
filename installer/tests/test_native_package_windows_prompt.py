"""The Windows supervisor's player-name prompt: its rule and when it may appear (synthetic, host-built)."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from mag_installer import native_package_windows as windows

COMPILER = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')


class WindowsNamePromptTests(unittest.TestCase):
    def rule(self):
        match = re.search(r'/\* name-rule-begin \*/(.*?)/\* name-rule-end \*/', windows._SUPERVISOR, re.S)
        self.assertIsNotNone(match)
        return match.group(1)

    @unittest.skipUnless(COMPILER, 'C compiler')
    def test_rule_matches_the_game_settings(self):
        cases = {'Player': 1, 'PlayerRetail': 1, 'Rook_7': 1, 'abc': 1, 'Pilot-One-15chr': 1,
                 '': 0, 'ab': 0, '7up': 0, '_abc': 0, 'bad name': 0, 'sixteen-chars-xx': 0, 'n\u00e4me': 0}
        body = '\n'.join('    if (valid_player_name(L"%s") != %d) return %d;' % (name.replace('\\', '\\\\'), want, i + 1)
                         for i, (name, want) in enumerate(cases.items()))
        source = '#include <stddef.h>\n#include <wchar.h>\n' + self.rule() + '\nint main(void) {\n' + body + '\n    return 0;\n}\n'
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'rule.c'; path.write_text(source, encoding='utf-8')
            subprocess.run([COMPILER, '-std=c11', '-Wall', '-Werror', str(path), '-o', str(Path(tmp) / 'rule')],
                           check=True, capture_output=True, timeout=60)
            result = subprocess.run([str(Path(tmp) / 'rule')], timeout=10)
            self.assertEqual(result.returncode, 0, 'case %d failed' % result.returncode)

    def test_prompt_only_on_the_games_cue_and_never_with_no_prompt(self):
        source = windows._SUPERVISOR
        self.assertIn('L"--no-prompt"', source)
        loop = source[source.index('for (int attempt = 2;'):source.index('DeleteFileW(marker);\n    if (result != 90)')]
        # Asked only while interactive, after a failed launcher stage whose game asked for a name.
        self.assertIn('result != 90 && prompt', loop)
        self.assertIn('GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES', loop)
        self.assertIn('int answer = ask_player_name(chosen, 16, settings, server);\n        if (!answer) break;', loop)
        self.assertIn('else set_env(L"MAG_PLAYER_SET_NAME", chosen);', loop)
        # Linking from the prompt instead fetches a ticket, whose account name the launcher saves.
        self.assertIn('if (answer == 2) portal_phase(settings, server, ticket, 1, logs, prompt);', loop)
        # The chosen name never reaches the engine as a first-run option.
        self.assertIn('L"MAG_PLAYER_SET_NAME"};', source)

    def test_modal_loop_wakes_when_the_window_closes(self):
        source = windows._SUPERVISOR
        destroy = source[source.index('case WM_DESTROY:'):source.index('return DefWindowProcW(window, message, w, l);')]
        self.assertIn('state->done = 1', destroy)
        self.assertIn('PostMessageW(NULL, WM_NULL, 0, 0);', destroy)  # a sent click must not leave GetMessageW waiting
        self.assertIn('while (!state.done && GetMessageW(&message, NULL, 0, 0) > 0)', source)

    def test_supervisor_links_the_prompt_libraries(self):
        import inspect
        compile_source = inspect.getsource(windows._compile)
        for library in ('user32.lib', 'gdi32.lib', 'shell32.lib', 'winhttp.lib'):  # windows, font, command line, portal
            self.assertIn(library, compile_source)

    def test_account_link_and_a_ticket_before_each_phase(self):
        source = windows._SUPERVISOR
        self.assertIn('if (!wcscmp(a, L"--link")) { account = 1; continue; }', source)
        self.assertIn('if (!wcscmp(a, L"--unlink")) { account = 2; continue; }', source)
        # Account commands exit before any game file is checked or a launch folder is made.
        self.assertLess(source.index('if (account) {'), source.index('check(launcher, MAG_LAUNCHER_SHA256)'))
        launcher = source.index('portal_phase(settings, server, ticket, 1, logs, prompt);\n    join(log, logs, L"launcher.log");')
        engine = source.index('portal_phase(settings, NULL, ticket, 0, logs, prompt);\n    join(log, logs, L"engine.log");')
        self.assertLess(launcher, engine)
        phase = source[source.index('static void portal_phase('):source.index('int WINAPI wWinMain(')]
        self.assertIn('SetEnvironmentVariableW(L"MAG_PLAYER_PORTAL_TICKET", NULL);', phase)  # never a stale ticket
        self.assertIn('if (launcher && name[0]) set_env(L"MAG_PLAYER_SET_NAME", name);', phase)
        self.assertIn('if (prompt) MessageBoxW', source[source.index('static void notice('):])  # --no-prompt: logged only
        self.assertTrue(all((windows.Path(__file__).resolve().parents[2] / 'src' / name).is_file() for name in windows.SUPERVISOR_SOURCES))


if __name__ == '__main__':
    unittest.main()
