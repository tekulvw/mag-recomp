"""Opt-in synthetic headless GPU check of the rendering-resolution adapter on the Vulkan backend.

Set MAG_TEST_VULKAN_SDK_DIR to a runtime checkout (the pinned one, or newer). Needs the Vulkan and
SDL2 development files (pkg-config), glslang (GLSLANG_INCLUDE/GLSLANG_LIB, default the system
paths) and a Vulkan device; Mesa's lavapipe (VK_ICD_FILENAMES=.../lvp_icd.json) is enough. It
builds tests/player_render_scale_vulkan.c against the SDK's own Vulkan backend with the draw
engine registration renamed to the adapter's, as cmake/MagRenderScale.cmake does, and runs it at
every scale under the Khronos validation layer when that layer is installed. It opens no window
and never loads or runs a game. A missing device is a failure when opted in.
"""
import os
import pathlib
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SDK = os.environ.get('MAG_TEST_VULKAN_SDK_DIR')
SDK_SOURCES = ['libs/video/' + name + '.c' for name in (
    'rsx_draw_engine', 'rsx_fifo_busy', 'rsx_commands', 'rsx_dispatch', 'rsx_texture_layout', 'rsx_vertex_compact',
    'rsx_vertex_fetch', 'rsx_vp_decompiler', 'rsx_fp_decompiler')] + [
    'runtime/diagnostics/critical_trace.c', 'runtime/diagnostics/render_ablation.c']
VALIDATION_ERRORS = ('VUID-', 'SYNC-HAZARD', 'Validation Error', 'THREADING ERROR')


def pkg_config(*args):
    return shlex.split(subprocess.check_output(['pkg-config', *args], text=True))


@unittest.skipUnless(SDK and shutil.which('pkg-config'), 'opt-in Vulkan test needs MAG_TEST_VULKAN_SDK_DIR')
class PlayerRenderScaleVulkanTests(unittest.TestCase):
    def test_real_device_resolution_detail_logical_readback_and_present(self):
        sdk = pathlib.Path(SDK)
        self.assertTrue((sdk / 'libs/video/rsx_vulkan_backend.c').is_file(), 'not a runtime checkout: ' + SDK)
        self.assertEqual(subprocess.run(['pkg-config', '--exists', 'vulkan', 'sdl2']).returncode, 0,
                         'Vulkan/SDL2 development files')
        cc, cxx = os.environ.get('CC', 'cc'), os.environ.get('CXX', 'c++')
        flags = pkg_config('--cflags', 'vulkan', 'sdl2')
        glslang_include, glslang_lib = os.environ.get('GLSLANG_INCLUDE'), os.environ.get('GLSLANG_LIB')
        glslang = ((['-L' + glslang_lib, '-Wl,-rpath,' + glslang_lib] if glslang_lib else []) +
                   ['-lglslang', '-lglslang-default-resource-limits'])
        common = ['-O1', '-g', '-ffunction-sections', '-fdata-sections', '-I' + str(sdk / 'include'),
                  '-I' + str(sdk / 'libs/video'), '-I' + str(sdk), '-I' + str(ROOT / 'src'), *flags]
        with tempfile.TemporaryDirectory(prefix='mag-render-scale-vulkan-') as tmp:
            out = pathlib.Path(tmp)
            objects, commands = [], []
            for index, source in enumerate(SDK_SOURCES):
                objects.append(out / f'sdk{index}.o')
                commands.append([cc, '-std=gnu17', *common, '-c', str(sdk / source), '-o', str(objects[-1])])
            objects.append(out / 'spirv.o')
            commands.append([cxx, '-std=c++17', *common, '-DPS3RECOMP_HAVE_SPIRV_TRANSLATION=1',
                             *(['-I' + glslang_include] if glslang_include else []),
                             '-c', str(sdk / 'libs/video/rsx_shader_spirv.cpp'), '-o', str(objects[-1])])
            # The backend under test, its engine registration renamed to the adapter's.
            objects.append(out / 'fixture.o')
            commands.append([cc, '-std=gnu17', *common, '-Drsx_draw_engine_set_backend=mag_player_set_render_backend',
                             '-c', str(ROOT / 'tests/player_render_scale_vulkan.c'), '-o', str(objects[-1])])
            objects.append(out / 'adapter.o')
            commands.append([cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', *common,
                             '-DMAG_PLAYER_RENDER_SCALE_VULKAN=1',
                             '-c', str(ROOT / 'src/player_render_scale.cpp'), '-o', str(objects[-1])])
            for command in commands:
                result = subprocess.run(command, capture_output=True, text=True, timeout=600)
                self.assertEqual(result.returncode, 0, ' '.join(command) + '\n' + result.stderr)
            binary = out / 'test'
            link = [cxx, *map(str, objects), '-Wl,--gc-sections', '-pthread', '-ldl', '-lm',
                    *pkg_config('--libs', 'sdl2'), *glslang, '-o', str(binary)]
            result = subprocess.run(link, capture_output=True, text=True, timeout=600)
            if result.returncode:
                # A static glslang (Ubuntu's glslang-dev) needs SPIRV-Tools spelled out after it.
                retry = link[:-2] + ['-lSPIRV', '-lSPIRV-Tools-opt', '-lSPIRV-Tools'] + link[-2:]
                result = subprocess.run(retry, capture_output=True, text=True, timeout=600)
            self.assertEqual(result.returncode, 0, result.stderr)
            env = dict(os.environ)
            layers = pathlib.Path('/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json')
            if layers.exists() or env.get('VK_LAYER_PATH'):
                settings = out / 'vk_layer_settings.txt'
                settings.write_text('khronos_validation.validate_core = true\n'
                                    'khronos_validation.validate_sync = true\n')
                env.update(VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation', VK_LAYER_SETTINGS_PATH=str(settings))
            for percent in ('100', '150', '200', '300'):
                with self.subTest(percent=percent):
                    result = subprocess.run([str(binary), percent], capture_output=True, text=True,
                                            timeout=600, env=env)
                    text = result.stdout + result.stderr
                    print(result.stdout.strip())
                    self.assertEqual(result.returncode, 0, text)
                    self.assertIn('0 failures', result.stdout)
                    self.assertFalse(any(e in text for e in VALIDATION_ERRORS), text)
                    if percent == '100':
                        self.assertNotIn('GPU render targets at', result.stderr)
                        self.assertNotIn('present scale', result.stderr)
                    else:
                        self.assertIn(f'GPU render targets at {percent}%', result.stderr)
                        self.assertIn(f'present scale {percent}%: display image', result.stderr)


if __name__ == '__main__':
    unittest.main()
