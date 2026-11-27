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

    def test_vulkan_registration_asks_for_the_enlarged_present_and_keeps_the_scaling_decisions(self):
        """The adapter as the Vulkan backend's registration builds it (MAG_PLAYER_RENDER_SCALE_VULKAN)."""
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp);header=root/'libs/video/rsx_draw_engine.h'
            header.parent.mkdir(parents=True)
            shutil.copyfile(ROOT/'tests/player_render_backend_fixture.h',header)
            binary=root/'render-scale-vulkan'
            result=subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-DMAG_PLAYER_RENDER_SCALE_VULKAN=1',
                '-I'+str(root),'-I'+str(ROOT/'src'),str(ROOT/'tests/player_render_scale_vulkan_boundary.cpp'),
                str(ROOT/'src/player_render_scale.cpp'),'-o',str(binary)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr)
            for value in ('100','150','200','300'):
                result=subprocess.run([str(binary),value],capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(binary),'refused'],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,86,result.stdout+result.stderr)
            self.assertIn('the Vulkan backend refused the rendering resolution',result.stderr)

    @unittest.skipUnless(shutil.which('cmake'), 'CMake')
    def test_cmake_wraps_the_vulkan_backend_registration(self):
        """cmake/MagRenderScale.cmake in its Vulkan mode: the backend source's registration reaches the
        adapter, which asks that backend for the enlarged present; other sources register unchanged."""
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp);sdk=root/'sdk';(sdk/'libs/video').mkdir(parents=True)
            shutil.copyfile(ROOT/'tests/player_render_backend_fixture.h',sdk/'libs/video/rsx_draw_engine.h')
            (sdk/'libs/video/rsx_vulkan_backend.c').write_text('''#include "rsx_draw_engine.h"
static u32 create(void*u,u32 w,u32 h){(void)u;return w*10000u+h;}
static void release(void*u,u32 h){(void)u;(void)h;}
static u32 color(void*u,rsx_be_format f,u32 w,u32 h,const void*s,u32 p){(void)u;(void)f;(void)s;(void)p;return w*10000u+h;}
static u32 pipe(void*u,const char*v,const char*f,const rsx_be_render_state*r,const rsx_vertex_layout_plan*l,u32 s,rsx_be_format t,u32 n){
    (void)u;(void)v;(void)f;(void)r;(void)l;(void)s;(void)t;(void)n;return 1;}
static void bind(void*u,const u32*s,u32 n,u32 d){(void)u;(void)s;(void)n;(void)d;}
static void vp(void*u,float x,float y,float w,float h){(void)u;(void)x;(void)y;(void)w;(void)h;}
static void sc(void*u,u32 x,u32 y,u32 w,u32 h){(void)u;(void)x;(void)y;(void)w;(void)h;}
static void rb(void*u,u32 s,u32 x,u32 y,u32 w,u32 h,void*o,u32 p){(void)u;(void)s;(void)x;(void)y;(void)w;(void)h;(void)o;(void)p;}
static int drb(void*u,u32 s,u32 x,u32 y,u32 w,u32 h,float*o,u32 p){(void)u;(void)s;(void)x;(void)y;(void)w;(void)h;(void)o;(void)p;return 0;}
static rsx_draw_backend table;
u32 fixture_present_percent;
int rsx_vulkan_backend_set_present_scale(u32 percent){fixture_present_percent=percent;return 0;}
const rsx_draw_backend* fixture_vulkan_table(void){return &table;}
void fixture_vulkan_init(void){
    table.depth_target_create=create;table.color_target_release=release;table.depth_target_release=release;
    table.texture_release=release;table.color_target_create=color;table.pipeline_create=pipe;table.bind_targets=bind;
    table.set_viewport=vp;table.set_scissor=sc;table.readback=rb;table.depth_readback=drb;
    rsx_draw_engine_set_backend(&table);
}
''')
            (sdk/'libs/video/rsx_draw_engine.c').write_text('''#include "rsx_draw_engine.h"
const rsx_draw_backend* fixture_registered;
void rsx_draw_engine_set_backend(const rsx_draw_backend *backend) { fixture_registered = backend; }
void fixture_other_init(const rsx_draw_backend *backend) { rsx_draw_engine_set_backend(backend); }
''')
            (sdk/'CMakeLists.txt').write_text('''add_library(ps3recomp_runtime STATIC libs/video/rsx_vulkan_backend.c libs/video/rsx_draw_engine.c)
target_include_directories(ps3recomp_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
''')
            (root/'main.cpp').write_text('''#include "libs/video/rsx_draw_engine.h"
#include <cstdio>
#include <cstdlib>
extern "C" { extern const rsx_draw_backend* fixture_registered; extern u32 fixture_present_percent;
const rsx_draw_backend* fixture_vulkan_table(void); void fixture_vulkan_init(void);
void fixture_other_init(const rsx_draw_backend*); }
int main(int argc,char**argv){
    if(argc!=2)return 2;
    setenv("MAG_PLAYER_RENDER_SCALE",argv[1],1);
    fixture_vulkan_init();
    const bool scaled=std::atoi(argv[1])!=100;
    if((fixture_registered!=fixture_vulkan_table())!=scaled)return 3;
    if(fixture_present_percent!=(scaled?(u32)std::atoi(argv[1]):0u))return 4;
    if(scaled&&fixture_registered->depth_target_create(nullptr,4,2)!=(4u*std::atoi(argv[1])/100)*10000u+2u*std::atoi(argv[1])/100)return 5;
    fixture_other_init(fixture_vulkan_table());
    if(fixture_registered!=fixture_vulkan_table())return 6;
    std::puts("wired");return 0;
}
''')
            (root/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(MAGRenderScaleFixture LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
add_subdirectory(sdk)
add_executable(wiring "${{CMAKE_CURRENT_SOURCE_DIR}}/main.cpp")
target_link_libraries(wiring PRIVATE ps3recomp_runtime)
include("{ROOT}/cmake/MagRenderScale.cmake")
mag_enable_player_render_scale(wiring "${{CMAKE_CURRENT_SOURCE_DIR}}/sdk" vulkan)
''')
            for command in (['cmake','-S',str(root),'-B',str(root/'build')],['cmake','--build',str(root/'build')]):
                result=subprocess.run(command,capture_output=True,text=True,timeout=120)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            for value in ('100','150','200','300'):
                result=subprocess.run([str(root/'build/wiring'),value],capture_output=True,text=True,timeout=10)
                self.assertEqual((result.returncode,result.stdout.strip()),(0,'wired'),value+' '+result.stderr)

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
            'texture_reusable':'whether a guest-memory texture of its guest size takes new contents in place',
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
