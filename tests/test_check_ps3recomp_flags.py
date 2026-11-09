"""Synthetic check: docs/ps3recomp-flags.md lists exactly the ps3recomp switches mag-recomp sets.

Fixture trees stand in for the repository and for a runtime checkout; no runtime, compiler or
game files are needed. The last test runs the check on this repository."""
import tempfile
import unittest
from pathlib import Path

from tools.check_ps3recomp_flags import ROOT, check, scan

DEFAULTS_H = '''inline int mag_apply_runtime_defaults() {
    struct Setting { const char* name; const char* value; };
    for (const auto& setting : {
             // SPU caches. Setting{"SPU_COMMENTED_OUT", "1"} is not a default.
             Setting{"SPU_DISPATCH_CACHE", "1"},
             Setting{"PS3RECOMP_RSX_DEFERRED_REPORT_CAPACITY", "4096"},
             Setting{"MAG_NATIVE_CONSUMER", "1"},
             Setting{"MAG_PRIVATE_SWITCH", "1"},
#if defined(__linux__)
             Setting{"PS3RECOMP_VK_BATCHED_ENGINE", "1"},
#endif
         }) { if (std::getenv(setting.name)) continue; }
    if (!getenv("PS3RECOMP_METAL_FPS")) setenv("PS3RECOMP_METAL_FPS", "1", 0);
    if (!getenv("RUST_MIN_STACK")) setenv("RUST_MIN_STACK", bytes, 0);
    return 0;
}
'''

SETTINGS_MM = '''void prepare(NSDictionary* settings) {
    setenv("PS3_NP_ONLINE_ID",[settings[@"player_name"] UTF8String],1);
    const char* read = getenv("PS3_READ_ONLY");
}
'''

PACKAGE_PY = '''MOVIE_PLAYBACK_ENV = {'PS3RECOMP_SAIL_TIMED_THX': '1'}
_SUPERVISOR = r\'\'\'
 NSMutableDictionary *env=[NSMutableDictionary dictionaryWithDictionary:@{@"PATH":@"/usr/bin:/bin",@"PS3_HLE_UNRESOLVED":@"halt",@"PS3_VFS_ROOT":vfs}];
 env[@"PS3_GAMEDATA_ROOT"]=gamedata;
 set_env(L"PS3_BOOT_PATH", L"/dev_bdvd/EBOOT.BIN");
 /* set_env(L"PS3_IN_A_COMMENT", L"1"); */
\'\'\'


def recipe(config, ffmpeg):
    common = ['-G', 'Ninja', '-DPS3RECOMP_DIR=' + config['sdk'], '-DPS3RECOMP_RSX_VULKAN=ON']
    for name in ('avcodec', 'swscale'):
        common.append('-DPS3RECOMP_FFMPEG_' + name.upper() + '_LIBRARY=' + ffmpeg[name])
    env = dict(config)
    env['PS3RECOMP_METAL_HEADLESS'] = '1'
    env.update(PS3_SCTRACE=config['trace'], MAG_TRACE='1')
    return common
'''

CMAKE_MODULE = '''# set(PS3RECOMP_COMMENTED ON CACHE BOOL "not set")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PS3RECOMP_SPU_DECODE_CACHE_HEAP ON CACHE BOOL "heap cache")
endif()
set(MAG_OWN_OPTION ON CACHE BOOL "mag-recomp's own")
'''
CMAKE_LISTS = '''set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk" CACHE PATH "the runtime checkout")
set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
target_compile_definitions(game PRIVATE MAG_OWN_DEFINE=1)
'''

