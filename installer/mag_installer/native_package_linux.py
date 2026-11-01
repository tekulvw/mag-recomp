"""Private Linux native package; no Python interpreter is needed at launch.

Non-platform ELF dependencies are copied into lib/. The Linux ELF loader, glibc,
and graphics driver dispatch remain system-provided: build on the oldest supported
Linux baseline and move the complete installation, not just MAG/. ldd failures
report missing dependencies before packaging. This never launches game binaries.
"""
from __future__ import annotations
import json
import os
from pathlib import Path
import re
import shutil

from .model import InstallerError
from .safety import Cancellation, contained

_SUPERVISOR = r'''
#include <openssl/evp.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>
#include "supervisor_config.h"
namespace fs=std::filesystem;
static fs::path root;
struct OwnedOverlay { fs::path path; ~OwnedOverlay(){if(!path.empty()){std::error_code error;fs::remove_all(path,error);}} };
static volatile sig_atomic_t child=0;
static void interrupted(int sig) { if(child>0) kill(child,sig); }
static void fail(const std::string& message) { throw std::runtime_error(message); }
static std::string read_file(const fs::path& p,size_t limit=0) {
 int fd=open(p.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);struct stat st{};
 if(fd<0)fail("Cannot open local file: "+p.string());
 if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||(limit && (uint64_t)st.st_size>limit)){close(fd);fail("Invalid local file: "+p.string());}
 std::string data;char b[65536];ssize_t n;
 while((n=read(fd,b,sizeof b))!=0){if(n<0){if(errno==EINTR)continue;close(fd);fail("Cannot read local file");}data.append(b,n);if(limit&&data.size()>limit){close(fd);fail("Local file exceeds limit");}}
 close(fd);return data;
}
static std::string hex(const unsigned char* bytes,size_t size) { std::string s;for(size_t i=0;i<size;i++){s+="0123456789abcdef"[bytes[i]>>4];s+="0123456789abcdef"[bytes[i]&15];}return s; }
static std::string sha(const std::string& data) { unsigned char d[32];unsigned n=0;if(!EVP_Digest(data.data(),data.size(),d,&n,EVP_sha256(),nullptr)||n!=32)fail("SHA256 unavailable");return hex(d,n); }
static void check(const fs::path& p,const char* expected) { if(sha(read_file(p))!=expected)fail("Local input identity changed: "+p.string()); }
static uint64_t be(const char* p,int n) { uint64_t x=0;while(n--)x=(x<<8)|(unsigned char)*p++;return x; }
static std::string key_hash(const fs::path& path) {
 auto d=read_file(path);const char* b=d.data();if(d.size()<64||memcmp(b,"\177ELF\2\2\1",7))fail("Invalid launcher ELF");
 auto off=be(b+32,8),size=be(b+54,2),count=be(b+56,2);
 if(size!=56||off>d.size()||count>(d.size()-off)/size)fail("Invalid ELF headers");
 for(uint64_t i=0;i<count;i++){const char* s=b+off+i*size;auto file=be(s+8,8),va=be(s+16,8),len=be(s+32,8);
 if(be(s,4)==1&&va<=0x32A660&&len>=16&&0x32A670-va<=len){auto delta=0x32A660-va;if(file>d.size()||delta>d.size()-file||d.size()-file-delta<16)fail("Invalid key segment");return sha(d.substr(file+delta,16));}}
 fail("Launcher key segment unavailable");return {};
}
static void private_directory(const fs::path& p) {
 struct stat st{};
 if(lstat(p.c_str(),&st)==0){if(!S_ISDIR(st.st_mode))fail("Private directory is not an ordinary directory: "+p.string());return;}
 if(errno!=ENOENT||mkdir(p.c_str(),0700))fail("Cannot create private directory: "+p.string());
}
static int run(const fs::path& binary,const fs::path& elf,const fs::path& cwd,std::map<std::string,std::string> env,const char* log) {
 int fd=open((cwd/log).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)fail("Cannot create launch log");
 std::vector<std::string> values;for(auto& e:env)values.push_back(e.first+"="+e.second);
 std::vector<char*> block;for(auto& v:values)block.push_back(v.data());block.push_back(nullptr);
 pid_t parent=getpid();pid_t pid=fork();if(pid<0){close(fd);fail("Cannot fork game");}
 if(!pid){prctl(PR_SET_PDEATHSIG,SIGTERM);if(getppid()!=parent)_exit(1);if(chdir(cwd.c_str())||dup2(fd,1)<0||dup2(fd,2)<0)_exit(1);close(fd);char* args[]={const_cast<char*>(binary.c_str()),const_cast<char*>(elf.c_str()),nullptr};execve(binary.c_str(),args,block.data());dprintf(2,"Cannot start MAG: %s. Check Linux desktop/runtime dependencies.\n",strerror(errno));_exit(127);}
 close(fd);child=pid;int status;while(waitpid(pid,&status,0)<0){if(errno!=EINTR)fail("Cannot wait for game");}child=0;
 return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
int main(int argc,char **argv) {
 umask(0077);
 OwnedOverlay overlay;
 try {
 char self[65536];auto n=readlink("/proc/self/exe",self,sizeof self-1);if(n<=0||n==(ssize_t)sizeof self-1)fail("Cannot locate MAG executable");self[n]=0;
 fs::path app=fs::path(self).parent_path();root=app.parent_path();auto workspace=(app/MAG_WORKSPACE).lexically_normal();auto vfs=workspace/"local/retail-v1.0";
 auto launcher=app/"bin/MagRecomp",engine=app/"bin/MagEngine",le=vfs/"PS3_GAME/USRDIR/EBOOT.elf",ee=vfs/"PS3_GAME/USRDIR/bin/mage_g.elf",es=vfs/"PS3_GAME/USRDIR/bin/mage_g.self";
 check(launcher,MAG_LAUNCHER_SHA256);check(engine,MAG_ENGINE_SHA256);check(le,MAG_LAUNCHER_ELF_SHA256);check(ee,MAG_ENGINE_ELF_SHA256);check(es,MAG_ENGINE_SELF_SHA256);auto key=key_hash(le);
 unsigned char random[16];int rng=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(rng<0)fail("No system random source");size_t got=0;while(got<16){auto c=read(rng,random+got,16-got);if(c<0&&errno==EINTR)continue;if(c<=0)fail("No system randomness");got+=c;}close(rng);auto token=hex(random,16);
 auto logs=root/"logs"/("launch-"+token);private_directory(logs.parent_path());if(!fs::create_directory(logs))fail("Cannot create private launch directory");auto request=logs/"handoff-request.json";private_directory(root/"home");
 std::map<std::string,std::string> env={{"PATH","/usr/bin:/bin"},{"HOME",(root/"home").string()},{"LANG","C.UTF-8"},{"LD_LIBRARY_PATH",(app/"lib").string()},{"PS3_VFS_ROOT",vfs.string()},{"PS3_BOOT_PATH","/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN"},{"PS3RECOMP_VK_WINDOW","1"},{"PS3RECOMP_VK_GUEST_PROGRAMS","1"},{"PS3_HLE_UNRESOLVED","halt"},{"PS3_SYSCALL_UNRESOLVED","halt"},{"MAG_HANDOFF_REQUEST",request.string()},{"MAG_HANDOFF_TOKEN",token},{"MAG_ENGINE_BINARY",engine.string()},{"MAG_ENGINE_ELF",ee.string()},{"MAG_ENGINE_SELF",es.string()},{"MAG_ENGINE_BINARY_SHA256",MAG_ENGINE_SHA256},{"MAG_ENGINE_ELF_SHA256",MAG_ENGINE_ELF_SHA256},{"MAG_ENGINE_SELF_SHA256",MAG_ENGINE_SELF_SHA256},{"MAG_ENGINE_KEY_SHA256",key}};
 // Preserve only desktop transport/session variables, never inherited game diagnostics.
 for(auto name:{"DISPLAY","WAYLAND_DISPLAY","XDG_RUNTIME_DIR","DBUS_SESSION_BUS_ADDRESS","XAUTHORITY","PULSE_SERVER","PIPEWIRE_REMOTE"})if(auto value=getenv(name))env[name]=value;
 auto settingsHome=getenv("XDG_CONFIG_HOME");auto realHome=getenv("HOME");
 if((!settingsHome||!*settingsHome)&&(!realHome||!*realHome))fail("The user settings folder is unavailable");
 fs::path configHome=settingsHome&&*settingsHome?fs::path(settingsHome):fs::path(realHome)/".config";
 if(!configHome.is_absolute())fail("The user settings folder must be absolute");
 std::string pattern=(logs/"player-overlay-XXXXXX").string();std::vector<char> temporary(pattern.begin(),pattern.end());temporary.push_back(0);
 auto created=mkdtemp(temporary.data());if(!created)fail("Cannot create the private launch settings folder");overlay.path=created;
 env["MAG_PLAYER_SETTINGS_PATH"]=(configHome/"MAG/settings.json").string();env["MAG_PLAYER_OVERLAY_ROOT"]=overlay.path.string();env["MAG_PLAYER_SETUP"]="1";
 if(argc>1&&!strcmp(argv[1],"--settings"))env["MAG_PLAYER_SHOW_SETTINGS"]="1";
 signal(SIGINT,interrupted);signal(SIGTERM,interrupted);signal(SIGHUP,interrupted);
 int result=run(launcher,le,logs,env,"launcher.log");if(result!=90)return result;
 auto expected="{\"version\":1,\"token\":\""+token+"\",\"guest_path\":\"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self\",\"kind\":\"mage_g\"}\n";
 if(read_file(request,4096)!=expected)fail("Engine handoff request identity mismatch");
 check(engine,MAG_ENGINE_SHA256);check(ee,MAG_ENGINE_ELF_SHA256);check(es,MAG_ENGINE_SELF_SHA256);
 for(auto i=env.begin();i!=env.end();)if(i->first.find("MAG_HANDOFF_")==0||i->first.find("MAG_ENGINE_")==0)i=env.erase(i);else ++i;
 env.erase("MAG_PLAYER_SETUP");env.erase("MAG_PLAYER_SHOW_SETTINGS");
 env["PS3_BOOT_PATH"]="/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self";return run(engine,ee,logs,env,"engine.log");
 }catch(const std::exception& e){std::string msg="MAG could not start: "+std::string(e.what())+"\n";std::cerr<<msg;if(!root.empty()){int fd=open((root/"launch-error.log").c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd>=0){write(fd,msg.data(),msg.size());close(fd);}}return 1;}
}
'''


