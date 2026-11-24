import io
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace
from mag_installer.managed_tools import extract_managed,dependency_plan,LOCK,readiness
from mag_installer.model import InstallerError

class ManagedToolTests(unittest.TestCase):
    def test_existing_apple_sdk_does_not_offer_install(self):
        from mag_installer.managed_tools import ensure_apple_sdk
        with patch('mag_installer.managed_tools._apple_sdk_probe',return_value=('/SDK','')), patch('mag_installer.managed_tools.subprocess.run') as run:
            self.assertEqual(ensure_apple_sdk(),'/SDK');run.assert_not_called()

    def test_explicit_apple_install_resumes_automatically(self):
        from mag_installer.managed_tools import ensure_apple_sdk
        events=[]
        with patch('mag_installer.managed_tools._apple_sdk_probe',side_effect=[(None,'missing'),('/SDK','')]), patch('mag_installer.managed_tools.subprocess.run',return_value=SimpleNamespace(returncode=0,stdout='',stderr='')) as run:
            self.assertEqual(ensure_apple_sdk(events=events.append),'/SDK')
            self.assertEqual(run.call_args.args[0],['/usr/bin/xcode-select','--install'])
            self.assertIn('apple-sdk-ready',[event['event'] for event in events])

    def test_apple_install_rejection_and_timeout_are_actionable(self):
        from mag_installer.managed_tools import ensure_apple_sdk
        with patch('mag_installer.managed_tools._apple_sdk_probe',return_value=(None,'SDK missing')), patch('mag_installer.managed_tools.subprocess.run',return_value=SimpleNamespace(returncode=1,stdout='',stderr='installation already requested')):
            with self.assertRaisesRegex(InstallerError,'installation already requested'):ensure_apple_sdk()
        with patch('mag_installer.managed_tools._apple_sdk_probe',return_value=(None,'SDK missing')), patch('mag_installer.managed_tools.subprocess.run',return_value=SimpleNamespace(returncode=0,stdout='',stderr='')), patch('mag_installer.managed_tools.APPLE_SDK_WAIT_SECONDS',0):
            with self.assertRaisesRegex(InstallerError,'30 minutes'):ensure_apple_sdk()

    def test_apple_install_wait_is_cancellable(self):
        from mag_installer.managed_tools import ensure_apple_sdk
        from mag_installer.safety import Cancellation,Cancelled
        token=Cancellation();calls=[]
        def probe():
            calls.append(1)
            if len(calls)>1:token.cancel()
            return None,'missing'
        with patch('mag_installer.managed_tools._apple_sdk_probe',side_effect=probe), patch('mag_installer.managed_tools.subprocess.run',return_value=SimpleNamespace(returncode=0,stdout='',stderr='')):
            with self.assertRaises(Cancelled):ensure_apple_sdk(token)

    def archive(self,root,members):
        archive=root/'fixture.tar.gz'
        with tarfile.open(archive,'w:gz') as package:
            for name,kind,value in members:
                info=tarfile.TarInfo(name)
                if kind=='file':
                    data=value.encode();info.size=len(data);info.mode=0o755;package.addfile(info,io.BytesIO(data))
                else:
                    info.type=tarfile.SYMTYPE if kind=='link' else tarfile.LNKTYPE;info.linkname=value;package.addfile(info)
        return archive
    def extract(self,archive,target):
        return extract_managed(archive,target,{'archive':'tar.gz','max_entries':100,'max_expanded_bytes':10000})
    @unittest.skipIf(os.name == 'nt', 'POSIX archive links and executable modes; the Windows pins contain neither')
    def test_internal_links_and_executable_modes_and_independent_hardlinks(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);archive=self.archive(root,[('python/bin/python3.12','file','real fixture'),('python/bin/python3','link','python3.12'),('python/bin/copy','hard','python/bin/python3.12')])
            dest=self.extract(archive,root/'out')
            self.assertEqual((dest/'python/bin/python3').read_text(),'real fixture')
            self.assertTrue((dest/'python/bin/python3.12').stat().st_mode&0o111)
            (dest/'python/bin/copy').write_text('modified')
            self.assertEqual((dest/'python/bin/python3.12').read_text(),'real fixture')
    @unittest.skipIf(os.name == 'nt', 'POSIX archive links and executable modes; the Windows pins contain neither')
    def test_nested_framework_alias_resolves_inside_archive(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);archive=self.archive(root,[('Framework/Versions/A/library','file','actual'),('Framework/Versions/Current','link','A'),('Framework/library','link','Versions/Current/library')])
            dest=self.extract(archive,root/'out')
            self.assertEqual((dest/'Framework/library').read_text(),'actual')

    def test_escape_cycle_symlink_ancestor_and_collision_are_refused(self):
        cases=[[('../escape','file','bad')],[('a','link','../escape')],[('a','link','b'),('b','link','a')],[('a','link','b'),('a/child','file','bad'),('b','file','safe')],[('a','file','1'),('a','file','2')]]
        for members in cases:
            with self.subTest(members=members),tempfile.TemporaryDirectory() as temp:
                root=Path(temp);archive=self.archive(root,members)
                with self.assertRaises(InstallerError):self.extract(archive,root/'out')
                self.assertFalse((root/'escape').exists())
    def test_case_collisions_remain_refused_on_windows_and_macos(self):
        for host in ('windows-x86_64', 'macos-arm64'):
            with self.subTest(host=host), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); archive = self.archive(root, [('A', 'file', 'one'), ('a', 'file', 'two')])
                with patch('mag_installer.managed_tools.host_key', return_value=host):
                    with self.assertRaisesRegex(InstallerError, 'colliding'):
                        self.extract(archive, root / 'out')

    @unittest.skipUnless(__import__('sys').platform.startswith('linux'), 'Requires a Linux case-sensitive filesystem')
    def test_linux_terminfo_case_distinct_entries_survive(self):
        # Real pinned Python archive contains both terminfo/2/2621A and 2621a.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = self.archive(root, [('python/share/terminfo/2/2621A', 'file', 'upper'),
                                          ('python/share/terminfo/2/2621a', 'file', 'lower')])
            dest = self.extract(archive, root / 'out')
            self.assertEqual((dest / 'python/share/terminfo/2/2621A').read_text(), 'upper')
            self.assertEqual((dest / 'python/share/terminfo/2/2621a').read_text(), 'lower')

    def test_linux_rejects_normalized_duplicate_archive_names(self):
        for names in (('same', 'same'), ('caf\u00e9', 'cafe\u0301')):
            with self.subTest(names=names), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); archive = self.archive(root, [(name, 'file', 'value') for name in names])
                with patch('mag_installer.managed_tools.host_key', return_value='linux-x86_64'):
                    with self.assertRaisesRegex(InstallerError, 'colliding'):
                        self.extract(archive, root / 'out')

    def test_archive_limits(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);archive=self.archive(root,[('file','file','too big')])
            with self.assertRaises(InstallerError):extract_managed(archive,root/'out',{'archive':'tar.gz','max_entries':1,'max_expanded_bytes':2})
    def test_real_pins_and_explicit_source_graph(self):
        self.assertEqual(len(LOCK),12)
        for record in LOCK.values():
            self.assertTrue(record['url'].startswith('https://'));self.assertEqual(len(record['sha256']),64);self.assertGreater(record['size'],0)
        paths={'cmake':'/owned/cmake','ninja':'/owned/ninja',**{'source-'+key:'/owned/'+key for key in ('sdl2','spirv-cross','glslang','ffmpeg')}}
        plan=dependency_plan(paths,Path('/owned/prefix'))
        self.assertEqual(len(plan),12)
        self.assertEqual(plan[-2]['argv'],['/usr/bin/make','-j2'])
        self.assertIn('-DENABLE_OPT=OFF',next(s for s in plan if s['id']=='configure-glslang')['argv'])
        self.assertIn('--disable-autodetect',next(s for s in plan if s['id']=='configure-ffmpeg')['argv'])
    def test_windows_vulkan_dependencies_are_pinned_and_built_with_the_static_crt(self):
        from mag_installer.managed_tools import WINDOWS_LOCK, WINDOWS_DEPENDENCIES, windows_dependency_plan
        for key in WINDOWS_DEPENDENCIES:
            record=WINDOWS_LOCK[key]
            self.assertTrue(record['url'].startswith('https://github.com/KhronosGroup/'));self.assertEqual(len(record['sha256']),64);self.assertGreater(record['size'],0)
        self.assertNotIn('source-ffmpeg',WINDOWS_LOCK);self.assertNotIn('source-sdl2',WINDOWS_LOCK)
        paths={'cmake':'C:/owned/cmake.exe','ninja':'C:/owned/ninja.exe','python':'C:/owned/python.exe',
               **{'source-'+k:'C:/owned/'+k for k in ('glslang','vulkan-headers','spirv-headers','spirv-tools')}}
        plan=windows_dependency_plan(paths,Path('C:/owned/prefix'),'C:/VS/clang-cl.exe')
        self.assertEqual([s['id'] for s in plan],[f'{step}-{key}' for key in ('vulkan-headers','spirv-headers','spirv-tools','glslang')
                                                  for step in ('configure','build','install')])
        glslang=next(s for s in plan if s['id']=='configure-glslang')['argv']
        # The optimizer drops unused bindings; reflection depends on it (never ENABLE_OPT=OFF).
        for flag in ('-DENABLE_OPT=ON','-DALLOW_EXTERNAL_SPIRV_TOOLS=ON','-DCMAKE_PREFIX_PATH=C:/owned/prefix','-DBUILD_SHARED_LIBS=OFF',
                     '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded','-DCMAKE_CXX_COMPILER=C:/VS/clang-cl.exe'):
            self.assertIn(flag,glslang)
        tools=next(s for s in plan if s['id']=='configure-spirv-tools')['argv']
        self.assertIn('-DSPIRV-Headers_SOURCE_DIR=C:/owned/spirv-headers',tools);self.assertIn('-DPython3_EXECUTABLE=C:/owned/python.exe',tools)
        self.assertIn('ef96ed763b43b59b33b31b362f09a02b729fa1c9',WINDOWS_LOCK['source-spirv-tools']['url'])
        self.assertIn('496543121ce6419f23d6fa5d7194ba66c36212d2',WINDOWS_LOCK['source-spirv-headers']['url'])
    def test_windows_movie_decoder_is_a_pinned_lgpl_shared_ffmpeg_9(self):
        from mag_installer.managed_tools import WINDOWS_LOCK, WINDOWS_FFMPEG_DLLS, WINDOWS_FFMPEG_LIBRARIES
        record=WINDOWS_LOCK['ffmpeg']
        self.assertEqual(record['archive'],'zip');self.assertEqual(len(record['sha256']),64);self.assertGreater(record['size'],0)
        self.assertRegex(record['url'],r'^https://github\.com/BtbN/FFmpeg-Builds/releases/download/autobuild-[0-9-]+/ffmpeg-n9\.0[^/]*-win64-lgpl-shared-9\.0\.zip$')
        self.assertTrue(record['root'].endswith('-win64-lgpl-shared-9.0'))
        # libavcodec/libavformat 63 and libavutil 61: the majors the runtime's FFmpeg 9.0 headers expect.
        self.assertIn('avcodec-63.dll',WINDOWS_FFMPEG_DLLS);self.assertIn('avutil-61.dll',WINDOWS_FFMPEG_DLLS)
        self.assertEqual(WINDOWS_FFMPEG_LIBRARIES,('avcodec','avformat','avutil','swscale'))
    def test_windows_controllers_use_the_official_prebuilt_sdl2_outside_the_dependency_identity(self):
        from mag_installer.managed_tools import WINDOWS_LOCK, WINDOWS_DEPENDENCIES
        record=WINDOWS_LOCK['sdl2']
        self.assertEqual(record['id'],'sdl2-windows');self.assertEqual(record['archive'],'zip');self.assertEqual(record['root'],'SDL2-2.32.10')
        self.assertEqual(record['url'],'https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-VC.zip')
        self.assertEqual((record['size'],record['sha256']),(7172966,'af347939395a58b365846aaea27391e69f9ec9d4dd650d6ac40802159b418a6e'))
        # A source build would change MANAGED-DEPENDENCIES.json and refuse existing installs.
        self.assertNotIn('sdl2',WINDOWS_DEPENDENCIES)
    def test_windows_bootstrap_returns_the_sdl2_record_and_refuses_an_incomplete_package(self):
        import json as _json
        from mag_installer import managed_tools
        from mag_installer.managed_tools import WINDOWS_LOCK, WINDOWS_DEPENDENCIES, WINDOWS_FFMPEG_LIBRARIES, WINDOWS_FFMPEG_DLLS
        from mag_installer.safety import Cancellation
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)/'managed'
            def materialize(spec,artifacts,cache,token,events):
                location=Path(temp)/'artifacts'/spec['id']
                if 'root' in spec: (location/spec['root']).mkdir(parents=True,exist_ok=True)
                return location
            ffmpeg=Path(temp)/'artifacts/ffmpeg-windows'/WINDOWS_LOCK['ffmpeg']['root']
            sdl2=Path(temp)/'artifacts/sdl2-windows/SDL2-2.32.10'
            files=[*(ffmpeg/'lib'/(n+'.lib') for n in WINDOWS_FFMPEG_LIBRARIES),*(ffmpeg/'bin'/n for n in WINDOWS_FFMPEG_DLLS),
                   sdl2/'cmake/sdl2-config.cmake',sdl2/'include/SDL.h',sdl2/'lib/x64/SDL2.dll',sdl2/'lib/x64/SDL2.lib',sdl2/'LICENSE.txt']
            for path in files: path.parent.mkdir(parents=True,exist_ok=True);path.write_text('fixture')
            (ffmpeg/'include').mkdir()
            prefix=root/'prefix';prefix.mkdir(parents=True)
            (prefix/'MANAGED-DEPENDENCIES.json').write_text(_json.dumps({k:WINDOWS_LOCK[k]['sha256'] for k in WINDOWS_DEPENDENCIES}))
            (prefix/'MANAGED-INVENTORY.json').write_text(_json.dumps(managed_tools._inventory(prefix,Cancellation())))
            with patch('mag_installer.managed_tools.materialize',side_effect=materialize), \
                 patch('mag_installer.managed_tools._install_wheels'), patch('mag_installer.managed_tools.run_command'), \
                 patch.dict(os.environ,{'SystemRoot':str(Path(temp)/'Windows')}):
                result=managed_tools._bootstrap_windows(root,Path(temp)/'cache',None,None)
                self.assertEqual(result['sdl2'],{'cmake_dir':str(sdl2/'cmake'),'dll':str(sdl2/'lib/x64/SDL2.dll'),'license':str(sdl2/'LICENSE.txt')})
                (sdl2/'lib/x64/SDL2.dll').unlink()
                with self.assertRaisesRegex(InstallerError,r'pinned SDL2 build is missing: .*SDL2\.dll'):
                    managed_tools._bootstrap_windows(root,Path(temp)/'cache',None,None)
