"""The Windows MAG folder carries the prebuilt libraries' DLLs beside the executables and their licences."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from mag_installer.managed_tools import WINDOWS_FFMPEG_DLLS
from mag_installer import native_package_windows as windows


def _digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class WindowsPackagedLibrariesTests(unittest.TestCase):
    def fixture(self, root):
        workspace = root / 'workspace'; (workspace / 'profiles').mkdir(parents=True)
        disc = workspace / 'local/retail-v1.0/PS3_GAME/USRDIR'; (disc / 'bin').mkdir(parents=True)
        images = {}
        for folder, image, elf in [('build', 'EBOOT', disc / 'EBOOT.elf'), ('build-engine', 'mage_g', disc / 'bin/mage_g.elf')]:
            elf.write_bytes(b'fixture ELF ' + image.encode())
            binary = workspace / folder / 'MagRecomp.exe'; binary.parent.mkdir(); binary.write_bytes(image.encode())
            receipt = dict(image_name=image, binary_sha256=_digest(binary), elf_sha256=_digest(elf), sdk_revision='a' * 40)
            binary.with_suffix('.receipt.json').write_text(json.dumps(receipt))
            images[image] = dict(elf_sha256=_digest(elf), source_sha256='b' * 64)
        (workspace / 'profiles/retail-v1.0.json').write_text(json.dumps(dict(images=images)))
        (workspace / 'profiles/framework.json').write_text(json.dumps(dict(strict_runtime_revisions=['a' * 40],
                                                                           strict_hle_runtime_revisions=['a' * 40])))
        (workspace / 'src').mkdir()
        repository = Path(__file__).resolve().parents[2]
        for name in windows.SUPERVISOR_SOURCES:
            (workspace / 'src' / name).write_bytes((repository / 'src' / name).read_bytes())
        ffmpeg = root / 'ffmpeg'; (ffmpeg / 'include').mkdir(parents=True); (ffmpeg / 'bin').mkdir()
        for name in WINDOWS_FFMPEG_DLLS: (ffmpeg / 'bin' / name).write_text('ffmpeg ' + name)
        (ffmpeg / 'LICENSE.txt').write_text('LGPL fixture')
        sdl2 = root / 'SDL2-2.32.10'; (sdl2 / 'cmake').mkdir(parents=True); (sdl2 / 'lib/x64').mkdir(parents=True)
        (sdl2 / 'lib/x64/SDL2.dll').write_text('sdl2 fixture'); (sdl2 / 'LICENSE.txt').write_text('zlib fixture')
        records = {'ffmpeg': {'include': str(ffmpeg / 'include')},
                   'sdl2': {'cmake_dir': str(sdl2 / 'cmake'), 'dll': str(sdl2 / 'lib/x64/SDL2.dll'), 'license': str(sdl2 / 'LICENSE.txt')}}
        return workspace, records

    def package(self, root, workspace, extra):
        folder = root / ('MAG-' + '-'.join(sorted(extra)) if extra else 'MAG-none'); folder.mkdir()
        def compile(clang_cl, environment, source_dir, output, token): Path(output).write_text('supervisor fixture')
        config = {'msvc': {'vcvars': 'vcvars64.bat', 'clang_cl': 'clang-cl.exe'}, **extra}
        with patch.object(windows, '_compile', side_effect=compile):
            return windows.package(config, workspace, folder / 'MAG', environment={})

    def test_sdl2_and_ffmpeg_are_packaged_in_any_combination(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve(); workspace, records = self.fixture(root)
            for keys in ((), ('ffmpeg',), ('sdl2',), ('ffmpeg', 'sdl2')):
                with self.subTest(keys=keys):
                    destination = self.package(root, workspace, {key: records[key] for key in keys})
                    expected = {'MagRecomp.exe', 'MagEngine.exe'}
                    if 'ffmpeg' in keys: expected |= set(WINDOWS_FFMPEG_DLLS)
                    if 'sdl2' in keys: expected.add('SDL2.dll')
                    self.assertEqual({p.name for p in (destination / 'bin').iterdir()}, expected)
                    licenses = destination / 'resources/licenses'
                    self.assertEqual({p.name for p in licenses.iterdir()} if licenses.exists() else set(),
                                     {{'ffmpeg': 'FFmpeg-LICENSE.txt', 'sdl2': 'SDL2-LICENSE.txt'}[key] for key in keys})
                    if 'sdl2' in keys:
                        self.assertEqual((destination / 'bin/SDL2.dll').read_text(), 'sdl2 fixture')
                        self.assertEqual((licenses / 'SDL2-LICENSE.txt').read_text(), 'zlib fixture')

    def test_missing_sdl2_dll_fails_and_leaves_no_mag_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve(); workspace, records = self.fixture(root)
            Path(records['sdl2']['dll']).unlink()
            with self.assertRaisesRegex(Exception, 'ordinary file'):
                self.package(root, workspace, {'sdl2': records['sdl2']})
            self.assertFalse((root / 'MAG-sdl2/MAG').exists())


if __name__ == '__main__':
    unittest.main()
