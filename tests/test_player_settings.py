"""Exercise native settings without game files or a guest process."""
import pathlib
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def debug_object(folder):
    """src/player_settings_debug.cpp compiled for linking with the macOS panel."""
    target = pathlib.Path(folder) / 'debug.o'
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-c', str(ROOT / 'src/player_settings_debug.cpp'), '-I' + str(ROOT / 'src'),
                    '-Wall', '-Wextra', '-Werror', '-o', str(target)], check=True, capture_output=True, timeout=60)
    return target


@unittest.skipUnless(platform.system() == 'Darwin', 'native macOS settings')
class PlayerSettingsTests(unittest.TestCase):
    def test_debug_page_switch_and_logs_folder(self):
        """The C interface the macOS panel's Debug page uses (src/player_settings_debug.h), headless."""
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            source = root / 'debug.m'
            source.write_text(r"""
#import <Foundation/Foundation.h>
#include "player_settings_debug.h"
#include "player_settings_text.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) { @autoreleasepool {
    assert(argc == 2);
    NSString *folder = [NSString stringWithUTF8String:argv[1]];
    NSString *settings = [folder stringByAppendingPathComponent:@"settings.json"];
    NSString *trace = [folder stringByAppendingPathComponent:@MAG_CONTROLS_TRACE_FILE];
    assert(!mag_settings_debug_switch("controls_trace", settings.fileSystemRepresentation));
    assert(mag_settings_set_debug_switch("controls_trace", settings.fileSystemRepresentation, 1));
    assert([NSFileManager.defaultManager fileExistsAtPath:trace] && mag_settings_debug_switch("controls_trace", settings.fileSystemRepresentation));
    assert(mag_settings_set_debug_switch("controls_trace", settings.fileSystemRepresentation, 0) && ![NSFileManager.defaultManager fileExistsAtPath:trace]);
    char logs[4096];
    unsetenv("MAG_PLAYER_LOGS_ROOT"); assert(!mag_settings_logs_folder(logs, sizeof logs));
    setenv("MAG_PLAYER_LOGS_ROOT", "/tmp/mag logs", 1); assert(mag_settings_logs_folder(logs, sizeof logs) && !strcmp(logs, "/tmp/mag logs"));
    assert(!mag_settings_logs_folder(logs, 4));  // does not fit
    puts("debug page interface passed");
} }
""")
            binary = root / 'debug-test'
            result = subprocess.run(['xcrun', 'clang++', '-std=c++20', '-c', str(ROOT / 'src/player_settings_debug.cpp'), '-I' + str(ROOT / 'src'),
                                     '-Wall', '-Wextra', '-Werror', '-o', str(root / 'debug.o')], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'), str(source),
                                     str(root / 'debug.o'), '-lc++', '-framework', 'Foundation', '-o', str(binary)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary), tmp], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('debug page interface passed', result.stdout)

    def test_validation_atomic_save_and_corrupt_file_recovery(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'settings-test'
            result = subprocess.run([
                'xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'src'), str(ROOT / 'tests/player_settings_store.m'),
                str(ROOT / 'src/player_settings_store.m'), '-framework', 'Foundation',
                '-o', str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary), tmp], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('settings validation and persistence passed', result.stdout)

    def test_native_panel_apply_cancel_and_display_rollback(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'window-test'
            debug = debug_object(tmp)  # the Debug page's interface (src/player_settings_debug.h)
            result = subprocess.run([
                'xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-I' + str(ROOT / 'src'),
                str(ROOT / 'tests/player_settings_window.m'),
                str(ROOT / 'src/player_settings_store.m'), str(ROOT / 'src/player_settings_macos.m'), str(debug), '-lc++',
                '-framework', 'AppKit', '-framework', 'QuartzCore', '-o', str(binary)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary), tmp], capture_output=True, text=True, timeout=25)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('native panel apply/cancel/keep/revert/timeout passed', result.stdout)

    def test_native_panel_reports_rows(self):
        # Headless: the window is never ordered front and the application may not activate.
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'reports-test'
            debug = debug_object(tmp)  # the Debug page's interface (src/player_settings_debug.h)
            result = subprocess.run([
                'xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-I' + str(ROOT / 'src'),
                str(ROOT / 'tests/player_settings_reports.m'),
                str(ROOT / 'src/player_settings_store.m'), str(ROOT / 'src/player_settings_macos.m'), str(debug), '-lc++',
                '-framework', 'AppKit', '-framework', 'QuartzCore', '-o', str(binary)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary), tmp], capture_output=True, text=True, timeout=25)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('native panel reports rows passed', result.stdout)

    def _build_runtime_fixture(self,root):
        (root/'libs/input').mkdir(parents=True);(root/'SDL2').mkdir()
        (root/'libs/input/pad_macos.h').write_text('''#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint16_t buttons; uint8_t analog_lx,analog_ly,analog_rx,analog_ry; } PS3RecompMacOSPadState;
PS3RecompMacOSPadState ps3recomp_pad_macos_state(void);
uint16_t ps3recomp_pad_macos_buttons(void);
#ifdef __cplusplus
}
#endif
''')
        (root/'SDL2/SDL.h').write_text('''#include <stdint.h>
typedef uint8_t Uint8; typedef int16_t Sint16; typedef struct SDL_GameController SDL_GameController;
typedef enum { SDL_CONTROLLER_BUTTON_A } SDL_GameControllerButton;
typedef enum { SDL_CONTROLLER_AXIS_LEFTX } SDL_GameControllerAxis;
typedef enum { SDL_FALSE,SDL_TRUE } SDL_bool;
#ifdef __cplusplus
extern "C" {
#endif
Uint8 SDL_GameControllerGetButton(SDL_GameController*,SDL_GameControllerButton);
Sint16 SDL_GameControllerGetAxis(SDL_GameController*,SDL_GameControllerAxis);
void SDL_GameControllerUpdate(void);
SDL_GameController* SDL_GameControllerOpen(int);
void SDL_GameControllerClose(SDL_GameController*);
SDL_bool SDL_GameControllerGetAttached(SDL_GameController*);
#ifdef __cplusplus
}
#endif
''')
        (root/'sdata_decrypt.h').write_text('#include <stddef.h>\n#include <stdint.h>\nstatic inline uint8_t* sdata_decrypt(const uint8_t*,size_t,size_t*) { return NULL; }\n')
        sdk=root/'sdk';(sdk/'runtime/ppu').mkdir(parents=True)
        shutil.copytree(root/'libs',sdk/'libs');shutil.copytree(root/'SDL2',sdk/'SDL2')
        (sdk/'libs/video').mkdir()
        shutil.copyfile(ROOT/'tests/player_render_backend_fixture.h',sdk/'libs/video/rsx_draw_engine.h')
        (sdk/'libs/video/rsx_metal_backend.m').write_text('''#include "rsx_draw_engine.h"
void fixture_video_unregister(void) { rsx_draw_engine_set_backend(0); }
''')
        (sdk/'libs/video/rsx_draw_engine.c').write_text('''#include "rsx_draw_engine.h"
void rsx_draw_engine_set_backend(const rsx_draw_backend *backend) { (void)backend; }
''')
        shutil.copyfile(root/'sdata_decrypt.h',sdk/'runtime/ppu/sdata_decrypt.h')
        (sdk/'libs/input/cellPad.c').write_text('''#include <SDL2/SDL.h>
int fixture_input_button(void) { return SDL_GameControllerGetButton(0,SDL_CONTROLLER_BUTTON_A); }
int fixture_input_axis(void) { return SDL_GameControllerGetAxis(0,SDL_CONTROLLER_AXIS_LEFTX); }
''')
        (sdk/'libs/input/pad_macos.m').write_text('''#include "pad_macos.h"
int fixture_pad_held = 1;
PS3RecompMacOSPadState ps3recomp_pad_macos_state(void) {
    return fixture_pad_held ? (PS3RecompMacOSPadState){7,10,20,30,40} : (PS3RecompMacOSPadState){0,128,128,128,128};
}
uint16_t ps3recomp_pad_macos_buttons(void) { return fixture_pad_held ? 7 : 0; }
''')
        (sdk/'CMakeLists.txt').write_text('''add_library(ps3recomp_runtime STATIC libs/input/cellPad.c libs/input/pad_macos.m libs/video/rsx_metal_backend.m libs/video/rsx_draw_engine.c)
target_include_directories(ps3recomp_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
''')
        (root/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(MAGSettingsFixture LANGUAGES C CXX OBJC OBJCXX)
set(CMAKE_CXX_STANDARD 20)
add_subdirectory(sdk)
add_executable(runtime-test "{ROOT}/tests/player_settings_runtime.mm")
target_include_directories(runtime-test PRIVATE "{ROOT}/src")
target_compile_definitions(runtime-test PRIVATE MAG_CMAKE_INPUT_FIXTURE=1)
target_compile_options(runtime-test PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(runtime-test PRIVATE ps3recomp_runtime)
include("{ROOT}/cmake/MagPlayerSettings.cmake")
mag_enable_player_settings(runtime-test "${{CMAKE_CURRENT_SOURCE_DIR}}/sdk")
''')
        for command in (['cmake','-S',str(root),'-B',str(root/'build')],
                        ['cmake','--build',str(root/'build'),'--parallel','2']):
            result=subprocess.run(command,capture_output=True,text=True,timeout=45)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        return root/'build/runtime-test'

    def test_packaged_controls_fixture_builds_without_a_window(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertTrue(self._build_runtime_fixture(pathlib.Path(tmp)).is_file())

    def test_packaged_opt_in_snapshot_menu_and_input_ownership(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary=self._build_runtime_fixture(pathlib.Path(tmp))
            result=subprocess.run([str(binary),tmp],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('runtime snapshot, settings menu and input ownership passed',result.stdout)
