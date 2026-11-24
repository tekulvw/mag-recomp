"""Package private local native outputs into a self-contained, relocatable MAG folder.

The MAG folder holds MAG.app (MAG\\MAG.exe on Windows, MAG/MAG on Linux), the game files the
game reads (game/, hard-linked from the build workspace where the volume allows) and, once
played, home/ and logs/. Nothing in it refers to the build workspace, so it keeps working when
it is moved or the build folder is deleted. This packages no account data and runs no game.
Developer ID signing is deferred.
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
# The contract mirrors src/retail_handoff.cpp and tools/handoff.py exactly. It is Objective-C++ so
# it can end each session with the shared performance and error reports (src/player_reports.h).
_LAUNCHER = r'''
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CommonCrypto/CommonDigest.h>
#include "player_reports.h"
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
 NSData *d=readFile(p,0);const unsigned char *b=(const unsigned char *)d.bytes;
 if(d.length<64||memcmp(b,"\177ELF\2\2\1",7))fail(@"Invalid launcher ELF");
 uint64_t off=be(b+32,8),size=be(b+54,2),count=be(b+56,2);
 if(size!=56||off>d.length||count>(d.length-off)/size)fail(@"Invalid ELF program headers");
 for(uint64_t i=0;i<count;i++){const unsigned char *s=b+off+i*size;uint64_t file=be(s+8,8),va=be(s+16,8),len=be(s+32,8);
 if(be(s,4)==1&&va<=0x32A660&&len>=16&&0x32A670-va<=len){uint64_t at=file+0x32A660-va;if(at>d.length||d.length-at<16)fail(@"Invalid key segment");return hashData([d subdataWithRange:NSMakeRange(at,16)]);}}
 fail(@"Launcher key segment unavailable");return nil;
}
// After the game exits: the note the game's ticket refill left when a sign-in was refused or
// expired and never recovered (src/player_portal.h), shown once; automation only logs it.
static void signInNotice(NSString *owned,NSString *logs,BOOL quiet) {
 if(!owned)return;
 NSString *path=[owned stringByAppendingPathComponent:@"sign-in-notice.txt"];
 NSString *text=[[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil]
                 stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
 [NSFileManager.defaultManager removeItemAtPath:path error:nil];
 if(!text.length)return;
 [[text stringByAppendingString:@"\n"] writeToFile:[logs stringByAppendingPathComponent:@"sign-in-notice.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
 fprintf(stderr,"MAG account: %s\n",text.UTF8String);
 if(quiet)return;
 [NSApplication sharedApplication];
 NSAlert *alert=[NSAlert new];alert.messageText=@"MAG account";alert.informativeText=text;[alert addButtonWithTitle:@"OK"];
 [NSApp activateIgnoringOtherApps:YES];[alert runModal];
}
static int run(NSString *binary,NSString *elf,NSString *cwd,NSDictionary *env,NSString *log) {
 NSFileManager *fm=NSFileManager.defaultManager;[fm createFileAtPath:log contents:nil attributes:@{NSFilePosixPermissions:@0600}];
 NSFileHandle *f=[NSFileHandle fileHandleForWritingAtPath:log];NSTask *t=[NSTask new];t.executableURL=[NSURL fileURLWithPath:binary];t.arguments=@[elf];t.currentDirectoryURL=[NSURL fileURLWithPath:cwd];t.environment=env;t.standardOutput=f;t.standardError=f;
 NSError *e=nil;if(![t launchAndReturnError:&e])fail(e.localizedDescription);[t waitUntilExit];[f closeFile];return t.terminationReason==NSTaskTerminationReasonExit?t.terminationStatus:128+t.terminationStatus;
}
// The player's settings file. A caller's absolute MAG_PLAYER_SETTINGS_PATH (automation, tests)
// replaces the one in the player's Library, as XDG_CONFIG_HOME and APPDATA do elsewhere.
static NSString *settingsFile(void) {
 const char *chosen=getenv("MAG_PLAYER_SETTINGS_PATH");
 if(chosen&&*chosen=='/')return [NSString stringWithUTF8String:chosen];
 return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MAG/settings.json"];
}
// This launch's performance and error report session, ended when main returns (src/player_reports.h).
struct ReportSession {
 mag::reports::Session session;std::filesystem::path self;bool active=false;
 ~ReportSession(){if(active)mag::reports::end_session(session,self);}
};
int main(int argc,char **argv) { @autoreleasepool {
 NSString *app=NSBundle.mainBundle.bundlePath.stringByResolvingSymlinksInPath;
 NSString *root=app.stringByDeletingLastPathComponent;
 NSString *ownedOverlay=nil;
 ReportSession report;
 // The detached report sender a session starts: MAG --send-reports <launch log folder>.
 if(argc==3&&!strcmp(argv[1],"--send-reports"))
  return mag::reports::run_sender(std::filesystem::path(root.fileSystemRepresentation)/"home/reports",settingsFile().fileSystemRepresentation,argv[2]);
 @try {
 NSDictionary *m=json([app stringByAppendingPathComponent:@"Contents/Resources/runtime.json"],2097152);
 NSString *(^inApp)(NSString *)=^NSString *(NSString *p){return [app stringByAppendingPathComponent:p].stringByStandardizingPath;};
 // The game files live in the MAG folder beside the app, named relative to it.
 id game=m[@"game"];if(![game isKindOfClass:NSString.class]||![game length]||[game isAbsolutePath])fail(@"Invalid local manifest");
 NSString *vfs=inApp(game);BOOL folder=NO;
 if(![NSFileManager.defaultManager fileExistsAtPath:[vfs stringByAppendingPathComponent:@"PS3_GAME"] isDirectory:&folder]||!folder)
  fail([@"The game files are missing. Keep the game folder beside MAG.app: " stringByAppendingString:vfs]);
 NSString *launcher=inApp(m[@"launcher"]),*engine=inApp(m[@"engine"]);
 NSString *launcherElf=[vfs stringByAppendingPathComponent:@"PS3_GAME/USRDIR/EBOOT.elf"];
 NSString *engineElf=[vfs stringByAppendingPathComponent:@"PS3_GAME/USRDIR/bin/mage_g.elf"];
 NSString *engineSelf=[vfs stringByAppendingPathComponent:@"PS3_GAME/USRDIR/bin/mage_g.self"];
 NSDictionary *lr=json(inApp(@"Contents/Resources/package-receipts/launcher.json"),2097152),*er=json(inApp(@"Contents/Resources/package-receipts/engine.json"),2097152);
 if(![lr[@"image_name"] isEqual:@"EBOOT"]||![er[@"image_name"] isEqual:@"mage_g"])fail(@"Incorrect image receipts");
 check(launcher,lr[@"binary_sha256"]);check(engine,er[@"binary_sha256"]);check(launcherElf,lr[@"elf_sha256"]);check(engineElf,er[@"elf_sha256"]);check(engineSelf,m[@"engine_self_sha256"]);
 NSString *token=[NSUUID.UUID.UUIDString.lowercaseString stringByReplacingOccurrencesOfString:@"-" withString:@""];
 NSString *logs=[root stringByAppendingPathComponent:[@"logs/launch-" stringByAppendingString:token]];
 if(![NSFileManager.defaultManager createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:nil])fail(@"Cannot create launch log directory");
 NSString *request=[logs stringByAppendingPathComponent:@"handoff-request.json"];
 NSMutableDictionary *env=[NSMutableDictionary dictionaryWithDictionary:@{@"PATH":@"/usr/bin:/bin",@"HOME":[root stringByAppendingPathComponent:@"home"],@"LANG":@"en_US.UTF-8",@"PS3_VFS_ROOT":vfs,@"PS3_BOOT_PATH":@"/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN",@"PS3_HLE_UNRESOLVED":@"halt",@"PS3_SYSCALL_UNRESOLVED":@"halt",@"MAG_HANDOFF_REQUEST":request,@"MAG_HANDOFF_TOKEN":token,@"MAG_ENGINE_BINARY":engine,@"MAG_ENGINE_BINARY_SHA256":er[@"binary_sha256"],@"MAG_ENGINE_ELF":engineElf,@"MAG_ENGINE_ELF_SHA256":er[@"elf_sha256"],@"MAG_ENGINE_SELF":engineSelf,@"MAG_ENGINE_SELF_SHA256":m[@"engine_self_sha256"],@"MAG_ENGINE_KEY_SHA256":keyHash(launcherElf)}];
 // Required runtime defaults recorded at packaging (movie playback for decoder builds).
 NSDictionary *defaults=m[@"environment"]?:@{};if(![defaults isKindOfClass:NSDictionary.class])fail(@"Invalid runtime defaults");
 for(NSString *k in defaults){if(![k isKindOfClass:NSString.class]||![k hasPrefix:@"PS3RECOMP_"]||![defaults[k] isKindOfClass:NSString.class])fail(@"Invalid runtime defaults");env[k]=defaults[k];}
 // Saves and trophies live in the installation, not in this launch's log folder (the runtime's
 // default is the working directory), so progress survives every launch.
 NSString *gamedata=[root stringByAppendingPathComponent:@"home/gamedata"];
 if(![NSFileManager.defaultManager createDirectoryAtPath:gamedata withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:nil])fail(@"Cannot create the save data folder");
 env[@"PS3_GAMEDATA_ROOT"]=gamedata;
 NSString *tmp=NSTemporaryDirectory();if(tmp)env[@"TMPDIR"]=tmp;
 char *overlayTemplate=strdup([[tmp?:@"/tmp" stringByAppendingPathComponent:@"mag-player-XXXXXX"] fileSystemRepresentation]);
 char *created=mkdtemp(overlayTemplate);
 if(!created){free(overlayTemplate);fail(@"Cannot create the private launch settings folder");}
 ownedOverlay=[NSString stringWithUTF8String:created];free(overlayTemplate);
 env[@"MAG_PLAYER_SETTINGS_PATH"]=settingsFile();
 // The session summary (src/mag_session_monitor.h) and the settings panel's Send report request go
 // to this launch's log folder; the report is decided when the session ends.
 env[@"MAG_SESSION_DIR"]=logs;env[@"MAG_REPORT_REQUEST"]=[logs stringByAppendingPathComponent:@"report-requested"];
 {mag::reports::Session &s=report.session;s.platform="macos";
  id version=m[@"version"];s.version=[version isKindOfClass:NSString.class]?[version UTF8String]:"unknown";
  s.logs=logs.fileSystemRepresentation;s.settings=settingsFile().fileSystemRepresentation;
  s.queue=std::filesystem::path(root.fileSystemRepresentation)/"home/reports";
  s.receipts=inApp(@"Contents/Resources/package-receipts").fileSystemRepresentation;s.backend="Metal";
  s.redaction.folders={{root.UTF8String,"<MAG folder>"}};s.redaction.homes={NSHomeDirectory().UTF8String};
  if(const char *h=getenv("HOME");h&&*h=='/')s.redaction.homes.push_back(h);
  report.self=NSBundle.mainBundle.executablePath.fileSystemRepresentation;report.active=true;
  mag::reports::start_session(s,report.self);}
 env[@"MAG_PLAYER_OVERLAY_ROOT"]=ownedOverlay;env[@"MAG_PLAYER_SETUP"]=@"1";
 env[@"MAG_PLAYER_LOGS_ROOT"]=[root stringByAppendingPathComponent:@"logs"];  // MAG Settings' Debug page: Open logs folder
 // Setup opens at every launch. --no-prompt, or MAG_SKIP_SETUP=1 from the caller, skips it (automation);
 // --settings (or Option, now redundant) still forces it.
 BOOL showSettings=(NSEvent.modifierFlags & NSEventModifierFlagOption)!=0,skipSetup=NO;
 for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--settings"))showSettings=YES;else if(!strcmp(argv[i],"--no-prompt"))skipSetup=YES;}
 const char *callerSkip=getenv("MAG_SKIP_SETUP");if(callerSkip&&*callerSkip&&strcmp(callerSkip,"0"))skipSetup=YES;
 if(showSettings)env[@"MAG_PLAYER_SHOW_SETTINGS"]=@"1";else if(skipSetup)env[@"MAG_SKIP_SETUP"]=@"1";
 int result=run(launcher,launcherElf,logs,env,[logs stringByAppendingPathComponent:@"launcher.log"]);
 report.session.launcher_exit=result;
 if(result!=90){signInNotice(ownedOverlay,logs,skipSetup);return result;}
 NSDictionary *expected=@{@"version":@1,@"token":token,@"guest_path":@"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self",@"kind":@"mage_g"};
 if(![json(request,4096) isEqual:expected])fail(@"Engine handoff request identity mismatch");
 check(engine,er[@"binary_sha256"]);check(engineElf,er[@"elf_sha256"]);check(engineSelf,m[@"engine_self_sha256"]);
 for(NSString *k in env.allKeys.copy)if([k hasPrefix:@"MAG_HANDOFF_"]||[k hasPrefix:@"MAG_ENGINE_"])[env removeObjectForKey:k];
 [env removeObjectForKey:@"MAG_PLAYER_SETUP"];[env removeObjectForKey:@"MAG_PLAYER_SHOW_SETTINGS"];[env removeObjectForKey:@"MAG_SKIP_SETUP"];
 env[@"PS3_BOOT_PATH"]=@"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self";
 report.session.engine_exit=run(engine,engineElf,logs,env,[logs stringByAppendingPathComponent:@"engine.log"]);
 signInNotice(ownedOverlay,logs,skipSetup);
 return (int)report.session.engine_exit;
 } @catch(NSException *e) {
 if(e.reason)report.session.launch_error=e.reason.UTF8String;
 NSString *message=[NSString stringWithFormat:@"MAG could not start: %@\n",e.reason];
 [message writeToFile:[root stringByAppendingPathComponent:@"launch-error.log"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
 fprintf(stderr,"%s",message.UTF8String);return 1;
 } @finally {
 if(ownedOverlay)[NSFileManager.defaultManager removeItemAtPath:ownedOverlay error:nil];
 }
} }
'''


# Sources the supervisor compiles with it: the reports (src/player_reports.h), their gzip, and the
# portal transport that sends them with the linked account's token.
SUPERVISOR_SOURCES = ('player_reports.h', 'player_reports.cpp', 'player_gzip.h', 'player_gzip.cpp', 'player_portal.h',
                      'player_report_request.h', 'player_report_request.cpp', 'player_settings_text.h',
                      'player_portal.cpp', 'player_portal_macos.h', 'player_portal_macos.mm', 'player_settings_macos.h',
                      'player_settings_store.h', 'player_sign_in_refusal.h')


# The runtime's movie playback switches, as tools/boot.py applies them (MOVIE_PLAYBACK_ENV there).
# A build with the movie decoder needs them: without them it opens and closes each movie at once,
# and the game waits forever after the legal screen. A build without the decoder must not have them.
MOVIE_PLAYBACK_ENV = {'PS3RECOMP_SAIL_TIMED_THX': '1', 'PS3RECOMP_SAIL_ATRAC_AUDIO': '1',
                      'PS3RECOMP_GCM_CHECKED_REF': '1'}


def playback_environment(records):
    """The playback switches when either image's source receipt records the movie decoder."""
    decoder = [records[role].get('movie_playback') is True for role in ('launcher', 'engine')]
    return dict(MOVIE_PLAYBACK_ENV) if any(decoder) else {}