HEADER = '| Switch | Kind | Value | Platforms | Set in | Why | Opt out |\n|---|---|---|---|---|---|---|\n'
ROWS = {
    'SPU_DISPATCH_CACHE': '| `SPU_DISPATCH_CACHE` | env | `1` | all | `src/mag_runtime_defaults.h` | cache | `0` |',
    'PS3RECOMP_RSX_DEFERRED_REPORT_CAPACITY':
        '| `PS3RECOMP_RSX_DEFERRED_REPORT_CAPACITY` | env | `4096` | all | `src/mag_runtime_defaults.h` | slots | n |',
    'MAG_NATIVE_CONSUMER': '| `MAG_NATIVE_CONSUMER` | env | `1` | all | `src/mag_runtime_defaults.h` | wake | `0` |',
    'PS3RECOMP_VK_BATCHED_ENGINE': '| `PS3RECOMP_VK_BATCHED_ENGINE` | env | `1` | Linux | `src/mag_runtime_defaults.h` | batch | `0` |',
    'PS3RECOMP_METAL_FPS': '| `PS3RECOMP_METAL_FPS` | env | `1` | macOS | `src/mag_runtime_defaults.h` | title | `0` |',
    'PS3_NP_ONLINE_ID': '| `PS3_NP_ONLINE_ID` | env | the name | all | `src/player_settings_runtime.mm` | id | empty |',
    'PS3RECOMP_SAIL_TIMED_THX':
        '| `PS3RECOMP_SAIL_TIMED_THX` | env | `1` | movies | `installer/mag_installer/native_package.py` | movies | none |',
    'PS3_HLE_UNRESOLVED': '| `PS3_HLE_UNRESOLVED` | env | `halt` | all | `installer/mag_installer/native_package.py` | halt | none |',
    'PS3_VFS_ROOT': '| `PS3_VFS_ROOT` | env | game files | all | `installer/mag_installer/native_package.py` | files | required |',
    'PS3_GAMEDATA_ROOT': '| `PS3_GAMEDATA_ROOT` | env | saves | all | `installer/mag_installer/native_package.py` | saves | required |',
    'PS3_BOOT_PATH': '| `PS3_BOOT_PATH` | env | `/dev_bdvd/EBOOT.BIN` | all | `installer/mag_installer/native_package.py` | boot | required |',
    'PS3RECOMP_RSX_VULKAN': '| `PS3RECOMP_RSX_VULKAN` | CMake | `ON` | Linux | `installer/mag_installer/native_package.py` | vulkan | omit |',
    'PS3RECOMP_FFMPEG_AVCODEC_LIBRARY':
        '| `PS3RECOMP_FFMPEG_AVCODEC_LIBRARY` | CMake | a path | all | `installer/mag_installer/native_package.py` | lib | omit |',
    'PS3RECOMP_FFMPEG_SWSCALE_LIBRARY':
        '| `PS3RECOMP_FFMPEG_SWSCALE_LIBRARY` | CMake | a path | all | `installer/mag_installer/native_package.py` | lib | omit |',
    'PS3RECOMP_METAL_HEADLESS': '| `PS3RECOMP_METAL_HEADLESS` | env | `1` | macOS | `installer/mag_installer/native_package.py` | x | y |',
    'PS3_SCTRACE': '| `PS3_SCTRACE` | env | a mode | all | `installer/mag_installer/native_package.py` | x | y |',
    'PS3RECOMP_SPU_DECODE_CACHE_HEAP': '| `PS3RECOMP_SPU_DECODE_CACHE_HEAP` | CMake | `ON` | Linux | `cmake/MagLinuxRuntime.cmake` | tls | OFF |',
    'SPU_USE_DISPATCH_MT': '| `SPU_USE_DISPATCH_MT` | define | defined | all | `CMakeLists.txt` | musttail | none |',
}
RELIED = '| `PS3RECOMP_NATIVE_OSK_SDL` | CMake | `ON` | Linux | keyboard | OFF |'