def _copy_dependencies(binaries, destination, token, dynamic_libraries=()):
    """Copy resolved non-platform ELF dependencies; preserve system ABI/drivers."""
    from .native_package import _run_tool
    # glibc/loader and graphics driver dispatch must match the user's desktop.
    system = re.compile(r'^(?:linux-vdso|ld-linux|lib(?:c|m|pthread|dl|rt|resolv|util|nss_[^.]*)\.so|lib(?:GL|EGL|GLX|OpenGL|vulkan|drm|gbm)[^/]*\.so)')
    copied = {}
    dynamic_libraries = [Path(path).resolve(strict=True) for path in dynamic_libraries]
    for source in dynamic_libraries:
        dynamic = _run_tool(['readelf', '-d', str(source)], token)
        match = re.search(r'\(SONAME\).*?\[([^\]]+)\]', dynamic)
        if not match or not re.fullmatch(r'[A-Za-z0-9_+.-]+', match[1]):
            raise InstallerError('Linux dynamic runtime library needs a plain SONAME: ' + str(source))
        if match[1] in copied and copied[match[1]] != source:
            raise InstallerError('Linux runtime library basename collision: ' + match[1])
        copied[match[1]] = source
    search = os.pathsep.join(dict.fromkeys(str(path.parent) for path in dynamic_libraries))
    for binary in [*binaries, *dynamic_libraries]:
        argv = ['ldd', str(binary)]
        if search:
            argv = ['/usr/bin/env', 'LD_LIBRARY_PATH=' + search, *argv]
        output = _run_tool(argv, token)
        if 'not found' in output:
            raise InstallerError('Missing Linux desktop/runtime dependencies:\n' + output[-3000:])
        for line in output.splitlines():
            match = re.match(r'\s*(\S+) => (.+?) \(', line)
            if not match or system.match(match[1]):
                continue
            name, source = match.groups()
            if not Path(source).is_absolute():
                raise InstallerError('Linux dependency did not resolve to an absolute path: ' + source)
            source = Path(source).resolve()
            if name in copied and copied[name] != source:
                raise InstallerError('Linux runtime library basename collision: ' + name)
            copied[name] = source
    for name, source in copied.items():
        shutil.copyfile(source, destination / name)
        (destination / name).chmod(0o755)
    return sorted(copied)


