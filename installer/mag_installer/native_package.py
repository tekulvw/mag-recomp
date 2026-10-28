"""Package private local native outputs into a relocatable macOS app.

Assets stay in the owned installation workspace; move the complete installation root.
This packages no account data and runs no game. Developer ID signing is deferred.
"""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess

from .model import InstallerError
from .safety import Cancellation, contained, file_digest


def _run_tool(argv, token):
    token.check()
    process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        while True:
            token.check()
            try:
                output, _ = process.communicate(timeout=.1)
                break
            except subprocess.TimeoutExpired:
                pass
    except BaseException:
        process.terminate()
        try: process.wait(timeout=2)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        raise
    if process.returncode:
        raise InstallerError(f'App packaging tool failed: {argv[0]}: {output[-2000:]}')
    return output


def _digest(path, token):
    return file_digest(path, token)[1]


def _ordinary(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file(): raise InstallerError(f'Package input must be an ordinary file: {path}')
    return path


def _dependencies(path, token):
    lines = _run_tool(['/usr/bin/otool', '-L', str(path)], token).splitlines()[1:]
    names = [line.strip().split(' (compatibility version', 1)[0] for line in lines if line.strip()]
    # otool -L includes LC_ID_DYLIB for dylibs; that identity is not a dependency.
    identifiers = _run_tool(['/usr/bin/otool', '-D', str(path)], token).splitlines()[1:]
    return [name for name in names if name not in {line.strip() for line in identifiers}]


def _rpaths(path, token):
    text = _run_tool(['/usr/bin/otool', '-l', str(path)], token)
    return re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset \d+\)', text)


def _resolve_dependency(name, source, executable, inherited, token):
    if name.startswith(('/usr/lib/', '/System/Library/')): return None
    def expand(value):
        return Path(value.replace('@loader_path', str(source.parent)).replace('@executable_path', str(executable.parent)))
    if name.startswith('@rpath/'):
        tail = name[len('@rpath/'):]
        candidates = [expand(p) / tail for p in [*_rpaths(source, token), *inherited]]
    else: candidates = [expand(name)]
    for candidate in candidates:
        if candidate.is_absolute() and candidate.is_file(): return candidate.resolve()
    raise InstallerError(f'Cannot resolve runtime library {name} required by {source.name}')


