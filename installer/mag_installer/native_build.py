"""Private experimental local native builds; never launches the resulting game."""
import hashlib
import ast
import json
import os
from pathlib import Path
import stat
import subprocess
import shutil
import tempfile

from .model import InstallerError, host_key
from .pipeline import _exclusive_lock, _write_json, run_command
from .safety import Cancellation

SOURCE_DIRS = ('src', 'tools', 'cmake')
SOURCE_FILES = ('CMakeLists.txt', 'LICENSE', 'LICENSE.ps3recomp', 'NOTICE.md')
EXCLUDED = {'.git', '__pycache__', 'target', 'node_modules', '.venv'}


def _ordinary(path, directory=None):
    path = Path(path).absolute()
    for part in (path, *path.parents):
        if part.is_symlink():
            raise InstallerError(f'Symlink input is unsupported: {part}')
    if not path.exists() or (directory is True and not path.is_dir()) or (directory is False and not path.is_file()):
        raise InstallerError(f'Required {"directory" if directory else "file"} is missing: {path}')
    if not (path.is_file() or path.is_dir()):
        raise InstallerError(f'Input must be an ordinary file or directory: {path}')
    return path


def _files(root, token=None, exclude=()):
    root = _ordinary(root, True)
    for base, directories, files in os.walk(root, followlinks=False):
        directories[:] = [d for d in directories if d not in exclude]
        for name in directories + files:
            path = Path(base) / name
            if token: token.check()
            if path.is_symlink() or not (stat.S_ISDIR(path.lstat().st_mode) or stat.S_ISREG(path.lstat().st_mode)):
                raise InstallerError(f'Nonordinary source member: {path}')
        for name in files:
            yield Path(base) / name


def _source_files(root, token=None):
    for name in SOURCE_FILES:
        if (root / name).exists(): yield _ordinary(root / name, False)
    for name in SOURCE_DIRS:
        yield from _files(root / name, token, EXCLUDED)
    for name in ('profiles',):
        for path in (root / name).glob('*.json'):
            # Written per machine from the player's files (prepare_inputs, inventory).
            if 'network' in path.name or 'account' in path.name or path.name in ('retail-v1.0.json', 'retail-v1.0-inventory.json'):
                continue
            yield _ordinary(path, False)


def _probe(argv, env):
    try:
        return subprocess.run([str(x) for x in argv], env=env, stdin=subprocess.DEVNULL,
                              capture_output=True, text=True, check=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError) as exc:
        raise InstallerError(f'Prerequisite command failed: {argv[0]} {argv[1:]}; {str(getattr(exc, "stderr", ""))[-1500:]}') from exc


SUPPORTED_HOSTS = ('macos-arm64', 'windows-x86_64')
# Build trees nest the output path twice in some object paths; keep Windows
# paths well inside MAX_PATH instead of failing deep inside a compile.
WINDOWS_MAX_OUTPUT_PATH = 100


def check_windows_location(output):
    """The runtime opens guest files and its ELF argument through narrow APIs."""
    text = str(output)
    if not text.isascii():
        raise InstallerError('Choose an installation location whose path uses only English letters, digits and punctuation (for example C:\\MAG). The game runtime cannot open paths with other characters yet.')
    if len(text) > WINDOWS_MAX_OUTPUT_PATH:
        raise InstallerError(f'Choose a shorter installation location (at most {WINDOWS_MAX_OUTPUT_PATH} characters, for example C:\\MAG): Windows build paths would exceed their length limit.')


def _windows_environment(config, home):
    from .managed_tools import msvc_environment
    environment = msvc_environment(config['msvc']['vcvars'])
    # Windows variable names are case-insensitive; `set` prints Path.
    system_path = ''
    for key in [k for k in environment if k.upper() in ('PATH', 'PYTHONPATH', 'PYTHONHOME')]:
        value = environment.pop(key)
        if key.upper() == 'PATH': system_path = value
    tools = [str(Path(config[k]).parent) for k in ('python', 'cmake', 'ninja', 'git')]
    tools.append(str(Path(config['msvc']['clang_cl']).parent))
    environment.update({'PATH': os.pathsep.join([*dict.fromkeys(tools), system_path]),
                        'TEMP': str(home), 'TMP': str(home), 'PYTHONNOUSERSITE': '1', 'PYTHONUTF8': '1',
                        'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': os.devnull,
                        'GIT_OPTIONAL_LOCKS': '0', 'GIT_TERMINAL_PROMPT': '0',
                        'CMAKE_BUILD_PARALLEL_LEVEL': str(config.get('jobs', 2))})
    return environment