if __name__=='__main__':unittest.main()

class ManagedBytecodeTests(unittest.TestCase):
    """Running a managed Python writes bytecode caches into its tree; a retry
    must not refuse the verified tool for that, and nothing else may change."""
    def test_stray_or_rewritten_bytecode_is_discarded_and_other_changes_refused(self):
        import json as _json
        from mag_installer import managed_tools
        from mag_installer.safety import Cancellation
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); spec = {'id': 'python', 'sha256': 'a' * 64}
            final = root / ('python-' + 'a' * 16)
            (final / 'lib/__pycache__').mkdir(parents=True)
            (final / 'lib/module.py').write_text('x = 1\n')
            (final / 'lib/__pycache__/shipped.cpython-312.pyc').write_bytes(b'shipped')
            receipt = final / 'MANAGED-INVENTORY.json'
            receipt.write_text(_json.dumps(managed_tools._inventory(final, Cancellation())))
            # What a build leaves behind: a new cache and a rewritten shipped one.
            (final / 'lib/__pycache__/module.cpython-312.pyc').write_bytes(b'new')
            (final / 'lib/__pycache__/shipped.cpython-312.pyc').write_bytes(b'rewritten')
            self.assertEqual(managed_tools.materialize(spec, root, root / 'cache', Cancellation(), lambda e: None), final)
            self.assertEqual(sorted(p.name for p in (final / 'lib/__pycache__').iterdir()), [])
            # The verified source itself changing is still refused.
            (final / 'lib/module.py').write_text('x = 2\n')
            with self.assertRaisesRegex(InstallerError, 'inventory changed'):
                managed_tools.materialize(spec, root, root / 'cache', Cancellation(), lambda e: None)

    def test_build_commands_cache_bytecode_outside_the_tools(self):
        from mag_installer import managed_tools, native_build
        home = Path('/scratch/home')
        prefix = Path(managed_tools._env('p/python', 'c/cmake', 'n/ninja', home)['PYTHONPYCACHEPREFIX'])
        self.assertEqual(prefix, home / 'pycache')
        if os.name != 'nt':  # The Windows environment comes from the MSVC toolchain.
            env = native_build._environment({'python': '/t/python', 'cmake': '/t/cmake', 'ninja': '/t/ninja', 'git': '/t/git'}, home)
            self.assertEqual(Path(env['PYTHONPYCACHEPREFIX']), home / 'pycache')



