"""Synthetic renderer boundary checks; no SDK checkout or game assets needed."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT=pathlib.Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++'), 'C++ compiler')
class PlayerRenderScaleTests(unittest.TestCase):
    def test_scaled_render_resources_preserve_guest_coordinates_and_readbacks(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp);header=root/'libs/video/rsx_draw_engine.h'
            header.parent.mkdir(parents=True)
            shutil.copyfile(ROOT/'tests/player_render_backend_fixture.h',header)
            sources=[ROOT/'tests/player_render_scale.cpp']
            adapter=ROOT/'src/player_render_scale.cpp'
            if adapter.exists():sources.append(adapter)
            binary=root/'render-scale'
            result=subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror',
                '-I'+str(root),'-I'+str(ROOT/'src'),*map(str,sources),'-o',str(binary)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr)
            for value in ('100','150','200','300'):
                result=subprocess.run([str(binary),value],capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary),'oversize'],capture_output=True,text=True,timeout=10)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('render target',result.stderr)

    def test_public_runtime_without_optional_target_callbacks_still_compiles(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp);header=root/'libs/video/rsx_draw_engine.h'
            header.parent.mkdir(parents=True)
            text=(ROOT/'tests/player_render_backend_fixture.h').read_text()
            names=('depth_as_color','clear_color_rect','clear_depth_stencil_rect')
            header.write_text('\n'.join(line for line in text.splitlines() if not any('(*'+name+')' in line for name in names))+'\n')
            result=subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror',
                '-I'+str(root),'-I'+str(ROOT/'src'),'-fsyntax-only',str(ROOT/'src/player_render_scale.cpp')],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_adapter_compiles_against_supplied_public_runtime(self):
        sdk=os.environ.get('MAG_TEST_SDK')
        if not sdk:
            self.skipTest('Set MAG_TEST_SDK to the public runtime checkout')
        root=pathlib.Path(sdk)
        if not (root/'libs/video/rsx_draw_engine.h').is_file():
            self.skipTest('Public runtime checkout is unavailable')
        result=subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror',
            '-I'+str(root),'-I'+str(root/'include'),'-I'+str(ROOT/'src'),
            '-fsyntax-only',str(ROOT/'src/player_render_scale.cpp')],capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stderr)
