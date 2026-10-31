"""Opt-in synthetic headless GPU check using an ordinary built SDK.

Set MAG_TEST_METAL_SDK_DIR to the SDK source directory and
MAG_TEST_METAL_RUNTIME_ARCHIVE to its built runtime archive. Supply its shader
translation linker flags in MAG_TEST_METAL_LINK_FLAGS (for example the output
of the SDK dependency packages' pkg-config command). This test opens no window
and never loads or runs a game. A missing GPU is a failure when opted in.
"""
import os
import pathlib
import platform
import shlex
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SDK = os.environ.get('MAG_TEST_METAL_SDK_DIR')
ARCHIVE = os.environ.get('MAG_TEST_METAL_RUNTIME_ARCHIVE')


@unittest.skipUnless(platform.system() == 'Darwin' and SDK and ARCHIVE,
                     'opt-in macOS Metal test needs SDK source and runtime archive')
class PlayerRenderScaleMetalTests(unittest.TestCase):
    def test_real_gpu_resolution_detail_and_logical_readback(self):
        sdk = pathlib.Path(SDK)
        with tempfile.TemporaryDirectory(prefix='mag-render-scale-metal-') as tmp:
            output = pathlib.Path(tmp)
            gpu = output / 'gpu.o'
            adapter = output / 'adapter.o'
            binary = output / 'test'
            includes = ['-I' + str(ROOT / 'src'), '-I' + str(sdk), '-I' + str(sdk / 'include'),
                        '-I' + str(sdk / 'libs/video')]
            commands = [
                ['xcrun', 'clang', '-fobjc-arc', '-O1', *includes, '-c',
                 str(ROOT / 'tests/player_render_scale_metal.m'), '-o', str(gpu)],
                ['xcrun', 'clang++', '-std=c++20', '-O1', *includes, '-c',
                 str(ROOT / 'src/player_render_scale.cpp'), '-o', str(adapter)],
                ['xcrun', 'clang++', str(gpu), str(adapter), ARCHIVE,
                 '-Wl,-dead_strip', '-framework', 'Metal', '-framework', 'Cocoa',
                 '-framework', 'QuartzCore',
                 *shlex.split(os.environ.get('MAG_TEST_METAL_LINK_FLAGS', '')),
                 '-o', str(binary)],
            ]
            for command in commands:
                result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # Each process starts with fresh Metal and adapter state.
            for percent in ('100', '150', '200', '300'):
                with self.subTest(percent=percent):
                    result = subprocess.run([str(binary), percent], capture_output=True,
                                            text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn('0 failures', result.stdout)


if __name__ == '__main__':
    unittest.main()
