"""The Windows supervisor's setup window and account link: when they may appear (synthetic, host-built)."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from mag_installer import native_package_windows as windows

MINGW = shutil.which('x86_64-w64-mingw32-g++')
ROOT = Path(__file__).resolve().parents[2]


class WindowsSetupTests(unittest.TestCase):
    def main(self):
        source = windows._SUPERVISOR
        return source[source.index('int WINAPI wWinMain('):]

    def test_setup_opens_at_every_launch_unless_the_caller_opts_out(self):
        source = windows._SUPERVISOR
        main = self.main()
        self.assertIn('L"--no-prompt"', source)
        # Every launch: the settings window with Play MAG, before the launcher and its ticket.
        gate = 'if (prompt && !server && !player && !(skip[0] && wcscmp(skip, L"0"))) {'
        self.assertIn(gate, main)
        self.assertIn('DWORD chosen = run_settings(job, engine, logs, NULL);\n        if (chosen == 3) { mag_reports_end(NULL); return 0; }', main)
        self.assertLess(main.index(gate), main.index('portal_phase(settings, server, ticket, 1, logs, prompt);'))
        self.assertLess(main.index('HANDLE job = CreateJobObjectW'), main.index(gate))
        self.assertLess(main.index('check(engine, MAG_ENGINE_SHA256)'), main.index(gate))  # a verified engine only
        # The caller's MAG_SKIP_SETUP is read before the inherited MAG_ variables are dropped.
        self.assertLess(main.index('GetEnvironmentVariableW(L"MAG_SKIP_SETUP", skip, 8);'), main.index('clear_inherited();'))
        # The launcher never opens a second setup, and the engine never sees the switch.
        self.assertIn('set_env(L"MAG_SKIP_SETUP", L"1");', main)
        self.assertIn('L"MAG_SKIP_SETUP"};', main)
        run = source[source.index('static DWORD run_settings('):source.index('/* Account messages')]
        self.assertIn('--mag-player-settings-setup', run)
        self.assertIn('AssignProcessToJobObject(job, process.hProcess);', run)

    def test_missing_name_reopens_the_settings_window_never_with_no_prompt(self):
        main = self.main()
        loop = main[main.index('for (int attempt = 2;'):main.index('DeleteFileW(marker);\n    if (result != 90)')]
        self.assertIn('result != 90 && prompt', loop)
        self.assertIn('GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES', loop)
        self.assertIn('DWORD answer = run_settings(job, engine, logs, L"--notice=player-name");', loop)
        self.assertIn('if (answer == 3) { mag_reports_end(NULL); return 0; }', loop)
        # A link made in the window supplies the ticket and the account's name for the next try.
        self.assertIn('portal_phase(settings, server, ticket, 1, logs, prompt);', loop)
        # The supervisor's own name prompt is gone: one window for every choice.
        self.assertNotIn('ask_player_name', windows._SUPERVISOR)

    def test_logs_folder_for_the_debug_page(self):
        # MAG Settings' Debug page opens this MAG folder's logs, which the supervisor names.
        self.assertIn('set_env(L"MAG_PLAYER_LOGS_ROOT", logs_root);', self.main())

    def test_supervisor_links_the_window_libraries(self):
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
        engine = source.index('if (portal_phase(settings, NULL, ticket, 0, logs, prompt) == MAG_PORTAL_READY) {')
        self.assertLess(launcher, engine)
        phase = source[source.index('static int portal_phase('):source.index('int WINAPI wWinMain(')]
        self.assertIn('SetEnvironmentVariableW(L"MAG_PLAYER_PORTAL_TICKET", NULL);', phase)  # never a stale ticket
        self.assertIn('if (launcher && name[0]) set_env(L"MAG_PLAYER_SET_NAME", name);', phase)
        self.assertIn('if (prompt) MessageBoxW', source[source.index('static void notice('):])  # --no-prompt: logged only
        self.assertTrue(all((ROOT / 'src' / name).is_file() for name in windows.SUPERVISOR_SOURCES))

    # Off Windows only: on Windows the installer job builds the real supervisor with clang-cl, and
    # a hosted MinGW may have older headers (no BCRYPT_SHA256_ALG_HANDLE).
    @unittest.skipUnless(MINGW and os.name != 'nt', 'MinGW-w64 cross compiler, off Windows')
    def test_supervisor_compiles(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            windows.copy_supervisor_sources(ROOT, folder)
            digest = '"' + '0' * 64 + '"'
            (folder / 'supervisor_config.h').write_text(
                '#define MAG_GAME_ROOT L"..\\\\game"\n' + ''.join(
                    f'#define {name} {digest}\n' for name in ('MAG_LAUNCHER_SHA256', 'MAG_ENGINE_SHA256', 'MAG_LAUNCHER_ELF_SHA256',
                                                             'MAG_ENGINE_ELF_SHA256', 'MAG_ENGINE_SELF_SHA256')))
            (folder / 'supervisor.c').write_text(windows._SUPERVISOR, encoding='utf-8')
            gcc = MINGW[:-2] + 'cc'
            result = subprocess.run([gcc if Path(gcc).exists() else MINGW, '-x', 'c', '-std=c11', '-Wall', '-Werror',
                                     '-Wno-unused-function', '-c', str(folder / 'supervisor.c'), '-o', str(folder / 's.o')],
                                    capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)


    def test_friends_run_beside_the_engine_only(self):
        """Friends and presence (docs/player-social.md): started only when the engine's ticket is
        ready, stopped when the engine exits, with the caller's MAG_SOCIAL read before MAG_
        variables are cleared; the token stays in this process (MAG_SOCIAL_SPOOL names a folder)."""
        source = windows._SUPERVISOR
        main = source[source.index('int WINAPI wWinMain('):]
        self.assertLess(main.index('GetEnvironmentVariableW(L"MAG_SOCIAL", social, 8);'), main.index('clear_inherited();'))
        start = main.index('mag_social_start(settings, NULL, spool, friends_log, social[0] && !wcscmp(social, L"0"));')
        self.assertLess(main.index('if (portal_phase(settings, NULL, ticket, 0, logs, prompt) == MAG_PORTAL_READY) {'), start)
        self.assertLess(start, main.index('DWORD code = run(job, engine, engine_elf, logs, log);'))
        self.assertEqual(main.count('mag_social_start('), 1)
        portal = (ROOT / 'src/player_portal_win32.cpp').read_text(encoding='utf-8')
        self.assertIn('SetEnvironmentVariableW(L"MAG_SOCIAL_SPOOL", spool_dir);', portal)
        self.assertNotIn('MAG_SOCIAL_TOKEN', portal)

    def test_session_ends_with_a_report_decision_on_every_path(self):
        """Performance and error reports (src/player_reports_win32.h): begun once the launch folder
        exists, ended on every way out, with the caller's MAG_REPORTS read before MAG_ variables are cleared."""
        source = windows._SUPERVISOR
        main = source[source.index('int WINAPI wWinMain('):]
        self.assertLess(main.index('GetEnvironmentVariableW(L"MAG_REPORTS", reports, 8);'), main.index('clear_inherited();'))
        self.assertLess(main.index('clear_inherited();'), main.index('set_env(L"MAG_SESSION_DIR", logs);'))
        self.assertLess(main.index('set_env(L"MAG_SESSION_DIR", logs);'), main.index('mag_reports_begin(self, g_root, logs, settings'))
        self.assertIn('mag_reports_end(text);', source[source.index('static void fail('):source.index('static void join(')])
        self.assertIn('mag_reports_player_stop(); fail(message, NULL);', main)
        self.assertIn('DWORD code = run(job, engine, engine_elf, logs, log);\n    mag_social_stop();\n    mag_reports_engine_exit(code);\n    mag_reports_end(NULL);', main)
        self.assertEqual(main.count('mag_reports_launcher_exit(result);'), 2)
        # The detached sender: before any game file is checked or a launch folder is made.
        self.assertLess(main.index('return mag_reports_send(g_root, send_logs, settings);'), main.index('check(launcher, MAG_LAUNCHER_SHA256)'))

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