# A small Foundation supervisor avoids any Python/cache dependency at game launch.
# The contract mirrors src/retail_handoff.cpp and tools/handoff.py exactly.
_LAUNCHER = r'''
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
static void fail(NSString *s) { @throw [NSException exceptionWithName:@"MAGLaunch" reason:s userInfo:nil]; }
static NSData *readFile(NSString *p, NSUInteger limit) {
 int fd=open(p.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW);struct stat st;
 if(fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)||(limit && st.st_size>limit)) { if(fd>=0)close(fd);fail([@"Invalid local file: " stringByAppendingString:p]); }
 NSMutableData *d=[NSMutableData data];unsigned char b[65536];ssize_t n;
 while((n=read(fd,b,sizeof b))>0)[d appendBytes:b length:n];close(fd);if(n<0)fail(@"Cannot read local file");return d;
}
static NSString *hashData(NSData *d) { unsigned char out[32];CC_SHA256(d.bytes,(CC_LONG)d.length,out);NSMutableString *s=[NSMutableString string];for(int i=0;i<32;i++)[s appendFormat:@"%02x",out[i]];return s; }
static void check(NSString *p, NSString *sha) { if(![hashData(readFile(p,0)) isEqual:sha])fail([@"Local input identity changed: " stringByAppendingString:p]); }
static NSDictionary *json(NSString *p,NSUInteger limit) { id o=[NSJSONSerialization JSONObjectWithData:readFile(p,limit) options:0 error:nil];if(![o isKindOfClass:NSDictionary.class])fail(@"Invalid local manifest");return o; }
static uint64_t be(const unsigned char *p,int n) { uint64_t x=0;while(n--)x=(x<<8)|*p++;return x; }
static NSString *keyHash(NSString *p) {
 NSData *d=readFile(p,0);const unsigned char *b=d.bytes;
 if(d.length<64||memcmp(b,"\177ELF\2\2\1",7))fail(@"Invalid launcher ELF");
 uint64_t off=be(b+32,8),size=be(b+54,2),count=be(b+56,2);
 if(size!=56||off>d.length||count>(d.length-off)/size)fail(@"Invalid ELF program headers");
 for(uint64_t i=0;i<count;i++){const unsigned char *s=b+off+i*size;uint64_t file=be(s+8,8),va=be(s+16,8),len=be(s+32,8);
 if(be(s,4)==1&&va<=0x32A660&&len>=16&&0x32A670-va<=len){uint64_t at=file+0x32A660-va;if(at>d.length||d.length-at<16)fail(@"Invalid key segment");return hashData([d subdataWithRange:NSMakeRange(at,16)]);}}
 fail(@"Launcher key segment unavailable");return nil;
}
static int run(NSString *binary,NSString *elf,NSString *cwd,NSDictionary *env,NSString *log) {
 NSFileManager *fm=NSFileManager.defaultManager;[fm createFileAtPath:log contents:nil attributes:@{NSFilePosixPermissions:@0600}];
 NSFileHandle *f=[NSFileHandle fileHandleForWritingAtPath:log];NSTask *t=[NSTask new];t.executableURL=[NSURL fileURLWithPath:binary];t.arguments=@[elf];t.currentDirectoryURL=[NSURL fileURLWithPath:cwd];t.environment=env;t.standardOutput=f;t.standardError=f;
 NSError *e=nil;if(![t launchAndReturnError:&e])fail(e.localizedDescription);[t waitUntilExit];[f closeFile];return t.terminationReason==NSTaskTerminationReasonExit?t.terminationStatus:128+t.terminationStatus;
}
int main(int argc,char **argv) { @autoreleasepool {
 NSString *app=NSBundle.mainBundle.bundlePath.stringByResolvingSymlinksInPath;
 NSString *root=app.stringByDeletingLastPathComponent;
 @try {
 NSDictionary *m=json([app stringByAppendingPathComponent:@"Contents/Resources/runtime.json"],2097152);
 NSString *(^inApp)(NSString *)=^NSString *(NSString *p){return [app stringByAppendingPathComponent:p].stringByStandardizingPath;};
 NSString *workspace=[app stringByAppendingPathComponent:m[@"workspace"]].stringByStandardizingPath;
 NSString *vfs=[workspace stringByAppendingPathComponent:@"local/retail-v1.0"];
 NSString *launcher=inApp(m[@"launcher"]),*engine=inApp(m[@"engine"]);
 NSString *launcherElf=[workspace stringByAppendingPathComponent:@"local/retail-v1.0/PS3_GAME/USRDIR/EBOOT.elf"];
 NSString *engineElf=[workspace stringByAppendingPathComponent:@"local/retail-v1.0/PS3_GAME/USRDIR/bin/mage_g.elf"];
 NSString *engineSelf=[vfs stringByAppendingPathComponent:@"PS3_GAME/USRDIR/bin/mage_g.self"];
 NSDictionary *lr=json(inApp(@"Contents/Resources/package-receipts/launcher.json"),2097152),*er=json(inApp(@"Contents/Resources/package-receipts/engine.json"),2097152);
 if(![lr[@"image_name"] isEqual:@"EBOOT"]||![er[@"image_name"] isEqual:@"mage_g"])fail(@"Incorrect image receipts");
 check(launcher,lr[@"binary_sha256"]);check(engine,er[@"binary_sha256"]);check(launcherElf,lr[@"elf_sha256"]);check(engineElf,er[@"elf_sha256"]);check(engineSelf,m[@"engine_self_sha256"]);
 NSString *token=[NSUUID.UUID.UUIDString.lowercaseString stringByReplacingOccurrencesOfString:@"-" withString:@""];
 NSString *logs=[root stringByAppendingPathComponent:[@"logs/launch-" stringByAppendingString:token]];
 if(![NSFileManager.defaultManager createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:nil])fail(@"Cannot create launch log directory");
 NSString *request=[logs stringByAppendingPathComponent:@"handoff-request.json"];
 NSMutableDictionary *env=[NSMutableDictionary dictionaryWithDictionary:@{@"PATH":@"/usr/bin:/bin",@"HOME":[root stringByAppendingPathComponent:@"home"],@"LANG":@"en_US.UTF-8",@"PS3_VFS_ROOT":vfs,@"PS3_BOOT_PATH":@"/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN",@"PS3_HLE_UNRESOLVED":@"halt",@"PS3_SYSCALL_UNRESOLVED":@"halt",@"MAG_HANDOFF_REQUEST":request,@"MAG_HANDOFF_TOKEN":token,@"MAG_ENGINE_BINARY":engine,@"MAG_ENGINE_BINARY_SHA256":er[@"binary_sha256"],@"MAG_ENGINE_ELF":engineElf,@"MAG_ENGINE_ELF_SHA256":er[@"elf_sha256"],@"MAG_ENGINE_SELF":engineSelf,@"MAG_ENGINE_SELF_SHA256":m[@"engine_self_sha256"],@"MAG_ENGINE_KEY_SHA256":keyHash(launcherElf)}];
 NSString *tmp=NSTemporaryDirectory();if(tmp)env[@"TMPDIR"]=tmp;
 int result=run(launcher,launcherElf,logs,env,[logs stringByAppendingPathComponent:@"launcher.log"]);
 if(result!=90)return result;
 NSDictionary *expected=@{@"version":@1,@"token":token,@"guest_path":@"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self",@"kind":@"mage_g"};
 if(![json(request,4096) isEqual:expected])fail(@"Engine handoff request identity mismatch");
 check(engine,er[@"binary_sha256"]);check(engineElf,er[@"elf_sha256"]);check(engineSelf,m[@"engine_self_sha256"]);
 for(NSString *k in env.allKeys.copy)if([k hasPrefix:@"MAG_HANDOFF_"]||[k hasPrefix:@"MAG_ENGINE_"])[env removeObjectForKey:k];
 env[@"PS3_BOOT_PATH"]=@"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self";
 return run(engine,engineElf,logs,env,[logs stringByAppendingPathComponent:@"engine.log"]);
 } @catch(NSException *e) {
 NSString *message=[NSString stringWithFormat:@"MAG could not start: %@\n",e.reason];
 [message writeToFile:[root stringByAppendingPathComponent:@"launch-error.log"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
 fprintf(stderr,"%s",message.UTF8String);return 1;
 }
} }
'''


