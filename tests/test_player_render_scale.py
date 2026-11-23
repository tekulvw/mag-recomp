"""Synthetic renderer boundary checks; no SDK checkout or game assets needed."""
import os
import pathlib
import re
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

    def test_every_backend_callback_with_target_geometry_is_scaled_or_reviewed(self):
        """Which surfaces scale: every backend entry point that takes a pixel rectangle, an
        image size or a fragment program must be wrapped (scaled to the physical grid) or
        disabled by the adapter, unless it is listed here as guest-sized by design. A new SDK
        callback that passes through unreviewed would mix guest and physical sizes."""
        sdk=os.environ.get('MAG_TEST_SDK')
        if not sdk:
            self.skipTest('Set MAG_TEST_SDK to the public runtime checkout')
        header=pathlib.Path(sdk)/'libs/video/rsx_draw_engine.h'
        if not header.is_file():
            self.skipTest('Public runtime checkout is unavailable')
        text=header.read_text()
        body=text[text.index('typedef struct rsx_draw_backend {'):]
        body=re.sub(r'/\*.*?\*/','',body[:body.index('} rsx_draw_backend;')],flags=re.S)
        geometry=re.compile(r'\b(?:u32|float)\s+(?:x|y|w|h|width|height)\b')
        guest_sized={
            'init':'the output window size, independent of rendering resolution',
            'texture_create':'textures decoded from guest memory keep their guest size',
            'texture_upload':'rows of a guest-memory texture',
            'texture_create_3d':'volume textures decoded from guest memory',
            'texture_upload_3d':'slices of a guest-memory volume texture',
        }
        adapter=(ROOT/'src/player_render_scale.cpp').read_text()
        handled=set(re.findall(r'\b(?:scaled|target)\.(\w+)\s*=',adapter))
        unreviewed=[]
        for name,params in re.findall(r'\(\s*\*\s*(\w+)\s*\)\s*\(([^)]*)\)',body):
            if not (geometry.search(params) or 'ps_hlsl' in params):
                continue
            if name not in handled and name not in guest_sized:
                unreviewed.append(name)
        self.assertEqual(unreviewed,[],'backend callbacks pass guest geometry through unscaled')
        for name in guest_sized:
            self.assertNotIn(name,handled,name+' is listed as guest-sized but the adapter changes it')
