"""The opt-in native Edge geometry job: synthetic specification vectors for the core, and the
engine shim against a stand-in runtime. No title data; see docs/native-edgegeom.md."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'src/native_edgegeom'
FIXTURES = ROOT / 'tests/fixtures'


def compiler():
    return shutil.which('clang++')


def runtime_checkout():
    for candidate in (os.environ.get('MAG_TEST_SDK'), ROOT / 'local/ps3recomp-runtime'):
        if candidate and (Path(candidate) / 'runtime/spu/spu_context.h').is_file():
            return Path(candidate)
    return None


def build(binary, sources, includes):
    flags = ['-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-ffp-contract=off']
    if sys.platform == 'win32':
        flags += ['-D_CRT_SECURE_NO_WARNINGS']
    subprocess.run([compiler(), *flags, *('-I' + str(i) for i in includes), *map(str, sources), '-o', str(binary)],
                   check=True, capture_output=True, text=True)


@unittest.skipUnless(compiler(), 'clang++ required')
class NativeEdgeGeomCore(unittest.TestCase):
    def test_specification_vectors(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / ('core.exe' if sys.platform == 'win32' else 'core')
            build(binary, [SOURCES / 'edgegeom_core.cpp', FIXTURES / 'native_edgegeom_core.cpp'], [SOURCES, FIXTURES])
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS native edgegeom core', run.stdout)


@unittest.skipUnless(compiler() and runtime_checkout(), 'clang++ and a ps3recomp runtime checkout required')
class NativeEdgeGeomShim(unittest.TestCase):
    def test_integration_contract(self):
        sdk = runtime_checkout()
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / ('shim.exe' if sys.platform == 'win32' else 'shim')
            build(binary, [ROOT / 'src/mag_extension.cpp', SOURCES / 'edgegeom_core.cpp', SOURCES / 'edgegeom_runtime.cpp',
                           FIXTURES / 'native_edgegeom_shim.cpp'],
                  [ROOT / 'src', SOURCES, FIXTURES, sdk / 'runtime/spu', sdk / 'runtime', sdk])
            env = {k: v for k, v in os.environ.items() if not k.startswith('MAG_NATIVE_EDGEGEOM')}
            off = subprocess.run([str(binary), 'disabled'], capture_output=True, text=True, env=env)
            self.assertEqual(off.returncode, 0, off.stdout + off.stderr)
            self.assertIn('PASS native edgegeom shim disabled', off.stdout)
            self.assertNotIn('native-edgegeom', off.stderr)
            on = subprocess.run([str(binary)], capture_output=True, text=True, env=dict(env, MAG_NATIVE_EDGEGEOM='1'))
            self.assertEqual(on.returncode, 0, on.stdout + on.stderr)
            self.assertIn('PASS native edgegeom shim', on.stdout)


class NativeEdgeGeomBuild(unittest.TestCase):
    def test_off_by_default_and_needs_the_lift_step(self):
        module = (ROOT / 'cmake/MagNativeEdgeGeom.cmake').read_text()
        self.assertRegex(module, r'option\(MAG_NATIVE_EDGEGEOM "[^"]*" OFF\)')
        self.assertIn('payload_override_hooks', module)
        self.assertIn('native-edgegeom-sources=', module)
        self.assertIn('target_compile_definitions(${target} PRIVATE MAG_HAS_NATIVE_EDGEGEOM=1)', module)
        self.assertLess(module.index('if(_missing)'), module.index('MAG_HAS_NATIVE_EDGEGEOM=1'))
        top = (ROOT / 'CMakeLists.txt').read_text()
        self.assertEqual(len(re.findall(r'mag_enable_native_edgegeom\(', top)), 1)
        self.assertIn('${MAG_NATIVE_EDGEGEOM_RECEIPT_EXTRA}', top)

    def test_residency_record_is_the_lifted_code_span(self):
        # The shim's identity check expects what the streamed-job lift registers: the module at
        # LS 0x4000 with a span of its lifted code (shorter than the module image), not the
        # image size. A span equal to the image size would hide this mismatch.
        lift = (ROOT / 'tools/wws_job_lift.py').read_text()
        self.assertIn('LOAD_BASE = 0x4000', lift)
        self.assertIn("span = m['code_end'] - LOAD_BASE", lift)
        shim = (SOURCES / 'edgegeom_runtime.cpp').read_text()
        self.assertIn('spu_lifted_lookup(c, kEntry)', shim)
        self.assertNotIn('m.size == kModuleImage', shim)

    def test_no_clock_read_without_counters(self):
        # Every invocation without MAG_NATIVE_EDGEGEOM_STATS must stay clock-free: the one clock
        # read sits in clock_ns(), and each per-invocation call is guarded by the cached flag.
        shim = (SOURCES / 'edgegeom_runtime.cpp').read_text()
        self.assertEqual(shim.count('steady_clock::now'), 1)
        body = shim[shim.index('bool run(spu_context* c)'):shim.index('bool payload_override(')]
        calls = [m.start() for m in re.finditer(r'clock_ns\(\)', body)]
        self.assertTrue(calls)
        for at in calls:
            self.assertIn('stats ?', body[max(0, at - 40):at], body[max(0, at - 80):at + 20])
        self.assertIn('const bool stats = [] {', shim)

    def test_runtime_switch_is_exact(self):
        shim = (SOURCES / 'edgegeom_runtime.cpp').read_text()
        self.assertIn('getenv("MAG_NATIVE_EDGEGEOM")', shim)
        self.assertIn('!std::strcmp(e, "1")', shim)


if __name__ == '__main__':
    unittest.main()
