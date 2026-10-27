"""MAG_EXTENSIONS: each listed .cmake file is included with MAG_TARGET set and may add receipt extras."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/cmake_extension'
EXTENSION = '''target_sources(${MAG_TARGET} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/extra.c")
target_compile_definitions(${MAG_TARGET} PRIVATE MAG_SAMPLE)
list(APPEND MAG_RECEIPT_EXTRA "sample=${CMAKE_CURRENT_LIST_DIR}/extra.c")
'''


@unittest.skipUnless(shutil.which('cmake') and shutil.which('ninja'), 'cmake and ninja required')
class CmakeExtensionTests(unittest.TestCase):
    def build(self, extensions):
        tmp = Path(tempfile.mkdtemp(prefix='mag-cmake-extension-'))
        self.addCleanup(shutil.rmtree, tmp)
        args = ['cmake', '-S', str(FIXTURE), '-B', str(tmp / 'build'), '-G', 'Ninja', f'-DMAG_REPO={ROOT}']
        if extensions:
            (tmp / 'ext').mkdir()
            (tmp / 'ext/extra.c').write_text('int mag_sample_extension(void) { return 42; }\n')
            (tmp / 'ext/sample.cmake').write_text(EXTENSION)
            args.append(f"-DMAG_EXTENSIONS={tmp / 'ext/sample.cmake'}")
        subprocess.run(args, check=True, capture_output=True, text=True)
        subprocess.run(['cmake', '--build', str(tmp / 'build')], check=True, capture_output=True, text=True)
        run = subprocess.run([str(tmp / 'build/fixture')], capture_output=True, text=True)
        return tmp, run, (tmp / 'build/receipt-extra.txt').read_text()

    def test_extension_adds_sources_definitions_and_receipt_extra(self):
        tmp, run, extras = self.build(True)
        self.assertEqual((run.returncode, run.stdout.strip()), (0, 'extension'))
        self.assertEqual(extras, f"sample={tmp / 'ext/extra.c'}")

    def test_no_extensions_changes_nothing(self):
        _, run, extras = self.build(False)
        self.assertEqual((run.returncode, run.stdout.strip(), extras), (0, 'no extension', ''))

    def test_missing_extension_file_fails_configure(self):
        result = subprocess.run(['cmake', '-S', str(FIXTURE), '-B', tempfile.mkdtemp(prefix='mag-cmake-missing-'), '-G', 'Ninja',
                                 f'-DMAG_REPO={ROOT}', '-DMAG_EXTENSIONS=/nonexistent/extension.cmake'], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('MAG_EXTENSIONS entry not found', result.stderr)


if __name__ == '__main__':
    unittest.main()