class CheckPs3recompFlagsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'repo'
        self.files = {
            'src/mag_runtime_defaults.h': DEFAULTS_H,
            'src/player_settings_runtime.mm': SETTINGS_MM,
            'installer/mag_installer/native_package.py': PACKAGE_PY,
            'cmake/MagLinuxRuntime.cmake': CMAKE_MODULE,
            'CMakeLists.txt': CMAKE_LISTS,
            'tests/test_ignored.py': "env['PS3_ONLY_IN_A_TEST'] = '1'\n",
        }
        for name, text in self.files.items():
            (self.root / name).parent.mkdir(parents=True, exist_ok=True)
            (self.root / name).write_text(text)
        self.rows = dict(ROWS)
        self.relied = [RELIED]

    def write_doc(self, unread=()):
        text = '# Switches\n\n## Set\n\n' + HEADER + '\n'.join(self.rows.values()) + '\n'
        if unread:
            text += '\n## Set but not read by the pinned runtime\n\n' + HEADER + '\n'.join(unread) + '\n'
        text += '\n## Runtime defaults mag-recomp relies on\n\n' + HEADER + '\n'.join(self.relied) + '\n'
        (self.root / 'docs').mkdir(exist_ok=True)
        (self.root / 'docs/ps3recomp-flags.md').write_text(text)

    def problems(self, runtime=None):
        self.write_doc()
        return check(self.root, runtime)[0]

    def test_extracts_the_switches_and_their_values(self):
        found = {(s.kind, s.name, s.value) for s in scan(self.root)}
        self.assertEqual({name for _, name, _ in found}, set(ROWS))
        for expected in [('env', 'SPU_DISPATCH_CACHE', '1'), ('env', 'PS3RECOMP_RSX_DEFERRED_REPORT_CAPACITY', '4096'),
                         ('env', 'PS3_HLE_UNRESOLVED', 'halt'), ('env', 'PS3_VFS_ROOT', None),
                         ('env', 'PS3_BOOT_PATH', '/dev_bdvd/EBOOT.BIN'), ('env', 'PS3_NP_ONLINE_ID', None),
                         ('CMake', 'PS3RECOMP_RSX_VULKAN', 'ON'), ('CMake', 'PS3RECOMP_FFMPEG_SWSCALE_LIBRARY', None),
                         ('CMake', 'PS3RECOMP_SPU_DECODE_CACHE_HEAP', 'ON'), ('define', 'SPU_USE_DISPATCH_MT', None)]:
            self.assertIn(expected, found)

    def test_a_matching_document_passes(self):
        self.assertEqual(self.problems(), [])

    def test_a_missing_row_fails(self):
        del self.rows['PS3RECOMP_VK_BATCHED_ENGINE']
        self.assertTrue(any('PS3RECOMP_VK_BATCHED_ENGINE' in p and 'missing' in p for p in self.problems()))

    def test_a_row_nothing_sets_fails(self):
        self.rows['SPU_GONE'] = '| `SPU_GONE` | env | `1` | all | `src/mag_runtime_defaults.h` | gone | `0` |'
        self.assertTrue(any('SPU_GONE' in p and 'nothing in mag-recomp sets it' in p for p in self.problems()))

    def test_a_changed_value_fails(self):
        self.rows['PS3RECOMP_RSX_DEFERRED_REPORT_CAPACITY'] = self.rows['PS3RECOMP_RSX_DEFERRED_REPORT_CAPACITY'].replace('`4096`', '`8192`')
        self.assertTrue(any('`4096`' in p for p in self.problems()))

    def test_a_missing_file_fails(self):
        path = self.root / 'src/player_settings_portable.cpp'
        path.write_text('void f() { export_variable("PS3_NP_ONLINE_ID", settings.player_name); }\n')
        self.assertTrue(any('src/player_settings_portable.cpp' in p for p in self.problems()))

    def test_a_wrong_kind_fails(self):
        self.rows['PS3RECOMP_RSX_VULKAN'] = self.rows['PS3RECOMP_RSX_VULKAN'].replace('| CMake |', '| env |')
        self.assertTrue(any('PS3RECOMP_RSX_VULKAN is a CMake switch' in p for p in self.problems()))

    def test_a_relied_on_default_that_becomes_set_fails(self):
        self.relied.append('| `SPU_DISPATCH_CACHE` | env | unset | all | x | y |')
        del self.rows['SPU_DISPATCH_CACHE']
        self.assertTrue(any('listed as relied on' in p for p in self.problems()))

    def test_runtime_names(self):
        runtime = Path(self.tmp.name) / 'runtime'
        (runtime / 'libs').mkdir(parents=True)
        (runtime / 'cmake').mkdir()
        names = set(ROWS) - {'PS3RECOMP_METAL_HEADLESS', 'PS3RECOMP_FFMPEG_AVCODEC_LIBRARY', 'PS3RECOMP_FFMPEG_SWSCALE_LIBRARY'}
        (runtime / 'libs/reader.c').write_text(''.join(f'getenv("{n}");\n' for n in sorted(names)))
        (runtime / 'libs/tests').mkdir()
        (runtime / 'libs/tests/only_a_test.c').write_text('getenv("PS3RECOMP_METAL_HEADLESS");\n')
        (runtime / 'CMakeLists.txt').write_text('option(PS3RECOMP_NATIVE_OSK_SDL "dialog" ON)\n')
        (runtime / 'cmake/Ffmpeg.cmake').write_text('find_library(PS3RECOMP_FFMPEG_${component}_LIBRARY NAMES x)\n')
        problems = self.problems(runtime)
        self.assertEqual(problems, ['PS3RECOMP_METAL_HEADLESS is not named anywhere in the runtime at ' + str(runtime)])
        # A switch listed as no longer read must really be absent.
        unread = self.rows.pop('PS3RECOMP_METAL_HEADLESS')
        self.write_doc(unread=[unread])
        self.assertEqual(check(self.root, runtime)[0], [])
        (runtime / 'libs/reader.c').write_text('getenv("PS3RECOMP_METAL_HEADLESS");\n' + ''.join(f'getenv("{n}");\n' for n in sorted(names)))
        self.assertEqual(check(self.root, runtime)[0],
                         [f'PS3RECOMP_METAL_HEADLESS is listed as not read, but the runtime at {runtime} names it'])

    def test_this_repository_matches_its_document(self):
        problems, settings = check(ROOT)
        self.assertEqual(problems, [])
        self.assertIn('PS3RECOMP_RSX_EARLY_LABEL_RELEASE', {s.name for s in settings})


if __name__ == '__main__':
    unittest.main()