def _environment(config, home):
    if 'msvc' in config:
        return _windows_environment(config, home)
    tool_dirs = {str(Path(config[k]).parent) for k in ('python', 'cmake', 'ninja', 'git')}
    if config.get('managed_prefix'):
        tool_dirs.add(str(Path(config['managed_prefix']) / 'bin'))
    return {'PATH': os.pathsep.join(sorted(tool_dirs)) + os.pathsep + '/usr/bin:/bin:/usr/sbin:/sbin',
            'HOME': str(home), 'TMPDIR': str(home), 'PYTHONNOUSERSITE': '1',
            'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': os.devnull,
            'GIT_OPTIONAL_LOCKS': '0', 'GIT_TERMINAL_PROMPT': '0', 'CMAKE_BUILD_PARALLEL_LEVEL': str(config.get('jobs', 2))}


def _verify_hash(path, expected, token=None):
    with Path(path).open('rb') as stream:
        digest = hashlib.sha256()
        while True:
            if token: token.check()
            chunk = stream.read(1024 * 1024)
            if not chunk: break
            digest.update(chunk)
        actual = digest.hexdigest()
    if actual != expected:
        raise InstallerError(f'Input identity does not match the reviewed native profile: {path}')


def _verify_identities_unwrapped(source, disc, config, token=None):
    module = ast.parse((source / 'tools/prepare_inputs.py').read_text(encoding='utf-8'))
    retail = next((ast.literal_eval(node.value) for node in module.body
                   if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'RETAIL' for t in node.targets)), None)
    if not isinstance(retail, dict): raise InstallerError('Native source has no literal reviewed RETAIL input identities')
    for name, key in (('EBOOT.BIN', 'eboot_elf'), ('bin/mage_g.self', 'engine_elf')):
        original, decrypted = retail[name]
        _verify_hash(disc / 'USRDIR' / name, original, token)
        _verify_hash(config[key], decrypted, token)
    firmware = json.loads((source / 'profiles/retail-v1.0-firmware.json').read_text(encoding='utf-8'))
    _verify_hash(config['firmware'], firmware['image_sha256'], token)


def _verify_identities(source, disc, config, token=None):
    try:
        _verify_identities_unwrapped(source, disc, config, token)
    except (SyntaxError, ValueError, TypeError, KeyError, StopIteration) as exc:
        raise InstallerError(f'Native input identity metadata is malformed: {exc}') from exc


