#!/usr/bin/env python3
"""cmake/MagWindowsCTls.cmake: _MSC_VER/_MSC_FULL_VER from the compiler probe or a three-part
simulated version, never invented from a two-part one; the apply step's options; the opt-out."""
import os, pathlib, shutil, subprocess, sys, tempfile, unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


@unittest.skipIf(shutil.which('cmake') is None, 'cmake not found')
@unittest.skipIf(os.name == 'nt', 'the stand-in compiler is a POSIX shell script')
class WindowsCTls(unittest.TestCase):
    def test_script(self):
        with tempfile.TemporaryDirectory(prefix='mag-c-tls-') as work:
            r = subprocess.run(['cmake', f'-DROOT={ROOT}', f'-DWORK={work}', '-P',
                                str(ROOT / 'tests/cmake/test_windows_c_tls.cmake')],
                               capture_output=True, text=True, timeout=60)
            out = r.stdout + r.stderr
            self.assertEqual(r.returncode, 0, out)
            self.assertNotIn('CMake Error', out, out)
            self.assertIn('-- done', out, out)
            self.assertEqual(out.count('-- PASS '), 5, out)


if __name__ == '__main__':
    unittest.main()