def product_version(workspace):
    """The release version the workspace's source was cut from (release-please's manifest), for
    the reports the supervisor sends; "unknown" when the source carries none."""
    try:
        version = json.loads(contained(workspace, '.release-please-manifest.json').read_text(encoding='utf-8')).get('.')
    except (OSError, ValueError, InstallerError, AttributeError):
        return 'unknown'
    if isinstance(version, str) and re.fullmatch(r'[0-9A-Za-z.+-]{1,64}', version):
        return version
    return 'unknown'


# The game files' folder inside the MAG folder, and where the build workspace keeps them.
GAME_FOLDER = 'game'
WORKSPACE_GAME = 'local/retail-v1.0'


def check_workspace(workspace, folder):
    """The build workspace must be an ordinary folder outside the new MAG folder."""
    workspace, folder = Path(workspace).absolute(), Path(folder).absolute()
    if workspace.is_symlink() or not workspace.is_dir():
        raise InstallerError('The build workspace must be an ordinary folder')
    if not folder.is_dir() or folder.is_symlink():
        raise InstallerError('Choose a new MAG folder under an existing ordinary folder')
    workspace_real, folder_real = workspace.resolve(), folder.resolve()
    if workspace_real.is_relative_to(folder_real) or folder_real.is_relative_to(workspace_real):
        raise InstallerError('The build workspace must be outside the MAG folder')
    game = folder / GAME_FOLDER
    if game.exists() or game.is_symlink():
        raise InstallerError(f'The MAG folder already holds game files: {game}')
    return workspace, folder