class RustToolchainTests(unittest.TestCase):
    """The pinned Rust components for the game controls adapter, from synthetic archives."""

    def test_pins_follow_rust_toolchain_and_cover_every_host(self):
        import re
        from mag_installer import managed_tools
        toolchain = (Path(__file__).resolve().parents[2] / 'rust-toolchain.toml').read_text()
        self.assertEqual(re.search(r'channel\s*=\s*"([^"]+)"', toolchain)[1], managed_tools.RUST_CHANNEL)
        self.assertEqual(set(managed_tools.RUST_LOCKS), set(managed_tools.LOCKS))
        for host, specs in managed_tools.RUST_LOCKS.items():
            self.assertEqual(set(specs), {'rustc', 'cargo', 'rust-std'})
            for spec in specs.values():
                self.assertTrue(spec['url'].startswith('https://static.rust-lang.org/dist/'))
                self.assertIn('-' + managed_tools.RUST_CHANNEL + '-' + managed_tools.RUST_TARGETS[host], spec['url'])
                self.assertEqual(len(spec['sha256']), 64); self.assertGreater(spec['size'], 0)
        # The managed tool lists (and so the dependency-prefix identities) do not include it.
        for lock in managed_tools.LOCKS.values():
            self.assertFalse(any('rust' in key for key in lock))

    def component(self, root, name, inner, files):
        archive = root / (name + '.tar.xz')
        with tarfile.open(archive, 'w:xz') as package:
            for relative, data, mode in [('install.sh', '#!/bin/sh\n', 0o755), (inner + '/manifest.in', 'file:x\n', 0o644),
                                         *((inner + '/' + path, text, 0o755 if path.startswith('bin/') else 0o644)
                                           for path, text in files.items())]:
                info = tarfile.TarInfo(name + '/' + relative); payload = data.encode()
                info.size = len(payload); info.mode = mode; package.addfile(info, io.BytesIO(payload))
        return archive

    def toolchain(self, root, host, extra_std=None):
        from mag_installer import managed_tools
        target = managed_tools.RUST_TARGETS[host]; suffix = '.exe' if host == 'windows-x86_64' else ''
        archives = {
            'rustc': self.component(root, f'rustc-1.99.0-{target}', 'rustc',
                                    {'bin/rustc' + suffix: 'rustc', 'lib/librustc_driver.so': 'driver'}),
            'cargo': self.component(root, f'cargo-1.99.0-{target}', 'cargo', {'bin/cargo' + suffix: 'cargo'}),
            'rust-std': self.component(root, f'rust-std-1.99.0-{target}', 'rust-std-' + target,
                                       {f'lib/rustlib/{target}/lib/libstd.rlib': 'std', **(extra_std or {})})}
        def acquire(spec, cache, token, events):
            return next(path for key, path in archives.items() if spec['id'] == f'rust-{key}-{target}')
        return acquire

    def test_rustc_and_its_standard_library_share_one_verified_tree(self):
        from mag_installer import managed_tools
        for host in managed_tools.RUST_TARGETS:
            with self.subTest(host=host), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); target = managed_tools.RUST_TARGETS[host]
                with patch('mag_installer.managed_tools.acquire', side_effect=self.toolchain(root, host)), \
                     patch('mag_installer.managed_tools.host_key', return_value=host):
                    paths = managed_tools.rust_toolchain(root / 'managed', cache=root / 'cache', host=host)
                    rustc, cargo = Path(paths['rustc']), Path(paths['cargo'])
                    self.assertEqual(rustc.read_text(), 'rustc'); self.assertEqual(cargo.read_text(), 'cargo')
                    self.assertTrue(rustc.name.endswith('.exe') == (host == 'windows-x86_64'))
                    self.assertEqual((rustc.parent.parent / f'lib/rustlib/{target}/lib/libstd.rlib').read_text(), 'std')
                    self.assertFalse((rustc.parent.parent / 'manifest.in').exists())
                    if os.name != 'nt': self.assertTrue(rustc.stat().st_mode & 0o111)
                    # A retry reuses the verified tree; a changed file is refused.
                    self.assertEqual(managed_tools.rust_toolchain(root / 'managed', cache=root / 'cache', host=host), paths)
                    (rustc.parent.parent / f'lib/rustlib/{target}/lib/libstd.rlib').write_text('changed')
                    with self.assertRaisesRegex(InstallerError, 'Rust toolchain changed'):
                        managed_tools.rust_toolchain(root / 'managed', cache=root / 'cache', host=host)

    def test_overlapping_components_are_refused(self):
        from mag_installer import managed_tools
        host = 'linux-x86_64'
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with patch('mag_installer.managed_tools.acquire', side_effect=self.toolchain(root, host, {'bin/rustc': 'other'})), \
                 patch('mag_installer.managed_tools.host_key', return_value=host):
                with self.assertRaisesRegex(InstallerError, 'overlap'):
                    managed_tools.rust_toolchain(root / 'managed', cache=root / 'cache', host=host)
            self.assertFalse(any(p.name.startswith('rust-toolchain-') for p in (root / 'managed/artifacts').iterdir()))
