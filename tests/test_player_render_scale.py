"""Synthetic renderer boundary checks; no SDK checkout or game assets needed."""
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
