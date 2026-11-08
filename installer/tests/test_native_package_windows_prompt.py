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
        self.assertIn('if (!ask_player_name(chosen, 16)) break;', loop)
        self.assertIn('set_env(L"MAG_PLAYER_SET_NAME", chosen);', loop)
        # The chosen name never reaches the engine as a first-run option.
        self.assertIn('L"MAG_PLAYER_SET_NAME"};', source)

    def test_supervisor_links_the_prompt_libraries(self):
        import inspect
        compile_source = inspect.getsource(windows._compile)
        for library in ('user32.lib', 'gdi32.lib', 'shell32.lib'):  # windows, the dialog font, the command line
            self.assertIn(library, compile_source)


if __name__ == '__main__':
    unittest.main()