def stage_game(workspace, folder, token, events):
    """Place the game files in <MAG folder>/game: hard links to the workspace's copies where the
    volume allows, else copies. The firmware and everything else the build used stay behind."""
    from .native_build import link_tree
    counts = link_tree(contained(workspace, WORKSPACE_GAME), folder / GAME_FOLDER, token)
    events({'event': 'game-files-staged', **counts,
            'message': f'Game files in the MAG folder: {counts["linked"]} linked, {counts["copied"]} copied'})
    return folder / GAME_FOLDER


def package_receipt(record, binary, source_receipt, token):
    """Bind the packaged bytes to the build's receipt by digest. The build's receipt itself stays
    in the build folder: it records build-machine paths, which the MAG folder must not carry."""
    result = {key: record[key] for key in ('image_name', 'elf_sha256', 'sdk_revision')}
    result.update(binary_sha256=_digest(binary, token), kind='local-packaged-native',
                  source_receipt_sha256=_digest(source_receipt, token),
                  original_binary_sha256=record['binary_sha256'])
    return result


def package(config, workspace, destination, token=None, events=None):
    """Build <MAG folder>/MAG.app and <MAG folder>/game without launching any executable.

    The workspace is the finished build's; it may be anywhere outside the MAG folder, and the
    package never refers to it. No cache/source-toolchain paths enter the launcher. Existing
    destinations are never overwritten.
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
    if destination.suffix != '.app' or not destination.parent.is_dir(): raise InstallerError('Choose a new .app in the new MAG folder')
    workspace, folder = check_workspace(workspace, destination.parent)
    manifest = json.loads(contained(workspace, 'profiles/retail-v1.0.json').read_text(encoding='utf-8'))
    framework = json.loads(contained(workspace, 'profiles/framework.json').read_text(encoding='utf-8'))
    records, originals = {}, {}
    for role, folder_name, image in [('launcher', 'build', 'EBOOT'), ('engine', 'build-engine', 'mage_g')]:
        binary = _ordinary(contained(workspace, folder_name + '/MagRecomp'))
        receipt = _ordinary(binary.with_suffix('.receipt.json'))
        record = json.loads(receipt.read_text()); expected = manifest['images'][image]
        if (record.get('binary_sha256') != _digest(binary, token) or record.get('elf_sha256') != expected['elf_sha256'] or record.get('image_name') != image):
            raise InstallerError(f'Native source receipt mismatch: {receipt}')
        if record.get('sdk_revision') not in set(framework['strict_runtime_revisions']) & set(framework['strict_hle_runtime_revisions']):
            raise InstallerError('Packaged runtime must support strict syscall/HLE policies')
        relative = WORKSPACE_GAME + '/PS3_GAME/USRDIR/' + ('EBOOT.elf' if role == 'launcher' else 'bin/mage_g.elf')
        elf = _ordinary(contained(workspace, relative))
        if _digest(elf, token) != record['elf_sha256']:
            raise InstallerError(f'Native runtime ELF identity mismatch: {elf}')
        records[role], originals[role] = record, (binary, receipt)
    destination.mkdir(mode=0o700)
    try:
        game = stage_game(workspace, folder, token, events)
        macos = destination/'Contents/MacOS'; macos.mkdir(parents=True)
        resources = destination/'Contents/Resources'; resources.mkdir()
        libs = destination/'Contents/Frameworks'; libs.mkdir()
        package_receipts = resources/'package-receipts'; package_receipts.mkdir()
        copied, edges, library_names = {}, {}, {}
        for role, name in [('launcher', 'MagRecomp'), ('engine', 'MagEngine')]:
            source, receipt = originals[role]; target = macos/name
            shutil.copyfile(source, target); target.chmod(0o755)
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
                # The build machine's library path is not recorded, only its file name.
                adjustments.append({'file':str(target.relative_to(destination)),
                                    'from':Path(original).name if original.startswith('/') else original, 'to':replacement})
            # Every library now loads from the app, so search paths into the build machine
            # (its managed tools) are dropped; relative ones stay.
            for rpath in _rpaths(target, token):
                if not rpath.startswith('@'):
                    _run_tool(['/usr/bin/install_name_tool', '-delete_rpath', rpath, str(target)], token)
            _run_tool(['/usr/bin/codesign', '--force', '--sign', '-', str(target)], token)
        runtime = {'schema_version':2,'game':os.path.relpath(game,destination),'version':product_version(workspace),
                   'launcher':'Contents/MacOS/MagRecomp','engine':'Contents/MacOS/MagEngine',
                   'engine_self_sha256':manifest['images']['mage_g']['source_sha256'],
                   'signing':'local-ad-hoc','adjustments':adjustments,
                   'environment':playback_environment(records)}
        for role, name in [('launcher','MagRecomp'),('engine','MagEngine')]:
            # A distinct package receipt binds transformed bytes and the build's
            # immutable source receipt; it does not rewrite the source build.
            record = package_receipt(records[role], macos/name, originals[role][1], token)
            (package_receipts/(role+'.json')).write_text(json.dumps(record,indent=2)+'\n')
        (resources/'runtime.json').write_text(json.dumps(runtime,indent=2)+'\n')
        # The supervisor and the report, portal and gzip sources it builds with, from the
        # workspace's own source (removed again: the app carries only the binary).
        scratch=resources/'supervisor-build'; scratch.mkdir()
        source=scratch/'launcher.mm'; source.write_text(_LAUNCHER)
        for name in SUPERVISOR_SOURCES:
            shutil.copyfile(_ordinary(contained(workspace,'src/'+name)),scratch/name)
        supervisor=macos/'MAG'
        _run_tool(['/usr/bin/xcrun','clang++','-std=c++17','-O2','-fobjc-arc','-I'+str(scratch),
                   '-x','objective-c++',str(source),*[str(scratch/n) for n in SUPERVISOR_SOURCES if n.endswith('.mm')],
                   '-x','c++',*[str(scratch/n) for n in SUPERVISOR_SOURCES if n.endswith('.cpp')],
                   '-framework','Foundation','-framework','AppKit','-o',str(supervisor)],token)
        _ordinary(supervisor).chmod(0o755); shutil.rmtree(scratch)
        with (destination/'Contents/Info.plist').open('wb') as stream:
            plistlib.dump({'CFBundleExecutable':'MAG','CFBundleName':'MAG','CFBundleDisplayName':'MAG',
                           'CFBundleIdentifier':'local.mag.native','CFBundlePackageType':'APPL',
                           'CFBundleShortVersionString':'0.1','LSMinimumSystemVersion':'12.0',
                           'NSHighResolutionCapable':True},stream)
        _run_tool(['/usr/bin/codesign','--force','--sign','-',str(destination)],token)
        events({'event':'app-packaged','output':str(destination),'launcher':str(supervisor),
                'message':'MAG.app packaged. Its MAG folder is self-contained: move it as a whole.'})
        return destination
    except BaseException:
        shutil.rmtree(destination)
        shutil.rmtree(folder / GAME_FOLDER, ignore_errors=True)  # check_workspace: it was not there
        raise