def package(config, workspace, destination, token=None, events=None):
    """Build MAG.app above its owned workspace without launching any executable.

    The app + descendant workspace form the local install. No cache/source-toolchain
    paths enter the launcher. Existing destinations are never overwritten.
    """
    if config.get('host') == 'linux-x86_64':
        from .native_package_linux import package as package_linux
        return package_linux(config, workspace, destination, token=token, events=events)
    if 'msvc' in config:
        from .native_package_windows import package as package_windows
        return package_windows(config, workspace, destination, token=token, events=events)
    token = token or Cancellation(); events = events or (lambda event: None)
    workspace, destination = Path(workspace).absolute(), Path(destination).absolute()
    if destination.exists() or destination.is_symlink(): raise InstallerError('App destination already exists')
    if destination.suffix != '.app' or not destination.parent.is_dir(): raise InstallerError('Choose a new .app under the installation directory')
    root = destination.parent.resolve()
    workspace_real = workspace.resolve()
    if workspace.is_symlink() or not workspace.is_dir() or workspace_real == root or not workspace_real.is_relative_to(root):
        raise InstallerError('Owned workspace must be an ordinary descendant of the installation folder')
    # Reject symlink directory components even if they resolve back under root.
    relative = workspace.relative_to(destination.parent)
    if contained(destination.parent, str(relative)).resolve() != workspace_real:
        raise InstallerError('Workspace path changed during installation validation')
    if workspace_real.is_relative_to(destination.resolve()) or destination.resolve().is_relative_to(workspace_real):
        raise InstallerError('App and workspace must be separate installation paths')
    manifest = json.loads(contained(workspace, 'profiles/retail-v1.0.json').read_text(encoding='utf-8'))
    framework = json.loads(contained(workspace, 'profiles/framework.json').read_text(encoding='utf-8'))
    records, originals = {}, {}
    for role, folder, image in [('launcher', 'build', 'EBOOT'), ('engine', 'build-engine', 'mage_g')]:
        binary = _ordinary(contained(workspace, folder + '/MagRecomp'))
        receipt = _ordinary(binary.with_suffix('.receipt.json'))
        record = json.loads(receipt.read_text()); expected = manifest['images'][image]
        if (record.get('binary_sha256') != _digest(binary, token) or record.get('elf_sha256') != expected['elf_sha256'] or record.get('image_name') != image):
            raise InstallerError(f'Native source receipt mismatch: {receipt}')
        if record.get('sdk_revision') not in set(framework['strict_runtime_revisions']) & set(framework['strict_hle_runtime_revisions']):
            raise InstallerError('Packaged runtime must support strict syscall/HLE policies')
        relative = 'local/retail-v1.0/PS3_GAME/USRDIR/' + ('EBOOT.elf' if role == 'launcher' else 'bin/mage_g.elf')
        elf = _ordinary(contained(workspace, relative))
        if _digest(elf, token) != record['elf_sha256']:
            raise InstallerError(f'Native runtime ELF identity mismatch: {elf}')
        records[role], originals[role] = record, (binary, receipt)
    destination.mkdir(mode=0o700)
    try:
        macos = destination/'Contents/MacOS'; macos.mkdir(parents=True)
        resources = destination/'Contents/Resources'; resources.mkdir()
        libs = destination/'Contents/Frameworks'; libs.mkdir()
        receipts = resources/'source-receipts'; receipts.mkdir()
        package_receipts = resources/'package-receipts'; package_receipts.mkdir()
        copied, edges, library_names = {}, {}, {}
        for role, name in [('launcher', 'MagRecomp'), ('engine', 'MagEngine')]:
            source, receipt = originals[role]; target = macos/name
            shutil.copyfile(source, target); target.chmod(0o755)
            shutil.copyfile(receipt, receipts/(role+'.json'))
            copied[source.resolve()] = target
        queue = [(source.resolve(), source.resolve(), []) for source, _ in originals.values()]
        scanned = set()
        while queue:
            source, executable, inherited = queue.pop(0)
            if source in scanned: continue
            scanned.add(source); token.check()
            inherited = [*(p.replace('@loader_path', str(source.parent)).replace('@executable_path', str(executable.parent))
                           for p in _rpaths(source, token)), *inherited]
            edges[source] = []
            for dependency in _dependencies(source, token):
                resolved = _resolve_dependency(dependency, source, executable, inherited, token)
                if resolved is None or resolved == source: continue
                if resolved not in copied:
                    name = resolved.name
                    if name in library_names and library_names[name] != resolved:
                        raise InstallerError('Runtime library basename collision: '+name)
                    library_names[name] = resolved; target = libs/name
                    shutil.copyfile(resolved, target); target.chmod(0o755)
                    copied[resolved] = target; queue.append((resolved, executable, inherited))
                edges[source].append((dependency, resolved))
        adjustments = []
        for source, target in copied.items():
            if target.parent == libs:
                _run_tool(['/usr/bin/install_name_tool', '-id', '@rpath/'+target.name, str(target)], token)
            for original, resolved in edges[source]:
                replacement = ('@loader_path/' if target.parent == libs else '@executable_path/../Frameworks/') + copied[resolved].name
                _run_tool(['/usr/bin/install_name_tool', '-change', original, replacement, str(target)], token)
                adjustments.append({'file':str(target.relative_to(destination)), 'from':original, 'to':replacement})
            _run_tool(['/usr/bin/codesign', '--force', '--sign', '-', str(target)], token)
        runtime = {'schema_version':1,'workspace':os.path.relpath(workspace,destination),
                   'launcher':'Contents/MacOS/MagRecomp','engine':'Contents/MacOS/MagEngine',
                   'engine_self_sha256':manifest['images']['mage_g']['source_sha256'],
                   'signing':'local-ad-hoc','adjustments':adjustments}
        for role, name in [('launcher','MagRecomp'),('engine','MagEngine')]:
            target=macos/name
            # A distinct package receipt binds transformed bytes and points to
            # the immutable source receipt; it does not rewrite the source build.
            record = {key:records[role][key] for key in ('image_name','elf_sha256','sdk_revision')}
            record.update(binary_sha256=_digest(target,token),kind='local-packaged-native',
                          source_receipt_sha256=_digest(receipts/(role+'.json'),token),
                          original_binary_sha256=records[role]['binary_sha256'])
            (package_receipts/(role+'.json')).write_text(json.dumps(record,indent=2)+'\n')
        (resources/'runtime.json').write_text(json.dumps(runtime,indent=2)+'\n')
        source=resources/'launcher.m'; source.write_text(_LAUNCHER)
        supervisor=macos/'MAG'
        _run_tool(['/usr/bin/xcrun','clang','-x','objective-c','-fobjc-arc','-framework','Foundation',str(source),'-o',str(supervisor)],token)
        _ordinary(supervisor).chmod(0o755); source.unlink()
        with (destination/'Contents/Info.plist').open('wb') as stream:
            plistlib.dump({'CFBundleExecutable':'MAG','CFBundleName':'MAG','CFBundleDisplayName':'MAG',
                           'CFBundleIdentifier':'local.mag.native','CFBundlePackageType':'APPL',
                           'CFBundleShortVersionString':'0.1','LSMinimumSystemVersion':'12.0',
                           'NSHighResolutionCapable':True},stream)
        _run_tool(['/usr/bin/codesign','--force','--sign','-',str(destination)],token)
        events({'event':'app-packaged','output':str(destination),'launcher':str(supervisor),
                'message':'Local MAG app packaged. Move the complete installation folder with its workspace.'})
        return destination
    except BaseException:
        shutil.rmtree(destination)
        raise
