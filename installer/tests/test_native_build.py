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
            (source/'tools/prepare_inputs.py').write_text('RETAIL = '+repr({'EBOOT.BIN':(a,b),'bin/mage_g.self':(a,b)}))
            (source/'profiles/retail-v1.0-firmware.json').write_text(json.dumps({'image_sha256':b}))
            for name in ('EBOOT.BIN','bin/mage_g.self'): (disc/'USRDIR'/name).write_bytes(b'original')
            config={}
            for key in ('eboot_elf','engine_elf','firmware'):
                p=root/key; p.write_bytes(b'decrypted'); config[key]=str(p)
            native._verify_identities(source,disc,config); Path(config['engine_elf']).write_bytes(b'wrong')
            with self.assertRaisesRegex(InstallerError,'identity'): native._verify_identities(source,disc,config)
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
if __name__=='__main__': unittest.main()
