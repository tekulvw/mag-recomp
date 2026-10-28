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
if __name__=='__main__':unittest.main()
