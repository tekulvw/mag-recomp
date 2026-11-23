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


    def test_link_token_reads_standard_input_and_reports_for_automation(self):
        source = windows._SUPERVISOR
        self.assertIn('if (!wcscmp(a, L"--link-token")) { account = 3; continue; }', source)
        self.assertIn('else if (!wcsncmp(a, L"--portal-url", 12)) { target = &portal_url; prefix = 12; }', source)
        # --portal-url only with --link-token, which takes the account's name rather than --name.
        self.assertIn('if ((portal_url && account != 3) || (account == 3 && player)) fail(usage, NULL);', source)
        # An account command: it exits before any game file is checked.
        command = source[source.index('if (account) {'):source.index('check(launcher, MAG_LAUNCHER_SHA256)')]
        self.assertIn('int ok = mag_portal_link_token(settings, server, portal_url, message, 1024);', command)
        self.assertIn('report(message, ok, prompt);', command)
        # The result goes to redirected standard output or error; a message box only when interactive.
        report = source[source.index('static void report('):source.index('int WINAPI wWinMain(')]
        self.assertIn('GetStdHandle(ok ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE)', report)
        self.assertIn('else if (prompt) MessageBoxW', report)
        # The token comes from standard input only, through the shared import (src/player_portal.cpp).
        root = windows.Path(__file__).resolve().parents[2] / 'src'
        portal = (root / 'player_portal_win32.cpp').read_text(encoding='utf-8')
        body = portal[portal.index('int link_token(const wchar_t *settings_file'):portal.index('extern "C" int mag_portal_unlink(')]
        self.assertIn('GetStdHandle(STD_INPUT_HANDLE)', body)
        self.assertIn('read_device_token(input, token)', body)
        self.assertIn('import_device_token(*transport, portal, settings.server_address, token, link_path(path), link)', body)
        self.assertIn('int mag_portal_link_token(', (root / 'player_portal_win32.h').read_text(encoding='utf-8'))
        # Each phase asks for a ticket for the configured server.
        self.assertIn('settings.server_address, "Run MAG.exe --link to link it again.");', portal)

    @unittest.skipUnless(shutil.which('x86_64-w64-mingw32-g++') and shutil.which('x86_64-w64-mingw32-gcc'), 'MinGW-w64 cross compiler')
    def test_supervisor_and_portal_sources_compile(self):
        """The supervisor and its portal sources build for Windows (MinGW-w64 here; the installer uses clang-cl)."""
        with tempfile.TemporaryDirectory() as tmp:
            scratch = Path(tmp)
            digest = '"' + 'a' * 64 + '"'
            (scratch / 'supervisor_config.h').write_text(
                '#define MAG_GAME_ROOT L"..\\\\game"\n' + ''.join(f'#define MAG_{name}_SHA256 {digest}\n' for name in
                                                              ('LAUNCHER', 'ENGINE', 'LAUNCHER_ELF', 'ENGINE_ELF', 'ENGINE_SELF')) +
                '#define MAG_RUNTIME_DEFAULTS {{NULL, NULL}}\n', encoding='utf-8')
            (scratch / 'supervisor.c').write_text(windows._SUPERVISOR, encoding='utf-8')
            windows.copy_supervisor_sources(windows.Path(__file__).resolve().parents[2], scratch)
            portable = [name for name in windows.SUPERVISOR_SOURCES if name.endswith('.cpp')]
            for argv in (['x86_64-w64-mingw32-g++', '-std=c++20', '-DUNICODE', '-D_UNICODE', '-c', *portable],
                         ['x86_64-w64-mingw32-gcc', '-c', 'supervisor.c']):
                result = subprocess.run(argv, cwd=scratch, capture_output=True, text=True, timeout=300)
                self.assertEqual(result.returncode, 0, result.stderr[-3000:])


if __name__ == '__main__':
    unittest.main()
