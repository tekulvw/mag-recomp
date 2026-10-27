"""Maintainer-only, source-only local kit preparation. Never publishes a kit."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import time

from .model import InstallerError
from .native_build import _ordinary, _source_files, _files
from .native_recipe import ROLES, validate_source_tree
from .safety import Cancellation, file_digest


def _git(argv, cwd, env, token):
    """Only local Git operations, with cancellable owned process cleanup."""
    token.check()
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        try:
            process = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                       stdout=stdout, stderr=stderr)
            try:
                deadline = time.monotonic() + 300
                while process.poll() is None:
                    token.check()
                    if time.monotonic() > deadline:
                        raise InstallerError('Local SDK Git operation exceeded its five-minute deadline')
                    try: process.wait(timeout=0.1)
                    except subprocess.TimeoutExpired: pass
                token.check()
            except BaseException:
                process.terminate()
                try: process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill();process.wait()
                raise
            if process.returncode:
                stderr.seek(0)
                raise InstallerError('Local SDK Git operation failed: ' + stderr.read(4096).decode('utf-8',errors='replace'))
            stdout.seek(0)
            return stdout.read().decode('utf-8').strip()
        except OSError as exc:
            raise InstallerError(f'Cannot run local Git: {exc}') from exc


def _copy(source, target, token):
    target.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    size = 0
    with source.open('rb') as incoming, target.open('xb') as outgoing:
        while True:
            token.check()
            block = incoming.read(1024*1024)
            if not block: break
            digest.update(block);size += len(block);outgoing.write(block)
    target.chmod(0o755 if source.stat().st_mode & 0o111 else 0o644)
    return {'size':size,'sha256':digest.hexdigest()}


def prepare(source_tree, sdks, output_archive, token=None, events=None):
    """Prepare an owned local archive, without reading game inputs or publishing.

    The returned descriptor can be embedded in a separately reviewed maintained
    release manifest. It intentionally contains no download URL or local paths.
    SDK identities are caller-selected exact Git commits. Source uses the same
    bounded source allowlist as native builds, excluding runs/accounts/assets.
    """
    token = token or Cancellation();events = events or (lambda event:None)
    token.check()
    source = _ordinary(source_tree, True)
    output = Path(output_archive).expanduser().absolute()
    if output.exists() or output.is_symlink():
        raise InstallerError('Kit output already exists; choose a new archive path')
    _ordinary(output.parent,True)
    if output.is_relative_to(source):
        raise InstallerError('Kit output must be outside the source tree')
    if not isinstance(sdks,dict) or set(sdks) != set(ROLES):
        raise InstallerError('Kit needs exactly the runtime, lifter-spu, lifter-ppu and spu-programs SDK identities')
    selected={}
    for role in ROLES:
        record=sdks[role]
        if not isinstance(record,dict) or set(record) != {'path','revision'} or re.fullmatch('[0-9a-f]{40}',str(record.get('revision',''))) is None:
            raise InstallerError(f'SDK {role} requires a local path and exact lowercase full revision')
        path=_ordinary(record['path'],True)
        if output.is_relative_to(path):raise InstallerError('Kit output must be outside SDK source trees')
        selected[role]={'path':path,'revision':record['revision']}
    pins = validate_source_tree(source)
    for role in ROLES:
        if selected[role]['revision'] != pins[role]['commit']:
            raise InstallerError(f'SDK {role} must be the commit pinned in profiles/framework.json ({pins[role]["tag"]})')
    files=list(_source_files(source,token))
    git=shutil.which('git')
    if not git:raise InstallerError('Maintainer kit preparation requires local Git')
    with tempfile.TemporaryDirectory(prefix='.mag-kit-',dir=output.parent) as temporary:
        temp=Path(temporary);kit=temp/'kit';kit.mkdir()
        home=temp/'home';home.mkdir()
        env={'PATH':str(Path(git).parent)+os.pathsep+'/usr/bin:/bin', 'HOME':str(home),
             'TMPDIR':str(home),'GIT_CONFIG_NOSYSTEM':'1','GIT_CONFIG_GLOBAL':os.devnull,
             'GIT_OPTIONAL_LOCKS':'0','GIT_TERMINAL_PROMPT':'0','GIT_ALLOW_PROTOCOL':'file',
             'GIT_CONFIG_COUNT':'2','GIT_CONFIG_KEY_0':'core.hooksPath','GIT_CONFIG_VALUE_0':os.devnull,
             'GIT_CONFIG_KEY_1':'init.templateDir','GIT_CONFIG_VALUE_1':str(home/'empty-template')}
        (home/'empty-template').mkdir()
        # A kit carries tracked source only: refuse untracked scratch, captures or
        # output that happen to sit inside the allowlisted directories.
        if (source/'.git').exists():
            listing=_git([git,'-C',str(source),'ls-files','-z','--cached'],temp,env,token)
            tracked={name for name in listing.split('\0') if name}
            untracked=sorted(path.relative_to(source).as_posix() for path in files
                             if path.relative_to(source).as_posix() not in tracked)
            if untracked:
                raise InstallerError('Kit source has untracked files in its allowlisted directories; remove or commit them: '+', '.join(untracked[:10]))
        inventory=[]
        events({'event':'stage-start','stage':'copy-kit-source'})
        for path in sorted(files):
            relative=path.relative_to(source).as_posix()
            inventory.append({'path':relative,**_copy(path,kit/'source'/relative,token)})
        for role in ROLES:
            token.check();sdk=selected[role];clone=kit/'sdks'/role;clone.mkdir(parents=True)
            events({'event':'stage-start','stage':f'pin-sdk-{role}'})
            _git([git,'init','--quiet',str(clone)],temp,env,token)
            _git([git,'-C',str(clone),'-c','protocol.file.allow=always','fetch','--quiet','--no-tags','--depth=1','--',str(sdk['path']),sdk['revision']],temp,env,token)
            _git([git,'-C',str(clone),'-c','advice.detachedHead=false','checkout','--quiet','--detach',sdk['revision']],temp,env,token)
            if _git([git,'-C',str(clone),'rev-parse','HEAD'],temp,env,token) != sdk['revision']:
                raise InstallerError(f'SDK {role} checkout identity mismatch')
            tracked=_git([git,'-C',str(clone),'ls-files','--stage'],temp,env,token)
            if any(line.startswith('160000 ') for line in tracked.splitlines()):
                raise InstallerError(f'SDK {role} contains submodules; a complete curated source kit is required')
            # Scrub local fetch provenance only in the newly owned checkout.
            (clone/'.git/FETCH_HEAD').unlink(missing_ok=True)
            shutil.rmtree(clone/'.git/logs',ignore_errors=True)
        config={'schema_version':1,'source_tree':'source','sdks':{role:{'path':f'sdks/{role}','revision':selected[role]['revision'],'tag':pins[role]['tag']} for role in ROLES},'jobs':2,'native_graphics':False}
        manifest={**config,'source_bytes':sum(item['size'] for item in inventory),'source_inventory':inventory}
        (kit/'KIT.json').write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n',encoding='utf-8')
        regular_files=list(_files(kit,token))
        # Empty .git/refs directories are required for Git repository recognition.
        members=sorted(kit.rglob('*'))
        expanded=sum(path.stat().st_size for path in regular_files)
        events({'event':'stage-start','stage':'archive-source-kit'})
        archive=temp/'kit.tar.gz'
        with tarfile.open(archive,'w:gz',format=tarfile.PAX_FORMAT) as stream:
            for path in members:
                token.check()
                info=stream.gettarinfo(str(path),arcname=path.relative_to(kit).as_posix())
                info.uid=info.gid=0;info.uname=info.gname='';info.mtime=0
                if path.is_dir():
                    stream.addfile(info)
                else:
                    with path.open('rb') as content:stream.addfile(info,content)
        size,digest=file_digest(archive,token)
        descriptor={'schema_version':1,'sha256':digest,'size':size,'format':'tar.gz',
                    'max_expanded_size':expanded,'max_entries':len(members),'source_bytes':manifest['source_bytes'],'kit':config}
        token.check()
        # Link publishes a complete archive without replacing an existing path.
        try:os.link(archive,output)
        except FileExistsError as exc:raise InstallerError('Kit output appeared during preparation; existing archive preserved') from exc
        events({'event':'native-kit-prepared','output':str(output),'source_bytes':manifest['source_bytes'],'published':False})
        return descriptor


def pinned_sdks(source):
    """The checkouts tools/fetch_ps3recomp.py created, at their pinned paths."""
    from .native_recipe import framework_pins
    return {role: {'path': Path(source) / pin['path'], 'revision': pin['commit']}
            for role, pin in framework_pins(source).items()}


def main(argv=None):
    parser=argparse.ArgumentParser(description='Maintainer-only LOCAL source kit preparation. Does not publish, read game inputs or build MAG.')
    parser.add_argument('--source',type=Path,required=True,help='repository checkout after tools/fetch_ps3recomp.py')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args(argv)
    try:
        print(json.dumps(prepare(args.source,pinned_sdks(args.source),args.output),indent=2))
        return 0
    except (InstallerError,OSError) as exc:
        print(str(exc),file=__import__('sys').stderr);return 2
