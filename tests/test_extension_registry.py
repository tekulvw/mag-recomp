"""The runtime extension registry: empty by default, refuses double registration, carries scratch."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++'), 'clang++ required')
class ExtensionRegistryTests(unittest.TestCase):
    def test_registry_fixture(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'registry'
            subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                            str(ROOT / 'src/mag_extension.cpp'), str(ROOT / 'tests/fixtures/extension_registry.cpp'),
                            '-o', str(binary)], check=True, capture_output=True, text=True)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('PASS extension registry', run.stdout)


class TraceCallSiteTests(unittest.TestCase):
    """The trace scope only compiles inside an engine build, so check the call sites' placement in source."""
    def test_extension_call_sites_preserve_errno(self):
        source = (ROOT / 'src/mag_graphics_trace.cpp').read_text()
        ctor = source[source.index('MagGraphicsTraceScope::MagGraphicsTraceScope('):source.index('MagGraphicsTraceScope::~MagGraphicsTraceScope(')]
        dtor = source[source.index('MagGraphicsTraceScope::~MagGraphicsTraceScope('):]
        save, call, restore = ctor.index('const int ext_errno=errno;'), ctor.index('enter(c,f,'), ctor.index('errno=ext_errno;')
        self.assertLess(save, call); self.assertLess(call, restore)
        save, call, restore = dtor.index('const int caller_errno=errno;'), dtor.index('leave(ctx,'), dtor.index('errno=caller_errno;')
        self.assertLess(save, call); self.assertLess(call, restore)


if __name__ == '__main__':
    unittest.main()
