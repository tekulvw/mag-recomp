import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from mag_installer import native_build as native
from mag_installer.model import InstallerError
from mag_installer.safety import Cancellation

class NativeBuildTests(unittest.TestCase):
    def test_retry_reuses_kernel_lease_only_when_requested(self):
        for retry in (False, True):
            with self.subTest(retry=retry), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / 'output'
                with patch.object(native, 'preflight', return_value={'config': {}}), patch.object(native, '_exclusive_lock', side_effect=InstallerError('lease check')) as lease:
                    with self.assertRaisesRegex(InstallerError, 'lease check'):
                        native.install('fixture', output, retry_lock=retry)
                self.assertEqual(lease.call_args.args[:2], (output.with_name('.output.native-lock'), retry))
                self.assertFalse(output.exists())

    def test_windows_build_environment_drops_inherited_overrides(self):
        inherited = {'Path': 'C:/Windows/System32', 'INCLUDE': 'C:/VS/include', 'LIB': 'C:/VS/lib',
                     'WindowsSdkDir': 'C:/SDK', 'SystemRoot': 'C:/Windows',
                     'MAG_WORKSPACE': 'C:/elsewhere', 'MAG_LIFT_HOOKS': 'evil', 'mag_spu_lifter_repository': 'C:/other',
                     'PS3RECOMP_DIR': 'C:/sdk', 'RSX_ENGINE_PHASES': '1', 'CMAKE_TOOLCHAIN_FILE': 'C:/t.cmake',
                     'CFLAGS': '-O3', 'CL': '/O2', 'PYTHONPATH': 'C:/py'}
        config = {k: f'C:/tools/{k}/{k}.exe' for k in ('python', 'cmake', 'ninja', 'git')}
        config['msvc'] = {'vcvars': 'C:/VS/vcvars64.bat', 'clang_cl': 'C:/VS/Llvm/bin/clang-cl.exe'}
        with patch('mag_installer.managed_tools.msvc_environment', return_value=dict(inherited)):
            env = native._windows_environment(config, Path('home'))
        for name in ('MAG_WORKSPACE', 'MAG_LIFT_HOOKS', 'mag_spu_lifter_repository', 'PS3RECOMP_DIR',
                     'RSX_ENGINE_PHASES', 'CMAKE_TOOLCHAIN_FILE', 'CFLAGS', 'CL', 'PYTHONPATH', 'Path'):
            self.assertNotIn(name, env)
        for name in ('INCLUDE', 'LIB', 'WindowsSdkDir', 'SystemRoot'):
            self.assertEqual(env[name], inherited[name])
        self.assertTrue(env['PATH'].endswith('C:/Windows/System32'))
        self.assertEqual(env['CMAKE_BUILD_PARALLEL_LEVEL'], '2')

    def test_failed_stage_retains_incomplete_journal_and_logs(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve(); source=root/'source'; disc=root/'disc'
            source.mkdir(); disc.mkdir(); (source/'CMakeLists.txt').write_text('fixture')
            for name in native.SOURCE_DIRS: (source/name).mkdir()
            config={'source_tree':str(source),'python':'/usr/bin/python3','cmake':'/usr/bin/cmake','ninja':'/usr/bin/ninja','git':'/usr/bin/git','sdks':{'runtime':{'path':str(root/'sdk'),'revision':'a'*40}}}
            for key in ('eboot_elf','engine_elf','firmware'):
                p=root/key; p.write_bytes(b'fixture'); config[key]=str(p)
            out=root/'output'
            with patch.object(native,'preflight',return_value={'config':config,'disc_root':str(disc)}), patch('mag_installer.native_recipe.framework_pins',return_value={}), patch('mag_installer.native_recipe.build_plan',return_value=[{'id':'failed-stage','argv':['fixture']}]), patch.object(native,'run_command',side_effect=InstallerError('fixture failed')):
                with self.assertRaisesRegex(InstallerError,'fixture failed'): native.install(root/'config.json',out)
            record=json.loads((out/'native-build.json').read_text())
            self.assertEqual(record['status'],'incomplete'); self.assertTrue((out/'logs').is_dir())
            with self.assertRaisesRegex(InstallerError,'resume is unsupported'): native.install('unused',out,resume=True)

    def test_mocked_success_and_cancellation_preserve_source_and_full_disc(self):
        from mag_installer.safety import Cancelled
        for cancel in (False, True):
            with self.subTest(cancel=cancel), tempfile.TemporaryDirectory() as temp:
                root=Path(temp).resolve(); source=root/'source'; disc=root/'disc'
                source.mkdir(); disc.mkdir()
                for name in native.SOURCE_DIRS: (source/name).mkdir()
                (source/'CMakeLists.txt').write_bytes(b'unchanged source')
                (disc/'target').mkdir(); (disc/'target/private-file').write_bytes(b'full disc member')
                config={'source_tree':str(source),'python':'/usr/bin/python3','cmake':'/usr/bin/cmake','ninja':'/usr/bin/ninja','git':'/usr/bin/git','sdks':{'runtime':{'path':str(root/'sdk'),'revision':'a'*40}}}
                for key in ('eboot_elf','engine_elf','firmware'):
                    path=root/key; path.write_bytes(b'fixture'); config[key]=str(path)
                output=root/'output'; token=Cancellation(); events=[]
                def stage(*args,**kwargs):
                    if cancel:
                        token.cancel(); token.check()
                with patch.object(native,'preflight',return_value={'config':config,'disc_root':str(disc)}), patch('mag_installer.native_recipe.framework_pins',return_value={'runtime':{'path':'local/ps3recomp-runtime','commit':'a'*40,'tag':'fixture'}}), patch('mag_installer.native_recipe.build_plan',return_value=[]), patch.object(native,'run_command',side_effect=stage), patch.object(native,'_validate_receipts',return_value=['binary1','binary2']):
                    if cancel:
                        with self.assertRaises(Cancelled): native.install('fixture',output,token=token,events=events.append)
                    else:
                        self.assertEqual(native.install('fixture',output,token=token,events=events.append),output)
                record=json.loads((output/'native-build.json').read_text())
                self.assertEqual(record['status'],'incomplete' if cancel else 'complete')
                self.assertEqual((source/'CMakeLists.txt').read_bytes(),b'unchanged source')
                self.assertEqual((disc/'target/private-file').read_bytes(),b'full disc member')
                self.assertEqual((output/'workspace/local/retail-v1.0/PS3_GAME/target/private-file').read_bytes(),b'full disc member')
                self.assertEqual(native._environment(config,root)['GIT_OPTIONAL_LOCKS'],'0')
                with patch('mag_installer.native_recipe.load_config',return_value=config), patch.object(native,'host_key',return_value='macos-arm64'):
                    with self.assertRaisesRegex(InstallerError,'already exists'): native.preflight('fixture',output)

    def test_player_layout_with_elfs_inside_the_disc_copies_each_once(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve(); source=root/'source'; disc=root/'PS3_GAME'
            for name in native.SOURCE_DIRS: (source/name).mkdir(parents=True)
            (disc/'USRDIR/bin').mkdir(parents=True); (disc/'USRDIR/EBOOT.BIN').write_bytes(b'original')
            config={'source_tree':str(source),'python':'/usr/bin/python3','cmake':'/usr/bin/cmake','ninja':'/usr/bin/ninja','git':'/usr/bin/git','sdks':{'runtime':{'path':str(root/'sdk'),'revision':'a'*40}}}
            for key,relative in (('eboot_elf','USRDIR/EBOOT.elf'),('engine_elf','USRDIR/bin/mage_g.elf')):
                (disc/relative).write_bytes(key.encode()); config[key]=str(disc/relative)
            (root/'libsre.prx').write_bytes(b'firmware'); config['firmware']=str(root/'libsre.prx')
            output=root/'output'
            with patch.object(native,'preflight',return_value={'config':config,'disc_root':str(disc)}), patch('mag_installer.native_recipe.framework_pins',return_value={}), patch('mag_installer.native_recipe.build_plan',return_value=[]), patch.object(native,'run_command'), patch.object(native,'_validate_receipts',return_value=[]):
                native.install('fixture',output)
            usrdir=output/'workspace/local/retail-v1.0/PS3_GAME/USRDIR'
            self.assertEqual((usrdir/'EBOOT.elf').read_bytes(),b'eboot_elf')
            self.assertEqual((usrdir/'bin/mage_g.elf').read_bytes(),b'engine_elf')
            self.assertEqual((usrdir/'EBOOT.BIN').read_bytes(),b'original')

    def test_hash_cancellation_and_malformed_identity_are_friendly(self):
        from mag_installer.safety import Cancelled
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve(); file=root/'large'; file.write_bytes(b'fixture')
            token=Cancellation(); token.cancel()
            with self.assertRaises(Cancelled): native._verify_hash(file,'unused',token)
            (root/'tools').mkdir(); (root/'tools/prepare_inputs.py').write_text('RETAIL = [syntax')
            with self.assertRaisesRegex(InstallerError,'metadata is malformed'): native._verify_identities(root,root,{})

    def test_identity_mismatch_says_what_was_supplied(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve(); file=root/'libsre.prx'
            expected=hashlib.sha256(b'reviewed').hexdigest()
            cases=((b'SCE\x00'+b'\x00'*28,'still an encrypted SELF/SPRX'),
                   (b'not an executable','is not an ELF'),
                   (b'\x7fELF'+b'\x02'*28,'different game or firmware release'))
            for content,hint in cases:
                file.write_bytes(content)
                with self.assertRaises(InstallerError) as raised: native._verify_hash(file,expected,decrypted=True)
                text=str(raised.exception)
                self.assertIn('does not match the reviewed native profile',text); self.assertIn(hint,text)
                self.assertIn(f'Found SHA-256 {hashlib.sha256(content).hexdigest()} ({len(content)} bytes)',text)
                self.assertIn(f'expected SHA-256 {expected}',text)
            # Originals are SCE containers by design, so no decrypted-file advice is given for them.
            file.write_bytes(b'SCE\x00 original')
            with self.assertRaises(InstallerError) as raised: native._verify_hash(file,expected)
            self.assertNotIn('encrypted',str(raised.exception)); self.assertIn('Found SHA-256',str(raised.exception))
            file.write_bytes(b'reviewed'); native._verify_hash(file,expected,decrypted=True)

    def test_firmware_source_steps_name_the_download_commands_and_checked_identity(self):
        steps=native.FIRMWARE_SOURCE_STEPS
        for text in ('PS3UPDAT.PUP','https://www.playstation.com/en-us/get-help/ps3-system-software/',
                     '158471fd834f8ea8036136b6aab43cd86c7ba73d79ca30e0af3c0fe0001cf365','rpcs3 --installfw',
                     'rpcs3 --decrypt','dev_flash/sys/external/libsre.sprx','firmware/libsre.prx',
                     'only hosts the latest',"decrypted module's SHA-256",'system software 4.93',
                     '~/Library/Application Support/rpcs3/dev_flash','~/.config/rpcs3/dev_flash',
                     'next to rpcs3.exe'):
            self.assertIn(text,steps)
        self.assertIn('libsre.sprx',native.FIRMWARE_SOURCE_HINT)

    def test_known_unreviewed_firmware_release_is_named(self):
        with tempfile.TemporaryDirectory() as temp:
            file=Path(temp).resolve()/'libsre.prx'; content=b'\x7fELF synthetic release'
            file.write_bytes(content); known={hashlib.sha256(content).hexdigest():'system software 9.99'}
            with self.assertRaises(InstallerError) as raised:
                native._verify_hash(file,'0'*64,decrypted=True,advice='steps',known=known)
            text=str(raised.exception)
            self.assertIn('module from system software 9.99, decrypted correctly',text)
            self.assertNotIn('different game',text); self.assertTrue(text.endswith('\nsteps'))
        for digest,release in native.FIRMWARE_KNOWN_RELEASES.items():
            self.assertRegex(digest,'^[0-9a-f]{64}$'); self.assertIn('system software',release)

    def test_copy_is_independent(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve(); source=root/'source'; source.write_bytes(b'original'); target=root/'new/copied'
            native._copy_file(source,target,Cancellation()); source.write_bytes(b'changed')
            self.assertEqual(target.read_bytes(),b'original')
    def test_tree_refuses_symlinks_and_skips_caches(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve(); (root/'__pycache__').mkdir(); (root/'__pycache__/x').write_bytes(b'cache'); (root/'safe').write_bytes(b'source')
            self.assertEqual(list(native._files(root, exclude=native.EXCLUDED)),[root/'safe'])
            (root/'link').symlink_to(root/'safe')
            with self.assertRaises(InstallerError): list(native._files(root, exclude=native.EXCLUDED))
    def test_both_original_and_decrypted_identities_required(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve(); source=root/'source'; disc=root/'disc'
            (source/'tools').mkdir(parents=True); (source/'profiles').mkdir(); (disc/'USRDIR/bin').mkdir(parents=True)
            a=hashlib.sha256(b'original').hexdigest(); b=hashlib.sha256(b'decrypted').hexdigest()
            settings=hashlib.sha256(b'retail settings').hexdigest()
            (source/'tools/prepare_inputs.py').write_text('RETAIL = '+repr({'EBOOT.BIN':(a,b),'bin/mage_g.self':(a,b)})
                +"\nRETAIL_SETTINGS_ARCHIVE = 'run/settings.sdat'\nRETAIL_SETTINGS_ARCHIVE_SHA256 = "+repr((settings,)))
            (source/'profiles/retail-v1.0-firmware.json').write_text(json.dumps({'images':[{'release':'x','sha256':b}]}))
            for name in ('EBOOT.BIN','bin/mage_g.self'): (disc/'USRDIR'/name).write_bytes(b'original')
            (disc/'USRDIR/run').mkdir(); (disc/'USRDIR/run/settings.sdat').write_bytes(b'retail settings')
            config={}
            for key in ('eboot_elf','engine_elf','firmware'):
                p=root/key; p.write_bytes(b'decrypted'); config[key]=str(p)
            native._verify_identities(source,disc,config); Path(config['engine_elf']).write_bytes(b'wrong')
            with self.assertRaises(InstallerError) as raised: native._verify_identities(source,disc,config)
            # Where-to-get-it steps belong to the firmware module only, not to the game executables.
            self.assertIn('identity',str(raised.exception)); self.assertNotIn('Where to get firmware/libsre.prx',str(raised.exception))
            Path(config['engine_elf']).write_bytes(b'decrypted'); Path(config['firmware']).write_bytes(b'\x7fELF other release')
            with self.assertRaises(InstallerError) as raised: native._verify_identities(source,disc,config)
            text=str(raised.exception)
            self.assertIn('different game or firmware release',text); self.assertIn(native.FIRMWARE_SOURCE_STEPS,text)
    def test_any_reviewed_firmware_image_is_accepted(self):
        with tempfile.TemporaryDirectory() as temp:
            file=Path(temp).resolve()/'libsre.prx'
            digests=[hashlib.sha256(x).hexdigest() for x in (b'release a',b'release b')]
            for content in (b'release a',b'release b'):
                file.write_bytes(content); native._verify_hash(file,digests,decrypted=True)
            file.write_bytes(b'release c')
            with self.assertRaises(InstallerError) as raised: native._verify_hash(file,digests,decrypted=True)
            self.assertIn(f'expected SHA-256 {" or ".join(sorted(digests))}',str(raised.exception))

    def test_changed_settings_archive_refused_before_build(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve(); source=root/'source'; disc=root/'disc'
            (source/'tools').mkdir(parents=True); (source/'profiles').mkdir(); (disc/'USRDIR/bin').mkdir(parents=True)
            a=hashlib.sha256(b'original').hexdigest(); b=hashlib.sha256(b'decrypted').hexdigest()
            accepted=(hashlib.sha256(b'sdata').hexdigest(),hashlib.sha256(b'psarc').hexdigest())
            (source/'tools/prepare_inputs.py').write_text('RETAIL = '+repr({'EBOOT.BIN':(a,b),'bin/mage_g.self':(a,b)})
                +"\nRETAIL_SETTINGS_ARCHIVE = 'run/settings.sdat'\nRETAIL_SETTINGS_ARCHIVE_SHA256 = "+repr(accepted))
            (source/'profiles/retail-v1.0-firmware.json').write_text(json.dumps({'image_sha256':b}))
            for name in ('EBOOT.BIN','bin/mage_g.self'): (disc/'USRDIR'/name).write_bytes(b'original')
            (disc/'USRDIR/run').mkdir(); archive=disc/'USRDIR/run/settings.sdat'
            config={}
            for key in ('eboot_elf','engine_elf','firmware'):
                p=root/key; p.write_bytes(b'decrypted'); config[key]=str(p)
            for good in (b'sdata',b'psarc'):
                archive.write_bytes(good); native._verify_identities(source,disc,config)
            archive.write_bytes(b'psarc with changed settings')
            with self.assertRaisesRegex(InstallerError,'unmodified retail'): native._verify_identities(source,disc,config)
            archive.unlink()
            with self.assertRaises(InstallerError): native._verify_identities(source,disc,config)
    def test_receipts_bind_both_binaries(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve()
            for folder,image in (('build','EBOOT'),('build-engine','mage_g')):
                binary=root/folder/native.binary_name(); binary.parent.mkdir(); binary.write_bytes(b'fixture')
                binary.with_suffix('.receipt.json').write_text(json.dumps({'image_name':image,'binary_sha256':hashlib.sha256(b'fixture').hexdigest(),'sdk_revision':'a'*40,'elf_sha256':'b'*64}))
            (root/'profiles').mkdir(); images={}
            for image in ('EBOOT','mage_g'):
                elf=root/(image+'.elf'); elf.write_bytes(b'elf'); images[image]={'elf':str(elf),'elf_sha256':hashlib.sha256(b'elf').hexdigest()}
                folder='build' if image=='EBOOT' else 'build-engine'
                receipt=root/folder/'MagRecomp.receipt.json'; record=json.loads(receipt.read_text()); record['elf_sha256']=images[image]['elf_sha256']; receipt.write_text(json.dumps(record))
            (root/'profiles/retail-v1.0.json').write_text(json.dumps({'images':images}))
            with self.assertRaisesRegex(InstallerError,'receipt'): native._validate_receipts(root,'wrong-sdk')
            self.assertEqual(len(native._validate_receipts(root, 'a'*40)),2); (root/'build-engine'/native.binary_name()).write_bytes(b'changed')
            with self.assertRaisesRegex(InstallerError,'receipt'): native._validate_receipts(root, 'a'*40)

    def test_rust_source_and_lock_are_copied_without_build_outputs(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve()
            for name in native.SOURCE_DIRS:(root/name).mkdir()
            crate=root/'crates/mag-input';(crate/'src').mkdir(parents=True)
            for name in ('Cargo.toml','Cargo.lock','src/lib.rs'):(crate/name).write_text('synthetic')
            (root/'rust-toolchain.toml').write_text('synthetic pinned toolchain')
            (crate/'target').mkdir();(crate/'target/private.o').write_bytes(b'BUILD OUTPUT')
            paths={p.relative_to(root).as_posix() for p in native._source_files(root)}
            self.assertTrue({'rust-toolchain.toml','crates/mag-input/Cargo.toml','crates/mag-input/Cargo.lock','crates/mag-input/src/lib.rs'}<=paths)
            self.assertFalse(any('/target/' in p for p in paths))

    def test_curated_source_includes_target_tools_and_excludes_all_machine_profiles(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            for name in native.SOURCE_DIRS:
                (root/name).mkdir()
            for relative in ('tools/targets.py', 'cmake/MagRetailTarget.cmake',
                             'profiles/framework.json', 'profiles/retail-v2.12-inputs.json',
                             'profiles/retail-v1.0.json', 'profiles/retail-v1.0-inventory.json',
                             'profiles/retail-v2.12.json', 'profiles/retail-v2.12-inventory.json'):
                path = root/relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('synthetic source or private machine data')
            paths = {p.relative_to(root).as_posix() for p in native._source_files(root)}
            self.assertTrue({'tools/targets.py', 'cmake/MagRetailTarget.cmake',
                             'profiles/framework.json', 'profiles/retail-v2.12-inputs.json'} <= paths)
            for target in ('retail-v1.0', 'retail-v2.12'):
                self.assertNotIn(f'profiles/{target}.json', paths)
                self.assertNotIn(f'profiles/{target}-inventory.json', paths)

    def test_repository_reviewed_identities_remain_literal_v1_inputs(self):
        source = Path(__file__).resolve().parents[2]
        identities = native.reviewed_identities(source)
        self.assertEqual(identities['executables'], {
            'EBOOT.BIN': ['ebe4a52656cc38483d24656029a55ddef6fd53adc7f23b4c52c42f272ac8d1f9',
                          'eb916f92e7c32a9401739d79835630273e1f41a92e6a5d17753d9374062da7d9'],
            'bin/mage_g.self': ['54b324a39e5453916768d8e83057ea72865cf53b23a1a8f058e25915d401840c',
                                '0089f0d6d17e48533ce7ff8d943dddd03487b925e7b04a1a055bfb907568694c']})

if __name__=='__main__': unittest.main()
