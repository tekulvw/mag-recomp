"""Pinned private tools and source builds, with OS-authorized compiler setup."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import shutil
import stat
import subprocess
import time
import tarfile
import tempfile
import unicodedata
import zipfile

from .model import InstallerError, host_key
from .pipeline import run_command, _write_json
from .safety import Cancellation
from .tools import acquire

# Maintainer-reviewed official artifact metadata; populated below.
LOCK = {'python': {'id': 'python',
            'url': 'https://github.com/astral-sh/python-build-standalone/releases/download/20261021/cpython-3.12.15%2B20261021-aarch64-apple-darwin-install_only.tar.gz',
            'size': 25158763,
            'sha256': 'a18cc33862b53bd54a148c710def5375c1b47470cccedf817a19c2d211764584',
            'archive': 'tar.gz',
            'executable': 'python/bin/python3.12',
            'max_expanded_bytes': 1073741824,
            'max_entries': 50000},
 'cmake': {'id': 'cmake',
           'url': 'https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-macos-universal.tar.gz',
           'size': 89454352,
           'sha256': '4b7b73704b1db9b374e5c9ab8e17ac6148b817b6396ee75cd94a852cbac9d305',
           'archive': 'tar.gz',
           'executable': 'cmake-4.4.4-macos-universal/CMake.app/Contents/bin/cmake',
           'max_expanded_bytes': 1073741824,
           'max_entries': 50000},
 'ninja': {'id': 'ninja',
           'url': 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-mac.zip',
           'size': 314051,
           'sha256': 'c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b',
           'archive': 'zip',
           'executable': 'ninja',
           'max_expanded_bytes': 1073741824,
           'max_entries': 50000},
 'wheel-pycryptodome': {'id': 'wheel-pycryptodome',
                        'url': 'https://files.pythonhosted.org/packages/41/4c/954a9a1451f7bcc89d66db39bf6499ef5701e8ff35b6c898c659a7ec8546/pycryptodome-4.2.0-cp39-abi3-macosx_10_9_universal2.whl',
                        'size': 1680663,
                        'sha256': '3e430168f767ecec60c1f850df4abbd0b9f94786b9cc8041a00e976f6d5190c6',
                        'archive': 'zip',
                        'max_expanded_bytes': 104857600,
                        'max_entries': 10000,
                        'version': '4.2.0'},
 'wheel-capstone': {'id': 'wheel-capstone',
                    'url': 'https://files.pythonhosted.org/packages/42/9a/4eecfbc94961cda70301fd13de69042be1943bee456e37bc1369840601b1/capstone-5.0.9-py3-none-macosx_11_0_arm64.whl',
                    'size': 1202361,
                    'sha256': 'fc269c5082e17d7f9f265cd6980e2fdc6ab572df913951d76db0357e06b8001d',
                    'archive': 'zip',
                    'max_expanded_bytes': 104857600,
                    'max_entries': 10000,
                    'version': '5.0.9'},
 'wheel-construct': {'id': 'wheel-construct',
                     'url': 'https://files.pythonhosted.org/packages/b2/fb/08b3f4bf05da99aba8ffea52a558758def16e8516bc75ca94ff73587e7d3/construct-2.10.70-py3-none-any.whl',
                     'size': 63020,
                     'sha256': 'c80be81ef595a1a821ec69dc16099550ed22197615f4320b57cc9ce2a672cb30',
                     'archive': 'zip',
                     'max_expanded_bytes': 104857600,
                     'max_entries': 10000,
                     'version': '2.10.70'},
 'wheel-tabulate': {'id': 'wheel-tabulate',
                    'url': 'https://files.pythonhosted.org/packages/99/55/db07de81b5c630da5cbf5c7df646580ca26dfaefa593667fc6f2fe016d2e/tabulate-0.10.0-py3-none-any.whl',
                    'size': 39814,
                    'sha256': 'f0b0622e567335c8fabaaa659f1b33bcb6ddfe2e496071b743aa113f8774f2d3',
                    'archive': 'zip',
                    'max_expanded_bytes': 104857600,
                    'max_entries': 10000,
                    'version': '0.10.0'},
 'wheel-tqdm': {'id': 'wheel-tqdm',
                'url': 'https://files.pythonhosted.org/packages/a7/03/921a3d3c75785aca9ebfbfcabfbc3a1be12e2ab5265deb026d55a5a3f83e/tqdm-4.70.1-py3-none-any.whl',
                'size': 80199,
                'sha256': 'c293e525e6fef9c20e8728fd4612df02a0aa31bb5fe91ecd93e123b1b7bffa73',
                'archive': 'zip',
                'max_expanded_bytes': 104857600,
                'max_entries': 10000,
                'version': '4.70.1'},
 'source-sdl2': {'id': 'source-sdl2',
                 'url': 'https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz',
                 'size': 7630262,
                 'sha256': '5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165',
                 'archive': 'tar.gz',
                 'root': 'SDL2-2.32.10',
                 'max_expanded_bytes': 536870912,
                 'max_entries': 50000},
 'source-glslang': {'id': 'source-glslang',
                    'url': 'https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz',
                    'size': 4632787,
                    'sha256': '9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2',
                    'archive': 'tar.gz',
                    'root': 'glslang-16.6.0',
                    'max_expanded_bytes': 536870912,
                    'max_entries': 50000},
 'source-spirv-cross': {'id': 'source-spirv-cross',
                        'url': 'https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz',
                        'size': 2032974,
                        'sha256': 'f708982e88b763ef5b0394ed468fc2c3628f68cdab89f2e7a946cd628c04e721',
                        'archive': 'tar.gz',
                        'root': 'SPIRV-Cross-vulkan-sdk-1.4.363.0',
                        'max_expanded_bytes': 536870912,
                        'max_entries': 50000},
 'source-ffmpeg': {'id': 'source-ffmpeg',
                   'url': 'https://ffmpeg.org/releases/ffmpeg-9.0.1.tar.xz',
                   'size': 12036420,
                   'sha256': 'cf38e0e28c7e5605942c4a77755349b0145804a397af37eb1fb4c77cb237f635',
                   'archive': 'tar.xz',
                   'root': 'ffmpeg-9.0.1',
                   'max_expanded_bytes': 536870912,
                   'max_entries': 50000}}


# Windows x86_64: the same roles, from the same official origins. The Microsoft
# compiler/SDK is owned by Visual Studio Build Tools (see ensure_windows_build_tools);
# SDL2, glslang, SPIRV-Cross and FFmpeg are not used by the Windows runtime
# (XInput/WASAPI/D3D12, and FFmpeg is an optional run-time load).
WINDOWS_LOCK = {'python': {'id': 'python-windows',
            'url': 'https://github.com/astral-sh/python-build-standalone/releases/download/20261021/cpython-3.12.15%2B20261021-x86_64-pc-windows-msvc-install_only.tar.gz',
            'size': 46388793,
            'sha256': 'be7f14464d1d66f84429ad27e01f6d1482963afff64a2fb5256b2a51c7b5d398',
            'archive': 'tar.gz',
            'executable': 'python/python.exe',
            'site_packages': 'python/Lib/site-packages',
            'max_expanded_bytes': 1073741824,
            'max_entries': 50000},
 'cmake': {'id': 'cmake-windows',
           'url': 'https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip',
           'size': 54400562,
           'sha256': 'bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536',
           'archive': 'zip',
           'executable': 'cmake-4.4.4-windows-x86_64/bin/cmake.exe',
           'max_expanded_bytes': 1073741824,
           'max_entries': 50000},
 'ninja': {'id': 'ninja-windows',
           'url': 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip',
           'size': 291570,
           'sha256': '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65',
           'archive': 'zip',
           'executable': 'ninja.exe',
           'max_expanded_bytes': 1073741824,
           'max_entries': 50000},
 'git': {'id': 'mingit-windows',
         'url': 'https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.4/MinGit-2.56.0.4-64-bit.zip',
         'size': 39846926,
         'sha256': 'e3bf78351fe777fba47ddee5950082f8a98bb0f02604fdc7ba3ee2cb68ffa877',
         'archive': 'zip',
         'executable': 'cmd/git.exe',
         'max_expanded_bytes': 1073741824,
         'max_entries': 50000},
 'wheel-pycryptodome': {'id': 'wheel-pycryptodome-windows',
                        'url': 'https://files.pythonhosted.org/packages/03/5b/a4a31e6e1aabd5a26da77c7960e7fcce4fca9132e51868f1bba10bf1e932/pycryptodome-4.2.0-cp39-abi3-win_amd64.whl',
                        'size': 1658684,
                        'sha256': '3282c9fd744a16594e51e1a9fbfe4d658b6b0aaa1f050926e6d591ac7494f78b',
                        'archive': 'zip',
                        'max_expanded_bytes': 104857600,
                        'max_entries': 10000,
                        'version': '4.2.0'},
 'wheel-capstone': {'id': 'wheel-capstone-windows',
                    'url': 'https://files.pythonhosted.org/packages/50/e6/6f06fdb6a9ed32b2f7cd9c036b92d5324112c3ef7080f2c71efc367d40dd/capstone-5.0.9-py3-none-win_amd64.whl',
                    'size': 1273459,
                    'sha256': '732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0',
                    'archive': 'zip',
                    'max_expanded_bytes': 104857600,
                    'max_entries': 10000,
                    'version': '5.0.9'},
 'wheel-construct': LOCK['wheel-construct'],
 'wheel-tabulate': LOCK['wheel-tabulate'],
 'wheel-tqdm': LOCK['wheel-tqdm'],
 # tqdm requires colorama on Windows only.
 'wheel-colorama': {'id': 'wheel-colorama',
                    'url': 'https://files.pythonhosted.org/packages/d1/d6/3965ed04c63042e047cb6a3e6ed1a63a35087b6a609aa3a15ed8ac56c221/colorama-0.4.6-py2.py3-none-any.whl',
                    'size': 25335,
                    'sha256': '4f1d9991f5acc0ca119f9d443620b77f9d6b33703e51011c16baf57afb285fc6',
                    'archive': 'zip',
                    'max_expanded_bytes': 104857600,
                    'max_entries': 10000,
                    'version': '0.4.6'}}

# Linux x86_64 pins verified against official GitHub release assets and PyPI JSON.
LINUX_LOCK = {'python': {'id': 'python-linux',
            'url': 'https://github.com/astral-sh/python-build-standalone/releases/download/20261021/cpython-3.12.15%2B20261021-x86_64-unknown-linux-gnu-install_only.tar.gz',
            'size': 67908973,
            'sha256': '5ff9f0602ce964a529ed1e8555d3abab6341926d724402ba9a15e9e877191124',
            'archive': 'tar.gz',
            'max_expanded_bytes': 1073741824,
            'max_entries': 50000,
            'executable': 'python/bin/python3.12'},
 'cmake': {'id': 'cmake-linux',
           'url': 'https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-linux-x86_64.tar.gz',
           'size': 64865570,
           'sha256': 'e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb',
           'archive': 'tar.gz',
           'max_expanded_bytes': 1073741824,
           'max_entries': 50000,
           'executable': 'cmake-4.4.4-linux-x86_64/bin/cmake'},
 'ninja': {'id': 'ninja-linux',
           'url': 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip',
           'size': 134040,
           'sha256': '5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6',
           'archive': 'zip',
           'max_expanded_bytes': 1073741824,
           'max_entries': 50000,
           'executable': 'ninja'},
 'wheel-pycryptodome': {'id': 'wheel-pycryptodome-linux',
                        'url': 'https://files.pythonhosted.org/packages/18/88/33df6b1176b63aed427745696d88ed38f7877104cf0f6004ee9ad600f897/pycryptodome-4.2.0-cp39-abi3-manylinux2014_x86_64.manylinux_2_17_x86_64.whl',
                        'size': 2976488,
                        'sha256': '76777686de3a354c41710727988c2284448d5db9bd82e5ddb9a9b6710d17d9df',
                        'archive': 'zip',
                        'max_expanded_bytes': 1073741824,
                        'max_entries': 50000,
                        'version': '4.2.0'},
 'wheel-capstone': {'id': 'wheel-capstone-linux',
                    'url': 'https://files.pythonhosted.org/packages/d1/39/17747862222bb062e86b501f1f148d5ff589b77908b080d30f7f085cbfb7/capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl',
                    'size': 1485316,
                    'sha256': '273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e',
                    'archive': 'zip',
                    'max_expanded_bytes': 1073741824,
                    'max_entries': 50000,
                    'version': '5.0.9'}}
for _key in ('wheel-construct', 'wheel-tabulate', 'wheel-tqdm', 'source-sdl2', 'source-glslang', 'source-spirv-cross', 'source-ffmpeg'):
    LINUX_LOCK[_key] = LOCK[_key]

LOCKS = {'macos-arm64': LOCK, 'windows-x86_64': WINDOWS_LOCK, 'linux-x86_64': LINUX_LOCK}
SUPPORTED_HOSTS = tuple(LOCKS)


def _name(value):
    if not value or '\\' in value or value.startswith('/') or ':' in value:
        raise InstallerError('Unsafe managed archive path')
    parts = PurePosixPath(value).parts
    if '..' in parts or any(part in ('', '.') for part in parts):
        raise InstallerError('Unsafe managed archive path')
    return '/'.join(parts)


def extract_managed(archive, destination, spec, token=None):
    """Extract bounded ordinary files and internal links without traversing links.

    Files/directories are created before links. Link graphs must terminate at
    an existing archive member and remain inside the new extraction root.
    Hard links are copied as independent files. Device and special files fail.
    """
    token = token or Cancellation(); destination = Path(destination)
    if destination.exists() or destination.is_symlink():
        raise InstallerError('Managed extraction destination already exists')
    members = {}; folded = set(); total = 0
    is_zip = spec['archive'] == 'zip'
    opener = zipfile.ZipFile(archive) if is_zip else tarfile.open(archive, 'r:*')
    with opener as package:
        entries = package.infolist() if is_zip else package.getmembers()
        if len(entries) > spec['max_entries']: raise InstallerError('Too many managed archive members')
        for entry in entries:
            token.check(); raw = entry.filename if is_zip else entry.name
            raw = raw.rstrip('/')
            if raw in ('', '.'): continue
            if raw.startswith('./'): raw = raw[2:]
            name = _name(raw)
            # Linux archives (notably Python's terminfo database) contain real
            # case-distinct entries. Keep normalized/exact duplicate rejection,
            # and retain case folding on Windows and default macOS volumes.
            key = unicodedata.normalize('NFC', name)
            if host_key() != 'linux-x86_64': key = key.casefold()
            if key in folded: raise InstallerError('Duplicate/colliding managed archive members')
            folded.add(key)
            mode = (entry.external_attr >> 16) if is_zip else entry.mode
            size = entry.file_size if is_zip else entry.size
            directory = entry.is_dir() if is_zip else entry.isdir()
            symlink = stat.S_ISLNK(mode) if is_zip else entry.issym()
            hardlink = False if is_zip else entry.islnk()
            if not is_zip and not (directory or symlink or hardlink or entry.isfile()):
                raise InstallerError('Special managed archive member refused')
            if is_zip and stat.S_IFMT(mode) not in (0,stat.S_IFREG,stat.S_IFDIR,stat.S_IFLNK):
                raise InstallerError('Special managed ZIP member refused')
            total += size
            if size < 0 or total > spec['max_expanded_bytes']: raise InstallerError('Managed archive expanded size exceeds bound')
            target = None
            if symlink or hardlink:
                if is_zip:
                    if size > 4096: raise InstallerError('Oversized managed symlink')
                    target = package.read(entry).decode('utf-8')
                else: target = entry.linkname
                if not target or '\\' in target or target.startswith('/') or ':' in target:
                    raise InstallerError('Unsafe managed archive link')
                resolved = posixpath.normpath(target if hardlink else posixpath.join(posixpath.dirname(name), target))
                target = _name(resolved)
            members[name] = {'entry':entry,'directory':directory,'target':target,'hardlink':hardlink,'mode':mode}
        for name in members:
            for parent in PurePosixPath(name).parents:
                p=str(parent)
                if p == '.': continue
                if p in members and not members[p]['directory']:
                    raise InstallerError('Managed archive member has a nondirectory ancestor')
        def terminal(name, seen=()):
            if name in seen or len(seen)>32: raise InstallerError('Managed archive link cycle/depth refused')
            parts=PurePosixPath(name).parts
            for offset in range(1,len(parts)):
                prefix='/'.join(parts[:offset])
                if prefix in members and members[prefix]['target']:
                    resolved=_name(posixpath.normpath(posixpath.join(members[prefix]['target'],*parts[offset:])))
                    return terminal(resolved,seen+(name,))
            if name not in members:
                # An implicit parent directory is a valid internal target.
                if any(n.startswith(name+'/') for n in members): return name
                raise InstallerError('Managed archive link target is absent')
            if name in seen or len(seen)>32: raise InstallerError('Managed archive link cycle/depth refused')
            item=members[name]
            return terminal(item['target'],seen+(name,)) if item['target'] else name
        for name,item in members.items():
            if item['target']: terminal(name)
        destination.mkdir(parents=True,mode=0o700)
        try:
            for name,item in members.items():
                token.check(); path=destination/name
                if item['target']: continue
                if item['directory']: path.mkdir(parents=True,exist_ok=True); continue
                path.parent.mkdir(parents=True,exist_ok=True)
                incoming=package.open(item['entry']) if is_zip else package.extractfile(item['entry'])
                with incoming, path.open('xb') as outgoing:
                    copied=0
                    while chunk:=incoming.read(1024*1024):
                        token.check(); copied+=len(chunk)
                        if copied>spec['max_expanded_bytes']: raise InstallerError('Managed member exceeds bound')
                        outgoing.write(chunk)
                path.chmod(0o755 if item['mode'] & 0o111 else 0o644)
            for name,item in members.items():
                if not item['target']: continue
                token.check(); path=destination/name; path.parent.mkdir(parents=True,exist_ok=True)
                if item['hardlink']:
                    source=destination/terminal(name)
                    if not source.is_file(): raise InstallerError('Managed hardlink must resolve to a regular file')
                    shutil.copyfile(source,path); path.chmod(source.stat().st_mode & 0o777)
                else:
                    path.symlink_to(posixpath.relpath(item['target'],posixpath.dirname(name) or '.'))
        except BaseException:
            shutil.rmtree(destination)
            raise
    return destination


def _inventory(root, token):
    records={}
    for path in sorted(root.rglob('*')):
        token.check(); name=path.relative_to(root).as_posix()
        if name == 'MANAGED-INVENTORY.json': continue
        if path.is_symlink(): records[name]={'link':os.readlink(path)}
        elif path.is_file():
            with path.open('rb') as stream:
                digest=hashlib.sha256()
                while chunk:=stream.read(1024*1024): token.check(); digest.update(chunk)
            records[name]={'sha256':digest.hexdigest(),'mode':stat.S_IMODE(path.stat().st_mode)}
    return records


def materialize(spec, root, cache, token, events):
    root=Path(root); root.mkdir(parents=True,exist_ok=True)
    if root.is_symlink(): raise InstallerError('Managed root must not be a symlink')
    final=root/(spec['id']+'-'+spec['sha256'][:16])
    if final.exists():
        receipt=final/'MANAGED-INVENTORY.json'
        if final.is_symlink() or receipt.is_symlink() or not receipt.is_file(): raise InstallerError('Managed tool has no verified inventory; choose fresh storage')
        if json.loads(receipt.read_text()) != _inventory(final,token): raise InstallerError('Managed tool inventory changed; choose fresh storage')
        return final
    blob=acquire(spec,cache,token,events)
    with tempfile.TemporaryDirectory(prefix='.managed-',dir=root) as scratch:
        extracted=extract_managed(blob,Path(scratch)/'payload',spec,token)
        _write_json(extracted/'MANAGED-INVENTORY.json',_inventory(extracted,token))
        extracted.rename(final)
    return final


def _env(python, cmake, ninja, home, site=None):
    env={'PATH':os.pathsep.join((str(Path(python).parent),str(Path(cmake).parent),str(Path(ninja).parent),'/usr/bin','/bin','/usr/sbin','/sbin')),
         'HOME':str(home),'TMPDIR':str(home),'PYTHONNOUSERSITE':'1','GIT_OPTIONAL_LOCKS':'0','CMAKE_BUILD_PARALLEL_LEVEL':'2'}
    if site: env['PYTHONPATH']=os.pathsep.join(map(str,site))
    return env


def dependency_plan(paths, prefix):
    """Explicit source-build graph; all outputs stay under the owned prefix."""
    cmake=str(paths['cmake']); ninja=str(paths['ninja']); prefix=Path(prefix)
    stages=[]
    for key,options in (
        ('sdl2',['-DSDL_SHARED=ON','-DSDL_STATIC=OFF','-DSDL_TEST=OFF','-DSDL_TESTS=OFF']),
        ('spirv-cross',['-DSPIRV_CROSS_CLI=OFF','-DSPIRV_CROSS_ENABLE_TESTS=OFF','-DSPIRV_CROSS_SHARED=OFF','-DSPIRV_CROSS_STATIC=ON']),
        ('glslang',['-DENABLE_OPT=OFF','-DENABLE_GLSLANG_BINARIES=OFF','-DGLSLANG_TESTS=OFF','-DGLSLANG_ENABLE_INSTALL=ON','-DBUILD_TESTING=OFF','-DENABLE_CTEST=OFF'])):
        source=Path(paths['source-'+key]); build=prefix.parent/('build-'+key)
        stages.extend([{'id':'configure-'+key,'cwd':source,'argv':[cmake,'-S',str(source),'-B',str(build),'-G','Ninja','-DCMAKE_MAKE_PROGRAM='+ninja,'-DCMAKE_POLICY_VERSION_MINIMUM=3.5','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_INSTALL_PREFIX='+str(prefix),'-DCMAKE_INSTALL_LIBDIR=lib','-DCMAKE_POSITION_INDEPENDENT_CODE=ON',*options]},
                       {'id':'build-'+key,'cwd':source,'argv':[cmake,'--build',str(build),'--parallel','2']},
                       {'id':'install-'+key,'cwd':source,'argv':[cmake,'--install',str(build)]}])
    ffmpeg=Path(paths['source-ffmpeg']); build=prefix.parent/'build-ffmpeg'
    stages.extend([{'id':'configure-ffmpeg','cwd':build,'argv':[str(ffmpeg/'configure'),'--prefix='+str(prefix),'--disable-programs','--disable-doc','--disable-debug','--disable-static','--enable-shared','--disable-autodetect','--disable-avdevice','--disable-avfilter','--disable-network','--enable-zlib',*(['--disable-x86asm'] if paths.get('host')=='linux-x86_64' else [])]},
                   {'id':'build-ffmpeg','cwd':build,'argv':['/usr/bin/make','-j2']},
                   {'id':'install-ffmpeg','cwd':build,'argv':['/usr/bin/make','install']}])
    return stages


APPLE_SDK_WAIT_SECONDS = 30 * 60


def _apple_sdk_probe():
    """Read the active OS SDK without creating installer output or logs."""
    try:
        result = subprocess.run(['/usr/bin/xcrun', '--show-sdk-path'],
                                env={'PATH':'/usr/bin:/bin:/usr/sbin:/sbin'},
                                stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, timeout=15)
    except (OSError, subprocess.SubprocessError) as exc:
        return None, str(exc)
    path = result.stdout.strip()
    if result.returncode == 0 and path and Path(path).is_dir():
        return path, ''
    return None, (result.stderr.strip() or result.stdout.strip() or f'xcrun exited {result.returncode}')[-2000:]


def ensure_apple_sdk(token=None, events=None):
    """Explicit Build action only: offer Apple's OS installer and resume when ready.

    The OS owns installation and its authorization UI. This function never uses
    sudo or a package manager, and does not create installer output directories.
    Readiness inspection must use the read-only probe instead.
    """
    token = token or Cancellation(); events = events or (lambda event: None)
    token.check()
    sdk, detail = _apple_sdk_probe()
    if sdk: return sdk
    events({'event':'stage-start','stage':'apple-sdk',
            'message':'Apple Command Line Tools are needed. Complete the standard macOS installation dialog; Build MAG will continue automatically.'})
    try:
        offered = subprocess.run(['/usr/bin/xcode-select', '--install'],
                                 env={'PATH':'/usr/bin:/bin:/usr/sbin:/sbin'},
                                 stdin=subprocess.DEVNULL, capture_output=True,
                                 text=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as exc:
        raise InstallerError(f'Apple Command Line Tools installation could not be offered: {exc}. The selected Apple SDK must be installed and its developer license accepted before Build MAG can continue.') from exc
    if offered.returncode:
        reason = (offered.stderr.strip() or offered.stdout.strip() or detail)[-2000:]
        raise InstallerError(f'Apple Command Line Tools installation request was rejected: {reason}. Complete any existing Apple installation dialog, select a working Apple SDK and accept its developer license, then retry Build MAG.')
    started = last_notice = time.monotonic()
    deadline = started + APPLE_SDK_WAIT_SECONDS
    while time.monotonic() < deadline:
        token.check()
        sdk, detail = _apple_sdk_probe()
        if sdk:
            events({'event':'apple-sdk-ready','message':'Apple SDK is available. Continuing Build MAG.'})
            return sdk
        now = time.monotonic()
        if now-last_notice >= 10:
            events({'event':'stage-activity','stage':'apple-sdk','elapsed':now-started,
                    'message':'Waiting for the Apple Command Line Tools installation. Complete the macOS dialog to continue.'})
            last_notice=now
        # Check cancellation throughout the two-second polling interval.
        next_probe=min(time.monotonic()+2,deadline)
        while time.monotonic() < next_probe:
            token.check(); time.sleep(min(0.2,max(0,next_probe-time.monotonic())))
    raise InstallerError(f'Apple SDK was not available within 30 minutes: {detail}. Complete the Apple Command Line Tools installation and developer license steps, then retry Build MAG.')


def readiness(root):
    """Report metadata/OS prerequisites separately from native build validation."""
    host=host_key()
    if host=='windows-x86_64':
        return _windows_readiness()
    if host=='linux-x86_64':
        return _linux_readiness()
    if host!='macos-arm64':
        return {'ready':False,'reason':'Managed player builds support Apple Silicon Macs and 64-bit Windows PCs.'}
    expected={'python','cmake','ninja','source-sdl2','source-spirv-cross','source-glslang','source-ffmpeg',
              'wheel-pycryptodome','wheel-capstone','wheel-construct','wheel-tabulate','wheel-tqdm'}
    missing=sorted(expected-set(LOCK))
    if missing: return {'ready':False,'reason':'Missing managed dependency pins: '+', '.join(missing)}
    sdk, detail = _apple_sdk_probe()
    if not sdk:
        return {'ready':True,'requires_apple_sdk':True,'reason':'Build MAG will open the standard macOS Command Line Tools installation dialog.'}
    marker=Path(root)/'prefix/MANAGED-DEPENDENCIES.json'
    return {'ready':True,'reason':'Pinned managed tool and source dependency graph is available. Apple SDK detection and native source configuration run before compilation.',
            'native_dependencies_built':marker.is_file()}


def _install_wheels(python_root, wheel_roots, token, site_packages='python/lib/python3.12/site-packages'):
    site=python_root/site_packages
    site.mkdir(parents=True,exist_ok=True)
    for wheel in wheel_roots:
        for source in wheel.rglob('*'):
            token.check(); relative=source.relative_to(wheel)
            if relative.as_posix()=='MANAGED-INVENTORY.json': continue
            if source.is_symlink(): raise InstallerError('Python wheel links are unsupported')
            target=site/relative
            if source.is_dir(): target.mkdir(parents=True,exist_ok=True);continue
            if target.exists():
                if target.read_bytes()!=source.read_bytes(): raise InstallerError('Python wheel files conflict with the managed runtime')
                continue
            target.parent.mkdir(parents=True,exist_ok=True); shutil.copyfile(source,target)
    _write_json(python_root/'MANAGED-INVENTORY.json',_inventory(python_root,token))


def _dependency_paths(prefix, linux=False):
    paths = [prefix/'include/SDL2/SDL.h',prefix/'lib/cmake/SDL2/SDL2Config.cmake',
            prefix/'lib/cmake/glslang/glslang-config.cmake',prefix/'lib/libspirv-cross-c.a',
            prefix/'include/libavcodec/avcodec.h',prefix/'lib/libavcodec.63.dylib',
            prefix/'lib/libavformat.63.dylib',prefix/'lib/libavutil.61.dylib',prefix/'lib/libswscale.10.dylib']
    return [Path(str(p).replace('.63.dylib', '.so.63').replace('.61.dylib', '.so.61').replace('.10.dylib', '.so.10')) for p in paths] if linux else paths


def bootstrap(root, cache=None, token=None, events=None, build_dependencies=True):
    """Acquire managed tools, Python wheel closure and optional native dependencies.

    This requires the OS-managed Apple command-line SDK, never Homebrew.
    Returns tool paths, Python module roots and exact FFmpeg/CMake prefix paths.
    """
    if host_key()=='windows-x86_64':
        return _bootstrap_windows(root, cache, token, events)
    if host_key()=='linux-x86_64':
        return _bootstrap_linux(root, cache, token, events, build_dependencies)
    if host_key()!='macos-arm64': raise InstallerError('Managed native tools support Apple Silicon Macs and 64-bit Windows PCs')
    if not Path('/usr/bin/xcrun').is_file(): raise InstallerError('Apple Command Line Tools are required. Install them using the standard macOS developer-tools dialog, then retry Build MAG.')
    token=token or Cancellation(); events=events or (lambda event:None)
    root=Path(root).absolute(); root.mkdir(parents=True,exist_ok=True)
    if root.is_symlink(): raise InstallerError('Managed root must not be a symlink')
    cache=Path(cache) if cache else root/'cache'; home=root/'home';home.mkdir(exist_ok=True)
    logs=root/'logs';logs.mkdir(exist_ok=True)
    initial_env={'PATH':'/usr/bin:/bin:/usr/sbin:/sbin','HOME':str(home),'TMPDIR':str(home)}
    run_command(['/usr/bin/xcrun','--show-sdk-path'],root,logs/'apple-sdk.log',token,initial_env,events,'apple-sdk')
    paths={'git':'/usr/bin/git'}; sites=[]; python_root=None
    for key,spec in LOCK.items():
        if key.startswith('source-') and not build_dependencies: continue
        location=materialize(spec,root/'artifacts',cache,token,events)
        if key in ('python','cmake','ninja'):
            paths[key]=location/spec['executable']
            if key=='python': python_root=location
        elif key.startswith('wheel-'): sites.append(location)
        elif key.startswith('source-'): paths[key]=location/spec['root']
    _install_wheels(python_root,sites,token)
    env=_env(paths['python'],paths['cmake'],paths['ninja'],home,sites)
    run_command([str(paths['python']),'-c','import Crypto,capstone,construct,tabulate,tqdm'],root,logs/'python-modules.log',token,env,events,'python-modules')
    prefix=root/'prefix'
    if build_dependencies:
        marker=prefix/'MANAGED-DEPENDENCIES.json'
        identity={k:v['sha256'] for k,v in LOCK.items()}
        if marker.exists():
            if json.loads(marker.read_text()) != identity: raise InstallerError('Managed dependency identity changed; use a fresh managed directory')
            inventory=prefix/'MANAGED-INVENTORY.json'
            if not inventory.is_file() or json.loads(inventory.read_text()) != _inventory(prefix,token):
                raise InstallerError('Managed dependency files changed; use fresh managed storage')
        else:
            for stage in dependency_plan(paths,prefix):
                Path(stage['cwd']).mkdir(parents=True,exist_ok=True)
                events({'event':'stage-start','stage':stage['id']})
                run_command(stage['argv'],stage['cwd'],logs/(stage['id']+'.log'),token,env,events,stage['id'])
            missing=[str(path) for path in _dependency_paths(prefix) if not path.exists()]
            if missing: raise InstallerError('Managed dependencies did not produce required files: '+', '.join(missing))
            _write_json(marker,identity)
            _write_json(prefix/'MANAGED-INVENTORY.json',_inventory(prefix,token))
    paths.update(dependencies_ready=build_dependencies,cmake_args=['-DCMAKE_PREFIX_PATH='+str(prefix)],python_site=[str(p) for p in sites],prefix=str(prefix),logs=str(logs),
                 ffmpeg={'include':str(prefix/'include'),'avcodec':str(prefix/'lib/libavcodec.63.dylib'),'avformat':str(prefix/'lib/libavformat.63.dylib'),'avutil':str(prefix/'lib/libavutil.61.dylib'),'swscale':str(prefix/'lib/libswscale.10.dylib')})
    return {k:str(v) if isinstance(v,Path) else v for k,v in paths.items()}


# ---------------------------------------------------------------------------
# Windows: Visual Studio Build Tools (MSVC linker/CRT/Windows SDK + its clang-cl)
# ---------------------------------------------------------------------------
# Microsoft owns this installation, its license and its elevation prompt, as Apple
# owns the Command Line Tools. The bootstrapper changes with every VS update, so
# it is pinned by Authenticode publisher rather than by hash.
VS_BOOTSTRAPPER_URL = 'https://aka.ms/vs/17/release/vs_BuildTools.exe'
VS_REQUIRED_COMPONENTS = ('Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                          'Microsoft.VisualStudio.Component.VC.Llvm.Clang')
VS_INSTALL_COMPONENTS = ('Microsoft.VisualStudio.Workload.VCTools',
                         'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                         'Microsoft.VisualStudio.Component.VC.Llvm.Clang',
                         'Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset',
                         'Microsoft.VisualStudio.Component.Windows11SDK.22621')
VS_INSTALL_WAIT_SECONDS = 2 * 60 * 60
VS_SUCCESS_CODES = (0, 3010)  # 3010: installed, a restart is recommended


def _system32():
    root = os.environ.get('SystemRoot') or os.environ.get('windir')
    if not root: raise InstallerError('Windows SystemRoot is unavailable')
    return Path(root) / 'System32'


def _vswhere():
    base = os.environ.get('ProgramFiles(x86)') or os.environ.get('ProgramFiles') or 'C:\\Program Files (x86)'
    return Path(base) / 'Microsoft Visual Studio/Installer/vswhere.exe'


def _vs_instances(*arguments):
    vswhere = _vswhere()
    if not vswhere.is_file(): return None
    try:
        result = subprocess.run([str(vswhere), *arguments, '-format', 'json', '-utf8', '-nologo'],
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
        entries = json.loads(result.stdout.decode('utf-8') or '[]')
    except (OSError, subprocess.SubprocessError, ValueError, UnicodeError):
        return None
    return [entry for entry in entries if isinstance(entry, dict) and entry.get('installationPath')]


def windows_toolchain_probe():
    """Read-only: the newest VS instance with MSVC x64 tools and the bundled clang-cl."""
    entries = _vs_instances('-products', '*', '-requires', *VS_REQUIRED_COMPONENTS)
    if entries is None:
        return None, 'Visual Studio Build Tools are not installed'
    for entry in sorted(entries, key=lambda e: str(e.get('installationVersion', '')), reverse=True):
        install = Path(str(entry['installationPath']))
        vcvars = install / 'VC/Auxiliary/Build/vcvars64.bat'
        clang_cl = install / 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
        if vcvars.is_file() and clang_cl.is_file():
            return {'installation': str(install), 'vcvars': str(vcvars), 'clang_cl': str(clang_cl),
                    'version': str(entry.get('installationVersion', '')),
                    'product': str(entry.get('productId', ''))}, ''
    return None, 'No Visual Studio installation has both the MSVC x64 tools and C++ Clang'


def msvc_environment(vcvars, base=None):
    """Capture the variables vcvars64.bat sets; the batch file never runs our stages."""
    cmd = _system32() / 'cmd.exe'
    command = f'"{cmd}" /d /s /c ""{vcvars}" >nul 2>&1 && set"'
    try:
        result = subprocess.run(command, env=dict(base if base is not None else os.environ),
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as exc:
        raise InstallerError(f'Cannot load the Visual Studio build environment: {exc}') from exc
    if result.returncode:
        raise InstallerError('The Visual Studio build environment failed to load (vcvars64.bat). Repair Visual Studio Build Tools, then retry Build MAG.')
    environment = {}
    for line in result.stdout.decode('mbcs' if os.name == 'nt' else 'utf-8', errors='replace').splitlines():
        key, separator, value = line.partition('=')
        if separator and key and not key.startswith('='): environment[key] = value
    if not environment.get('WindowsSdkDir') or not environment.get('INCLUDE') or not environment.get('LIB'):
        raise InstallerError('Visual Studio Build Tools are missing the Windows SDK.')
    return environment


def _windows_readiness():
    toolchain, detail = windows_toolchain_probe()
    if not toolchain:
        return {'ready': True, 'requires_build_tools': True,
                'reason': 'Build MAG will open the Microsoft Visual Studio Build Tools installer (C++ tools, Clang and Windows SDK).'}
    return {'ready': True, 'reason': 'Pinned managed tools are available; Visual Studio ' + toolchain['version'] + ' C++ tools found.'}


def _verify_microsoft_signature(path):
    powershell = _system32() / 'WindowsPowerShell/v1.0/powershell.exe'
    script = ('$s = Get-AuthenticodeSignature -LiteralPath $env:MAG_SIGNED_FILE; '
              '[Console]::Out.WriteLine($s.Status); [Console]::Out.WriteLine($s.SignerCertificate.Subject)')
    try:
        result = subprocess.run([str(powershell), '-NoProfile', '-NonInteractive', '-Command', script],
                                env={**os.environ, 'MAG_SIGNED_FILE': str(path)}, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as exc:
        raise InstallerError(f'Cannot verify the Visual Studio installer signature: {exc}') from exc
    lines = result.stdout.strip().splitlines()
    if result.returncode or len(lines) < 2 or lines[0].strip() != 'Valid' or 'O=Microsoft Corporation' not in lines[1]:
        raise InstallerError('The downloaded Visual Studio installer is not validly signed by Microsoft; it was not run.')


def _download_bootstrapper(cache, token, events):
    import urllib.request
    cache = Path(cache); cache.mkdir(parents=True, exist_ok=True)
    target = cache / 'vs_BuildTools.exe'
    target.unlink(missing_ok=True)
    events({'event': 'download-start', 'tool': 'Visual Studio Build Tools installer'})
    request = urllib.request.Request(VS_BOOTSTRAPPER_URL, headers={'User-Agent': 'mag-installer'})
    received = 0
    try:
        with urllib.request.urlopen(request, timeout=60) as response, target.open('xb') as output:
            if not response.geturl().startswith('https://'):
                raise InstallerError('Visual Studio installer download left HTTPS')
            while block := response.read(1024 * 1024):
                token.check(); received += len(block)
                if received > 64 * 1024 * 1024: raise InstallerError('Visual Studio installer download is unexpectedly large')
                output.write(block)
    except OSError as exc:
        raise InstallerError(f'Cannot download the Visual Studio Build Tools installer: {exc}') from exc
    return target


def ensure_windows_build_tools(cache=None, token=None, events=None):
    """Explicit Build action only: add Microsoft's C++ tools if absent, then continue.

    Runs the Microsoft-signed Visual Studio bootstrapper with its own progress UI,
    license and UAC prompt (--passive). Never uses a package manager or silent
    elevation. Returns the selected toolchain record.
    """
    token = token or Cancellation(); events = events or (lambda event: None)
    token.check()
    toolchain, detail = windows_toolchain_probe()
    if toolchain:
        try:
            msvc_environment(toolchain['vcvars'])
            return toolchain
        except InstallerError as exc:
            detail = str(exc)
    events({'event': 'stage-start', 'stage': 'build-tools',
            'message': 'Microsoft C++ Build Tools are needed. Approve the Windows prompt and let the Visual Studio installer finish; Build MAG continues automatically.'})
    cache = Path(cache) if cache else Path(tempfile.gettempdir()) / 'mag-installer'
    bootstrapper = _download_bootstrapper(cache, token, events)
    _verify_microsoft_signature(bootstrapper)
    argv = [str(bootstrapper)]
    existing = (_vs_instances('-products', 'Microsoft.VisualStudio.Product.BuildTools') or [None])[0]
    if existing:
        argv += ['modify', '--installPath', str(existing['installationPath'])]
    argv += ['--passive', '--wait', '--norestart', '--nocache']
    for component in VS_INSTALL_COMPONENTS: argv += ['--add', component]
    started = last_notice = time.monotonic()
    try:
        process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError as exc:
        if getattr(exc, 'winerror', None) != 740:  # ERROR_ELEVATION_REQUIRED
            raise InstallerError(f'The Visual Studio installer could not start: {exc}') from exc
        process = _ElevatedProcess(argv)
    while process.poll() is None:
        token.check()
        now = time.monotonic()
        if now - started > VS_INSTALL_WAIT_SECONDS:
            raise InstallerError('Visual Studio Build Tools did not finish installing within two hours. Complete or repair it from the Visual Studio Installer, then retry Build MAG.')
        if now - last_notice >= 10:
            events({'event': 'stage-activity', 'stage': 'build-tools', 'elapsed': now - started,
                    'message': 'Waiting for the Visual Studio installer to finish.'})
            last_notice = now
        try: process.wait(timeout=1)
        except subprocess.TimeoutExpired: pass
    if process.returncode not in VS_SUCCESS_CODES:
        raise InstallerError(f'The Visual Studio installer stopped (code {process.returncode}). If it was cancelled or the Windows prompt was declined, retry Build MAG.')
    toolchain, detail = windows_toolchain_probe()
    if not toolchain:
        raise InstallerError(f'Visual Studio Build Tools are still incomplete: {detail}.')
    msvc_environment(toolchain['vcvars'])
    events({'event': 'build-tools-ready', 'message': 'Microsoft C++ Build Tools are available. Continuing Build MAG.'})
    return toolchain


class _ElevatedProcess:
    """A program whose manifest requires elevation: start it through the normal
    UAC prompt (ShellExecuteEx "runas") and expose poll/wait like Popen."""

    def __init__(self, argv):
        import ctypes
        from ctypes import wintypes
        class Info(ctypes.Structure):
            _fields_ = [('cbSize', wintypes.DWORD), ('fMask', ctypes.c_ulong), ('hwnd', wintypes.HWND),
                        ('lpVerb', wintypes.LPCWSTR), ('lpFile', wintypes.LPCWSTR), ('lpParameters', wintypes.LPCWSTR),
                        ('lpDirectory', wintypes.LPCWSTR), ('nShow', ctypes.c_int), ('hInstApp', wintypes.HINSTANCE),
                        ('lpIDList', ctypes.c_void_p), ('lpClass', wintypes.LPCWSTR), ('hkeyClass', wintypes.HKEY),
                        ('dwHotKey', wintypes.DWORD), ('hIcon', wintypes.HANDLE), ('hProcess', wintypes.HANDLE)]
        info = Info(cbSize=ctypes.sizeof(Info), fMask=0x40, lpVerb='runas', lpFile=argv[0],  # SEE_MASK_NOCLOSEPROCESS
                    lpParameters=subprocess.list2cmdline(argv[1:]), nShow=1)
        if not ctypes.windll.shell32.ShellExecuteExW(ctypes.byref(info)) or not info.hProcess:
            raise InstallerError('The Visual Studio installer needs administrator approval; the Windows prompt was declined.')
        self._kernel = ctypes.windll.kernel32
        self._handle = info.hProcess
        self.returncode = None

    def poll(self):
        if self.returncode is None and self._kernel.WaitForSingleObject(self._handle, 0) == 0:
            import ctypes
            code = ctypes.c_ulong()
            self._kernel.GetExitCodeProcess(self._handle, ctypes.byref(code))
            self._kernel.CloseHandle(self._handle)
            self.returncode = code.value
        return self.returncode

    def wait(self, timeout=None):
        deadline = None if timeout is None else time.monotonic() + timeout
        while self.poll() is None:
            if deadline is not None and time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired('vs_BuildTools.exe', timeout)
            time.sleep(0.2)
        return self.returncode


def _bootstrap_windows(root, cache, token, events):
    token = token or Cancellation(); events = events or (lambda event: None)
    root = Path(root).absolute(); root.mkdir(parents=True, exist_ok=True)
    if root.is_symlink(): raise InstallerError('Managed root must not be a symlink')
    cache = Path(cache) if cache else root / 'cache'
    home = root / 'home'; home.mkdir(exist_ok=True)
    logs = root / 'logs'; logs.mkdir(exist_ok=True)
    paths = {}; sites = []; python_root = None
    for key, spec in WINDOWS_LOCK.items():
        location = materialize(spec, root / 'artifacts', cache, token, events)
        if key in ('python', 'cmake', 'ninja', 'git'):
            paths[key] = location / spec['executable']
            if key == 'python': python_root = location
        elif key.startswith('wheel-'): sites.append(location)
    _install_wheels(python_root, sites, token, WINDOWS_LOCK['python']['site_packages'])
    env = {**os.environ, 'PATH': os.pathsep.join((str(Path(paths['python']).parent), str(_system32()))),
           'PYTHONNOUSERSITE': '1', 'TEMP': str(home), 'TMP': str(home)}
    for name in ('PYTHONPATH', 'PYTHONHOME'): env.pop(name, None)
    run_command([str(paths['python']), '-c', 'import Crypto,capstone,construct,tabulate,tqdm'], root,
                logs / 'python-modules.log', token, env, events, 'python-modules')
    run_command([str(paths['git']), '--version'], root, logs / 'git.log', token, env, events, 'git')
    paths.update(dependencies_ready=True, cmake_args=[], python_site=[str(p) for p in sites], logs=str(logs))
    return {k: str(v) if isinstance(v, Path) else v for k, v in paths.items()}


# Ubuntu owns compiler, C library and desktop headers. All downloaded tools and
# dependency builds remain private. No sudo shell or password collection.
LINUX_PACKAGES = ('clang', 'build-essential', 'git', 'pkg-config', 'libssl-dev',
                  'zlib1g-dev', 'libx11-dev', 'libxext-dev', 'libxrandr-dev',
                  'libxcursor-dev', 'libxi-dev', 'libxfixes-dev', 'libxss-dev',
                  'libxkbcommon-dev', 'libwayland-dev', 'libgl1-mesa-dev',
                  'libasound2-dev', 'libpulse-dev', 'libudev-dev', 'libvulkan-dev',
                  'libvulkan1', 'mesa-vulkan-drivers')


def linux_platform_probe():
    """Read only; distribution support is deliberately explicit."""
    import platform
    try:
        release = {}
        for line in Path('/etc/os-release').read_text().splitlines():
            key, separator, value = line.partition('=')
            if separator: release[key] = value.strip('"\'')
        version = tuple(int(n) for n in release.get('VERSION_ID', '').split('.'))
    except (OSError, ValueError):
        return False, 'Cannot identify Linux distribution from /etc/os-release'
    if platform.machine().lower() not in ('x86_64', 'amd64') or release.get('ID') != 'ubuntu' or version < (24, 4):
        return False, 'Linux Build MAG supports Ubuntu 24.04 or later on x86_64 with a graphical desktop'
    return True, ''


def linux_toolchain_probe():
    supported, detail = linux_platform_probe()
    if not supported: return None, detail
    try:
        result = subprocess.run(['/usr/bin/dpkg-query', '-W', '-f=${Package} ${Status}\n', *LINUX_PACKAGES],
                                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
        installed = {line.split(' ', 1)[0] for line in result.stdout.splitlines() if line.endswith(' install ok installed')}
    except (OSError, subprocess.SubprocessError):
        installed = set()
    missing = sorted(set(LINUX_PACKAGES) - installed)
    paths = {'cc': '/usr/bin/clang', 'cxx': '/usr/bin/clang++', 'git': '/usr/bin/git'}
    if missing or any(not Path(p).is_file() for p in paths.values()):
        return None, 'Required Ubuntu development packages: ' + ', '.join(missing or LINUX_PACKAGES)
    return {**paths, 'packages': list(LINUX_PACKAGES)}, ''


def _linux_readiness():
    supported, detail = linux_platform_probe()
    if not supported: return {'ready': False, 'reason': detail}
    toolchain, detail = linux_toolchain_probe()
    return {'ready': True, 'requires_build_tools': not bool(toolchain),
            'reason': 'Pinned Linux tools are available. ' + (detail + '. Build MAG will request system authorization to install these packages.' if not toolchain else 'Ubuntu development packages are installed.')}


def ensure_linux_build_tools(cache=None, token=None, events=None):
    """Build action: apt through the desktop PolicyKit authorization dialog."""
    token = token or Cancellation(); events = events or (lambda event: None)
    token.check()
    supported, detail = linux_platform_probe()
    if not supported: raise InstallerError(detail)
    toolchain, detail = linux_toolchain_probe()
    if toolchain: return toolchain
    if not Path('/usr/bin/pkexec').is_file():
        raise InstallerError(detail + '. Install them using Ubuntu Software or apt, then retry Build MAG (PolicyKit is unavailable).')
    events({'event': 'stage-start', 'stage': 'build-tools', 'message':
            'Ubuntu development packages are required. Approve the system authorization dialog. Packages: ' + ', '.join(LINUX_PACKAGES)})
    cache = Path(cache) if cache else Path(tempfile.gettempdir()) / 'mag-installer'
    cache.mkdir(parents=True, exist_ok=True)
    env = {key: value for key, value in os.environ.items() if key in ('DISPLAY', 'WAYLAND_DISPLAY', 'XDG_RUNTIME_DIR', 'DBUS_SESSION_BUS_ADDRESS', 'LANG')}
    env['PATH'] = '/usr/bin:/bin:/usr/sbin:/sbin'
    # Exact fixed argv; no shell, repository changes, or package names from input.
    run_command(['/usr/bin/pkexec', '/usr/bin/apt-get', 'install', '--yes', '--no-install-recommends', *LINUX_PACKAGES],
                cache, cache / 'ubuntu-build-tools.log', token, env, events, 'build-tools')
    toolchain, detail = linux_toolchain_probe()
    if not toolchain: raise InstallerError('Ubuntu development tools remain incomplete: ' + detail)
    return toolchain


def _bootstrap_linux(root, cache, token, events, build_dependencies):
    token = token or Cancellation(); events = events or (lambda event: None)
    root = Path(root).absolute()
    if root.is_symlink(): raise InstallerError('Managed root must not be a symlink')
    root.mkdir(parents=True, exist_ok=True)
    cache = Path(cache) if cache else root / 'cache'
    toolchain, detail = linux_toolchain_probe()
    if not toolchain: raise InstallerError(detail + '. Retry Build MAG to install Ubuntu development tools.')
    home = root / 'home'; home.mkdir(exist_ok=True)
    logs = root / 'logs'; logs.mkdir(exist_ok=True)
    paths = {'git': toolchain['git'], 'host': 'linux-x86_64'}; sites = []; python_root = None
    for key, spec in LINUX_LOCK.items():
        if key.startswith('source-') and not build_dependencies: continue
        location = materialize(spec, root / 'artifacts', cache, token, events)
        if key in ('python', 'cmake', 'ninja'):
            paths[key] = location / spec['executable']
            if key == 'python': python_root = location
        elif key.startswith('wheel-'): sites.append(location)
        elif key.startswith('source-'): paths[key] = location / spec['root']
    _install_wheels(python_root, sites, token)
    env = _env(paths['python'], paths['cmake'], paths['ninja'], home, sites)
    env.update(CC=toolchain['cc'], CXX=toolchain['cxx'])
    run_command([str(paths['python']), '-c', 'import Crypto,capstone,construct,tabulate,tqdm'], root,
                logs / 'python-modules.log', token, env, events, 'python-modules')
    prefix = root / 'prefix'
    if build_dependencies:
        marker = prefix / 'MANAGED-DEPENDENCIES.json'
        identity = {key: spec['sha256'] for key, spec in LINUX_LOCK.items()}
        if marker.exists():
            inventory = prefix / 'MANAGED-INVENTORY.json'
            if json.loads(marker.read_text()) != identity or not inventory.is_file() or json.loads(inventory.read_text()) != _inventory(prefix, token):
                raise InstallerError('Managed Linux dependency identity changed; use fresh managed storage')
        else:
            for stage in dependency_plan(paths, prefix):
                Path(stage['cwd']).mkdir(parents=True, exist_ok=True)
                events({'event': 'stage-start', 'stage': stage['id']})
                run_command(stage['argv'], stage['cwd'], logs / (stage['id'] + '.log'), token, env, events, stage['id'])
            missing = [str(p) for p in _dependency_paths(prefix, linux=True) if not p.exists()]
            if missing: raise InstallerError('Managed Linux dependencies did not produce: ' + ', '.join(missing))
            _write_json(marker, identity)
            _write_json(prefix / 'MANAGED-INVENTORY.json', _inventory(prefix, token))
    paths.update(dependencies_ready=build_dependencies, cmake_args=['-DCMAKE_PREFIX_PATH=' + str(prefix)],
                 python_site=[str(p) for p in sites], prefix=str(prefix), managed_prefix=str(prefix), logs=str(logs),
                 linux_toolchain={key: toolchain[key] for key in ('cc', 'cxx')},
                 ffmpeg={'include': str(prefix / 'include'), **{name: str(prefix / 'lib' / ('lib' + name + '.so.' + version))
                         for name, version in (('avcodec', '63'), ('avformat', '63'), ('avutil', '61'), ('swscale', '10'))}})
    return {key: str(value) if isinstance(value, Path) else value for key, value in paths.items()}