def preflight(config_path, output, token=None):
    from .native_recipe import load_config, validate_source_tree
    token = token or Cancellation()
    token.check()
    config = load_config(config_path)
    host = host_key()
    if host not in SUPPORTED_HOSTS:
        raise InstallerError('Native MAG builds support Apple Silicon Macs and 64-bit Windows PCs')
    if (host == 'windows-x86_64') != ('msvc' in config):
        raise InstallerError('Native build configuration does not match this host toolchain')
    output = Path(output).absolute()
    if host == 'windows-x86_64':
        check_windows_location(output)
    _ordinary(output.parent, True)
    if output.exists() or output.is_symlink():
        raise InstallerError('Native build destination already exists; choose a new destination')
    source = _ordinary(config['source_tree'], True)
    validate_source_tree(source)
    disc = _ordinary(config['disc_root'], True)
    if (disc / 'PS3_GAME').is_dir(): disc = _ordinary(disc / 'PS3_GAME', True)
    for name in ('USRDIR/EBOOT.BIN', 'USRDIR/bin/mage_g.self', 'PARAM.SFO'):
        _ordinary(disc / name, False)
    inputs = [source, disc, _ordinary(config_path, False)]
    for key in ('eboot_elf', 'engine_elf', 'firmware', 'python', 'cmake', 'ninja', 'git'):
        inputs.append(_ordinary(config[key], False))
    for key, value in config.get('ffmpeg', {}).items():
        inputs.append(_ordinary(value, key == 'include'))
    _verify_identities(source, disc, config, token)
    if config.get('native_graphics', False):
        raise InstallerError('Experimental local build supports only the legacy renderer; current private native graphics registrations are not configured')
    files = list(_source_files(source, token))
    disc_files = list(_files(disc, token))
    known_bytes = sum(p.stat().st_size for p in files + disc_files) + sum(Path(config[k]).stat().st_size for k in ('eboot_elf', 'engine_elf', 'firmware'))
    free_bytes = shutil.disk_usage(output.parent).free
    if free_bytes < known_bytes:
        raise InstallerError('Insufficient free space even for known native source/disc copies; generated/build space is additional')
    if 'msvc' in config:
        env = _environment(config, Path(tempfile.gettempdir()))
        _probe([config['msvc']['clang_cl'], '--version'], env)
    else:
        env = _environment(config, Path(tempfile.gettempdir()))
        _probe(['/usr/bin/xcrun', '--find', 'clang'], env)
        _probe(['/usr/bin/xcrun', '--show-sdk-path'], env)
    _probe([config['python'], '-c', 'import sys,json,hashlib,struct,concurrent.futures,Crypto,capstone,construct,tabulate,tqdm; assert sys.version_info >= (3,11), "Python 3.11+ required"'], env)
    for tool in ('cmake', 'ninja', 'git'): _probe([config[tool], '--version'], env)
    from .native_recipe import framework_pins
    pins = framework_pins(source)
    for role, sdk in config['sdks'].items():
        if sdk['revision'] != pins[role]['commit']:
            raise InstallerError(f'SDK {role} must be the commit pinned in profiles/framework.json ({pins[role]["tag"]})')
        tree = _ordinary(sdk['path'], True); inputs.append(tree)
        revision = _probe([config['git'], '-C', tree, 'rev-parse', sdk['revision'] + '^{commit}'], env)
        if revision != sdk['revision']:
            raise InstallerError(f'SDK {role} revision must be an exact full commit identity')
        # Kit repositories are created on POSIX; Windows extraction has no
        # executable bits, so compare content there, not file modes.
        mode = ['-c', 'core.filemode=false'] if 'msvc' in config else []
        if _probe([config['git'], '-C', tree, *mode, 'status', '--porcelain', '--untracked-files=no'], env):
            raise InstallerError(f'SDK {role} has tracked changes; provide a clean local checkout')
    for path in inputs:
        if output == path or output.is_relative_to(path) or path.is_relative_to(output):
            raise InstallerError(f'Native destination overlaps source input: {path}')
    return {'event': 'native-preflight', 'experimental': True, 'config': config,
            'disc_root': str(disc), 'required_bytes': known_bytes, 'free_bytes': free_bytes,
            'estimate_complete': False, 'message': 'Known copy size only; SDK clones, generated sources and build scratch are additional. Existing platform compiler toolchain required.'}


def binary_name():
    return 'MagRecomp.exe' if host_key().startswith('windows-') else 'MagRecomp'


def _copy_file(source, destination, token):
    _ordinary(source, False)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with source.open('rb') as incoming, destination.open('xb') as outgoing:
        while True:
            token.check()
            chunk = incoming.read(1024 * 1024)
            if not chunk: break
            outgoing.write(chunk)


def _validate_receipts(workspace, runtime_revision, token=None):
    manifest = json.loads((workspace / 'profiles/retail-v1.0.json').read_text(encoding='utf-8'))
    results = []
    for name, image in (('build', 'EBOOT'), ('build-engine', 'mage_g')):
        binary = _ordinary(workspace / name / binary_name(), False)
        receipt = _ordinary(binary.with_suffix('.receipt.json'), False)
        record = json.loads(receipt.read_text(encoding='utf-8'))
        with binary.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        expected = manifest['images'][image]
        elf = _ordinary(expected['elf'], False)
        if not elf.is_relative_to(workspace): raise InstallerError('Receipt ELF must remain in the owned workspace')
        _verify_hash(elf, expected['elf_sha256'], token)
        if record.get('image_name') != image or record.get('binary_sha256') != digest or record.get('sdk_revision') != runtime_revision or record.get('elf_sha256') != expected['elf_sha256']:
            raise InstallerError(f'Native output receipt is missing or mismatched: {receipt}')
        results.append(str(binary))
    return results


