"""The Windows shader and pipeline cache defaults (src/mag_shader_cache.h): where each cache goes,
the player's own PS3RECOMP_SPIRV_CACHE or PS3RECOMP_VK_PIPELINE_CACHE winning, the
MAG_SHADER_CACHE=0 and MAG_PIPELINE_CACHE=0 switches, and a missing or uncreatable folder leaving
that cache off. Synthetic: no game files, no GPU."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPILER = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')


class ShaderCacheTests(unittest.TestCase):
    @unittest.skipUnless(COMPILER, 'C++ compiler')
    def test_choice(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'shader_cache'
            build = subprocess.run([COMPILER, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                                    str(ROOT / 'tests/shader_cache.cpp'), '-o', str(binary)],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('[shader_cache] PASS', result.stdout)

    def test_windows_vulkan_main_applies_it(self):
        main = (ROOT / 'src/main.cpp').read_text()
        block = main[main.index('#if defined(_WIN32) && defined(PS3RECOMP_RSX_VULKAN)\n    /* Windows Vulkan builds'):]
        block = block[:block.index('#endif')]
        self.assertIn('mag::choose_shader_cache(', block)
        self.assertIn('getenv("PS3RECOMP_SPIRV_CACHE"), getenv("MAG_SHADER_CACHE"), getenv("LOCALAPPDATA")', block)
        self.assertIn('_putenv_s("PS3RECOMP_SPIRV_CACHE", cache.dir.c_str())', block)
        self.assertIn('mag::choose_pipeline_cache(', block)
        self.assertIn('getenv("PS3RECOMP_VK_PIPELINE_CACHE"), getenv("MAG_PIPELINE_CACHE"), getenv("LOCALAPPDATA")',
                      block)
        self.assertIn('_putenv_s("PS3RECOMP_VK_PIPELINE_CACHE", pipelines.dir.c_str())', block)
        self.assertIn('printf("[mag] %s\\n", pipelines.why.c_str())', block)
        # Before the guest starts (and so before the first shader is translated or pipeline built).
        self.assertLess(main.index('mag::choose_shader_cache('), main.index('ppu_load_elf(argv[1])'))
        self.assertLess(main.index('mag::choose_pipeline_cache('), main.index('ppu_load_elf(argv[1])'))

    def test_only_windows_vulkan_sets_them(self):
        # Linux and macOS stay unchanged: every use is inside the Windows Vulkan block.
        main = (ROOT / 'src/main.cpp').read_text()
        start = main.index('#if defined(_WIN32) && defined(PS3RECOMP_RSX_VULKAN)\n    /* Windows Vulkan builds')
        end = main.index('#endif', start)
        for needle in ('mag::choose_shader_cache(', 'mag::choose_pipeline_cache(', 'MAG_PIPELINE_CACHE'):
            self.assertEqual(main.count(needle), main[start:end].count(needle), needle)


if __name__ == '__main__':
    unittest.main()