def package(config, workspace, destination, token=None, events=None):
    """Build <install>/MAG beside its owned workspace without launching anything."""
    from .native_package import _ordinary, _digest
    token = token or Cancellation(); events = events or (lambda event: None)
    workspace, destination = Path(workspace).absolute(), Path(destination).absolute()
    if destination.exists() or destination.is_symlink(): raise InstallerError('MAG folder destination already exists')
    if destination.name != 'MAG' or not destination.parent.is_dir(): raise InstallerError('Choose a new MAG folder under the installation directory')
    root = destination.parent.resolve()
    workspace_real = workspace.resolve()
    if workspace.is_symlink() or not workspace.is_dir() or workspace_real == root or not workspace_real.is_relative_to(root):
        raise InstallerError('Owned workspace must be an ordinary descendant of the installation folder')
    contained(destination.parent, str(workspace.relative_to(destination.parent)))
    if workspace_real.is_relative_to(destination.resolve()) or destination.resolve().is_relative_to(workspace_real):
        raise InstallerError('App and workspace must be separate installation paths')
    manifest = json.loads(contained(workspace, 'profiles/retail-v1.0.json').read_text(encoding='utf-8'))
    framework = json.loads(contained(workspace, 'profiles/framework.json').read_text(encoding='utf-8'))
    records, originals = {}, {}
    for role, folder, image in [('launcher', 'build', 'EBOOT'), ('engine', 'build-engine', 'mage_g')]:
        binary = _ordinary(contained(workspace, folder + '/MagRecomp'))
        receipt = _ordinary(binary.with_suffix('.receipt.json'))
        record = json.loads(receipt.read_text(encoding='utf-8')); expected = manifest['images'][image]
        if (record.get('binary_sha256') != _digest(binary, token) or record.get('elf_sha256') != expected['elf_sha256'] or record.get('image_name') != image):
            raise InstallerError(f'Native source receipt mismatch: {receipt}')
        if record.get('sdk_revision') not in set(framework['strict_runtime_revisions']) & set(framework['strict_hle_runtime_revisions']):
            raise InstallerError('Packaged runtime must support strict syscall/HLE policies')
        relative = 'local/retail-v1.0/PS3_GAME/USRDIR/' + ('EBOOT.elf' if role == 'launcher' else 'bin/mage_g.elf')
        if _digest(_ordinary(contained(workspace, relative)), token) != record['elf_sha256']:
            raise InstallerError(f'Native runtime ELF identity mismatch: {relative}')
        records[role], originals[role] = record, (binary, receipt)
    destination.mkdir(mode=0o700)
    try:
        bin_dir = destination / 'bin'; bin_dir.mkdir()
        resources = destination / 'resources'; resources.mkdir()
        receipts = resources / 'source-receipts'; receipts.mkdir()
        package_receipts = resources / 'package-receipts'; package_receipts.mkdir()
        for role, name in [('launcher', 'MagRecomp'), ('engine', 'MagEngine')]:
            source, receipt = originals[role]
            shutil.copyfile(source, bin_dir / name)
            (bin_dir / name).chmod(0o755)
            shutil.copyfile(receipt, receipts / (role + '.json'))
            record = {key: records[role][key] for key in ('image_name', 'elf_sha256', 'sdk_revision')}
            record.update(binary_sha256=_digest(bin_dir / name, token), kind='local-packaged-native',
                          source_receipt_sha256=_digest(receipts / (role + '.json'), token),
                          original_binary_sha256=records[role]['binary_sha256'])
            (package_receipts / (role + '.json')).write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        workspace_relative = os.path.relpath(workspace, destination)
        engine_self = manifest['images']['mage_g']['source_sha256']
        runtime = {'schema_version': 1, 'workspace': Path(workspace_relative).as_posix(),
                   'launcher': 'bin/MagRecomp', 'engine': 'bin/MagEngine',
                   'engine_self_sha256': engine_self, 'signing': 'unsigned-local'}
        (resources / 'runtime.json').write_text(json.dumps(runtime, indent=2) + '\n', encoding='utf-8')
        scratch = destination / '.supervisor-build'; scratch.mkdir()
        definitions = {
            'MAG_WORKSPACE': workspace_relative,
            'MAG_LAUNCHER_SHA256': _digest(bin_dir / 'MagRecomp', token),
            'MAG_ENGINE_SHA256': _digest(bin_dir / 'MagEngine', token),
            'MAG_LAUNCHER_ELF_SHA256': records['launcher']['elf_sha256'],
            'MAG_ENGINE_ELF_SHA256': records['engine']['elf_sha256'],
            'MAG_ENGINE_SELF_SHA256': engine_self,
        }
        (scratch / 'supervisor_config.h').write_text(''.join(
            '#define ' + key + ' ' + json.dumps(value, ensure_ascii=False) + '\n'
            for key, value in definitions.items()), encoding='utf-8')
        source = scratch / 'supervisor.cpp'
        source.write_text(_SUPERVISOR, encoding='utf-8')
        from .native_package import _run_tool
        compiler = config.get('linux_toolchain', {}).get('cxx', 'c++')
        _run_tool([compiler, '-std=c++17', '-O2', str(source), '-o', str(destination / 'MAG'),
                   '-lcrypto', '-Wl,-rpath,$ORIGIN/lib'], token)
        _ordinary(destination / 'MAG').chmod(0o755)
        libraries = destination / 'lib'; libraries.mkdir()
        runtime['libraries'] = _copy_dependencies(
            [destination / 'MAG', bin_dir / 'MagRecomp', bin_dir / 'MagEngine'], libraries, token,
            [config['ffmpeg'][name] for name in ('avcodec', 'avformat', 'avutil', 'swscale')]
            if 'ffmpeg' in config else [])
        (resources / 'runtime.json').write_text(json.dumps(runtime, indent=2) + '\n', encoding='utf-8')
        shutil.rmtree(scratch)
        events({'event': 'app-packaged', 'output': str(destination), 'launcher': str(destination / 'MAG'),
                'message': 'MAG was packaged. Start it with MAG; keep the whole installation folder together.'})
        return destination
    except BaseException:
        shutil.rmtree(destination, ignore_errors=True)
        raise