def install(config_path, output, resume=False, token=None, events=None):
    from .native_recipe import build_plan, framework_pins
    if resume:
        raise InstallerError('Experimental native build resume is unsupported; retained logs explain the failure. Choose a new destination.')
    token = token or Cancellation()
    events = events or (lambda event: None)
    report = preflight(config_path, output, token=token)
    config = report['config']; output = Path(output).absolute()
    lock = output.with_name('.' + output.name + '.native-lock')
    with _exclusive_lock(lock, False, events):
        output.mkdir(mode=0o700)
        journal = output / 'native-build.json'
        state = {'schema_version': 1, 'kind': 'experimental-private-native-build', 'status': 'incomplete', 'output': str(output)}
        _write_json(journal, state)
        logs = output / 'logs'; logs.mkdir()
        home = output / 'home'; home.mkdir()
        env = _environment(config, home)
        workspace = output / 'workspace'; workspace.mkdir()
        try:
            source = Path(config['source_tree'])
            events({'event': 'stage-start', 'stage': 'copy-source'})
            for path in _source_files(source, token):
                _copy_file(path, workspace / path.relative_to(source), token)
            disc = Path(report['disc_root'])
            events({'event': 'stage-start', 'stage': 'copy-inputs'})
            # The player layout keeps the decrypted ELFs inside PS3_GAME; they are
            # copied once, explicitly, to their fixed workspace names below.
            prepared = {Path(config[key]).resolve() for key in ('eboot_elf', 'engine_elf')}
            for path in _files(disc, token):
                if path.resolve() in prepared: continue
                _copy_file(path, workspace / 'local/retail-v1.0/PS3_GAME' / path.relative_to(disc), token)
            for key, relative in (('eboot_elf', 'EBOOT.elf'), ('engine_elf', 'bin/mage_g.elf')):
                _copy_file(Path(config[key]), workspace / 'local/retail-v1.0/PS3_GAME/USRDIR' / relative, token)
            _copy_file(Path(config['firmware']), workspace / 'local/firmware/libsre.prx', token)
            # Each checkout goes to its pinned repository-relative path, where the
            # profiles and tools look for it (as tools/fetch_ps3recomp.py lays it out).
            for role, pin in framework_pins(workspace).items():
                if not role.replace('-', '').replace('_', '').isalnum(): raise InstallerError('Invalid SDK role')
                sdk = config['sdks'][role]; destination = workspace / pin['path']
                destination.parent.mkdir(parents=True, exist_ok=True)
                run_command([config['git'], 'clone', '--no-hardlinks', '--no-checkout', '--', sdk['path'], str(destination)], output, logs / f'clone-{role}.log', token, env, events, 'clone-' + role)
                run_command([config['git'], '-C', str(destination), 'checkout', '--detach', sdk['revision']], output, logs / f'checkout-{role}.log', token, env, events, 'checkout-' + role)
            for stage in build_plan(config, workspace):
                token.check(); stage_id = stage['id']
                if not stage_id.replace('-', '').replace('_', '').isalnum(): raise InstallerError('Invalid native stage identifier')
                events({'event': 'stage-start', 'stage': stage_id})
                run_command(stage['argv'], workspace, logs / (stage_id + '.log'), token, env, events, stage_id)
            binaries = _validate_receipts(workspace, config['sdks']['runtime']['revision'], token)
            token.check()
            state.update(status='complete', binaries=binaries)
            _write_json(journal, state)
            events({'event': 'experimental-local-build-complete', 'output': str(output), 'logs': str(logs), 'message': 'Experimental local native build complete. No game was launched; gameplay and portable distribution are unverified.'})
            return output
        except BaseException as exc:
            state.update(status='incomplete', error=str(exc), logs=str(logs))
            _write_json(journal, state)
            raise
