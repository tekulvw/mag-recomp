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
        self.assertIn('DWORD chosen = run_settings(job, engine, logs, NULL);\n        if (chosen == 3) return 0;', main)
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
        self.assertIn('if (answer == 3) return 0;', loop)
        # A link made in the window supplies the ticket and the account's name for the next try.
        self.assertIn('portal_phase(settings, server, ticket, 1, logs, prompt);', loop)
        # The supervisor's own name prompt is gone: one window for every choice.
        self.assertNotIn('ask_player_name', windows._SUPERVISOR)

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
        engine = source.index('portal_phase(settings, NULL, ticket, 0, logs, prompt);\n    join(log, logs, L"engine.log");')
        self.assertLess(launcher, engine)
        phase = source[source.index('static void portal_phase('):source.index('int WINAPI wWinMain(')]
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


if __name__ == '__main__':
    unittest.main()
