"""Private Linux native package; no Python interpreter is needed at launch.

Layout: <MAG folder>/MAG/{MAG,bin,lib,resources} and the game files in <MAG folder>/game.
Non-platform ELF dependencies are copied into lib/. The Linux ELF loader, glibc,
and graphics driver dispatch remain system-provided: build on the oldest supported
Linux baseline. Nothing refers to the build workspace, so the MAG folder can be moved
as a whole and the build folder deleted. ldd failures report missing dependencies
before packaging. This never launches game binaries.
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
#include <chrono>
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
#include <thread>
#include <unistd.h>
#include "player_portal.h"
#include "player_settings_portable.h"
#include "supervisor_config.h"
#ifndef MAG_RUNTIME_DEFAULTS  // a config written before packaged defaults existed: none
#define MAG_RUNTIME_DEFAULTS {}
#endif
namespace fs=std::filesystem;
static fs::path root;
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
// The settings window (MagEngine --mag-player-settings-setup, src/player_settings_session.h): the
// same window F10 opens in game, with Play MAG, before the game starts. 0: Play MAG; 3: cancelled;
// anything else: it could not open, and the launch goes on with the saved settings.
static int run_settings(const fs::path& engine,const fs::path& logs,const std::map<std::string,std::string>& env,const char* notice) {
 int fd=open((logs/"settings.log").c_str(),O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return 1;
 std::vector<std::string> values;for(auto& e:env)values.push_back(e.first+"="+e.second);
 std::vector<char*> block;for(auto& v:values)block.push_back(v.data());block.push_back(nullptr);
 pid_t parent=getpid();pid_t pid=fork();if(pid<0){close(fd);return 1;}
 if(!pid){prctl(PR_SET_PDEATHSIG,SIGTERM);if(getppid()!=parent)_exit(1);if(chdir(logs.c_str())||dup2(fd,1)<0||dup2(fd,2)<0)_exit(1);close(fd);
  char setup[]="--mag-player-settings-setup";char* args[]={const_cast<char*>(engine.c_str()),setup,const_cast<char*>(notice),nullptr};
  execve(engine.c_str(),args,block.data());_exit(127);}
 close(fd);child=pid;int status;while(waitpid(pid,&status,0)<0){if(errno!=EINTR)return 1;}child=0;
 return WIFEXITED(status)?WEXITSTATUS(status):1;
}
static const char* usage="Usage: MAG [--server <hostname or IPv4>] [--name <player name>] [--no-prompt] | MAG --link | MAG --unlink"
 " | MAG [--server <hostname or IPv4>] [--portal-url <url>] --link-token < token";
// The settings as this launch will apply them, for choosing the account portal. A damaged file
// yields none: the game reports it and plays offline.
static mag::PortableSettings player_settings(const fs::path& path,const std::map<std::string,std::string>& choices,bool& usable) {
 mag::PortableSettings settings;std::string error;std::error_code none;usable=true;
 if(fs::exists(fs::symlink_status(path,none))){try{usable=mag::parse_portable_settings(read_file(path,65536),settings,error);}catch(const std::exception&){usable=false;}}
 if(!usable)return mag::PortableSettings{};
 if(auto c=choices.find("MAG_PLAYER_SET_SERVER");c!=choices.end()){std::string host=c->second;if(mag::check_server_address(host,error))settings.server_address=host;}
 if(auto c=choices.find("MAG_PLAYER_SET_NAME");c!=choices.end()&&mag::check_player_name(c->second,error))settings.player_name=c->second;
 return settings;
}
// Open the link page in the player's browser, never waiting for it (best effort; the URL is also printed).
static void open_browser(const std::string& url) {
 if(!getenv("DISPLAY")&&!getenv("WAYLAND_DISPLAY"))return;
 pid_t pid=fork();if(pid<0)return;
 if(!pid){if(fork()==0){setsid();int null=open("/dev/null",O_RDWR);if(null>=0){dup2(null,0);dup2(null,1);dup2(null,2);}
  execlp("xdg-open","xdg-open",url.c_str(),(char*)nullptr);_exit(127);}_exit(0);}
 while(waitpid(pid,nullptr,0)<0&&errno==EINTR){}
}
static const char* relink="Run ./MAG --link to link it again.";
// MAG --link: an account link through the portal's device-code page; the token is stored owner-only.
static int link_account(const fs::path& link_file,const std::string& portal,const fs::path& settings) {
 if(portal.empty())fail("No account portal applies to this server. Linking works with play.magtest.online, or with portal_url in "+settings.string()+".");
 auto transport=mag::portal::make_platform_transport();mag::portal::Link link;
 auto outcome=mag::portal::run_link(*transport,portal,"MAG launcher (Linux)",{
  [](const mag::portal::LinkStart& start){
   std::cout<<"To link MAG to your account, open "<<start.verification_uri<<" and enter this code:\n\n    "<<start.user_code<<"\n\n";
   if(!start.verification_uri_complete.empty()){std::cout<<"Or open "<<start.verification_uri_complete<<"\n";open_browser(start.verification_uri_complete);}
   std::cout<<"Waiting for you to approve the link (Ctrl+C cancels)..."<<std::endl;},
  [](int seconds){std::this_thread::sleep_for(std::chrono::seconds(seconds));return true;}},link);
 if(!outcome.ok())fail(outcome.message);
 std::string error;if(!mag::portal::save_link(link_file,link,error))fail(error);
 std::cout<<"Linked"<<(link.player_name.empty()?std::string():" as "+link.player_name)<<". Start ./MAG to play.\n";
 if(link.player_name.empty())std::cout<<"Your account has no player name yet: claim one at "<<portal<<"/account first.\n";
 return 0;
}
// MAG --link-token: a device token minted on the portal (a service account for test automation),
// read from standard input, never the command line, which other users' processes can read. The
// --server and --portal-url choices are saved to the settings file, so later launches use the same
// server and portal; the token is then checked by fetching one ticket.
static int link_token(const fs::path& link_file,const fs::path& path,const std::map<std::string,std::string>& choices) {
 mag::PortableSettings settings;std::string error;std::error_code none;
 const bool exists=fs::exists(fs::symlink_status(path,none));
 if(exists){bool ok=false;try{ok=mag::parse_portable_settings(read_file(path,65536),settings,error);}catch(const std::exception& e){error=e.what();}
  if(!ok)fail("Fix or delete "+path.string()+" first: "+error);}
 bool changed=!exists;
 if(auto c=choices.find("MAG_PLAYER_SET_SERVER");c!=choices.end()){std::string host=c->second;
  if(!mag::check_server_address(host,error))fail("--server "+c->second+": "+error);changed|=host!=settings.server_address;settings.server_address=host;}
 if(auto c=choices.find("portal_url");c!=choices.end()){std::string url=c->second;
  if(!mag::portal::check_portal_url(url,error))fail("--portal-url "+c->second+": "+error);changed|=url!=settings.portal_url;settings.portal_url=url;}
 if(settings.server_address.empty())fail("Choose the game server first: ./MAG --server <host> --link-token.");
 const std::string portal=mag::portal::portal_for(settings.server_address,settings.portal_url);
 if(portal.empty())fail("No account portal applies to "+settings.server_address+". Add --portal-url with that server's account portal.");
 if(isatty(0))std::cerr<<"Paste the device token, then press Enter and Ctrl+D:"<<std::endl;
 std::string input;char buffer[1024];ssize_t n;
 while(input.size()<=4096&&(n=read(0,buffer,sizeof buffer))!=0){if(n<0){if(errno==EINTR)continue;fail("Cannot read the token from standard input.");}input.append(buffer,n);}
 std::string token;
 if(!mag::portal::read_device_token(input,token))fail("Standard input holds no device token. Pass the token the portal showed (it starts with mgd_).");
 if(changed&&!mag::portal::write_private_file(path,mag::format_portable_settings(settings),error))fail(error);
 auto transport=mag::portal::make_platform_transport();mag::portal::Link link;
 auto outcome=mag::portal::import_device_token(*transport,portal,settings.server_address,token,link_file,link);
 if(!outcome.ok())fail(outcome.message);
 if(link.player_name!=settings.player_name){settings.player_name=link.player_name;mag::portal::write_private_file(path,mag::format_portable_settings(settings),error);}  // the account's name wins, as at launch
 std::cout<<"Linked as "<<link.player_name<<" at "<<portal<<" for "<<settings.server_address<<". Start ./MAG to play.\n";
 return 0;
}
static int unlink_account(const fs::path& link_file) {
 mag::portal::Link link;bool exists=false;std::string error;mag::portal::load_link(link_file,link,exists,error);
 if(!mag::portal::forget_link(link_file,error))fail(error);
 if(!exists){std::cout<<"MAG is not linked to an account on this computer.\n";return 0;}
 std::cout<<"MAG is no longer linked on this computer."<<(link.portal.empty()?std::string():" To also remove it from your account, use "+link.portal+"/account.")<<"\n";
 return 0;
}
// Before each phase: a fresh ticket from the linked account, or the name sign-in when unlinked.
// The launcher phase also takes the ticket's player name, which the game then saves.
static void portal_ticket(std::map<std::string,std::string>& env,const fs::path& link_file,const fs::path& ticket,
                          const mag::PortableSettings& settings,bool launcher) {
 env.erase("MAG_PLAYER_PORTAL_TICKET");
 const std::string portal=mag::portal::portal_for(settings.server_address,settings.portal_url);
 if(portal.empty()||settings.server_address.empty())return;
 if(!settings.ticket_file.empty()){if(launcher)std::cerr<<"MAG account: the ticket_file setting is used instead of the account link.\n";return;}
 auto transport=mag::portal::make_platform_transport();
 auto result=mag::portal::prepare_launch_ticket(*transport,link_file,ticket,portal,settings.server_address,relink);
 using State=mag::portal::LaunchTicket::State;
 if(result.state==State::Ready){
  env["MAG_PLAYER_PORTAL_TICKET"]=result.file.string();
  if(launcher&&result.player_name!=settings.player_name){
   env["MAG_PLAYER_SET_NAME"]=result.player_name;
   std::cerr<<"MAG account: you play as "<<result.player_name<<", the name on your linked account"
            <<(settings.player_name.empty()?std::string():" (MAG had "+settings.player_name+")")<<". MAG saved it as your player name.\n";
  }
  return;
 }
 if(result.state!=State::Failed){if(!result.message.empty()&&launcher)std::cerr<<"MAG account: "<<result.message<<"\n";return;}
 if(!mag::portal::fall_back_to_name_sign_in)fail(result.message);
 std::cerr<<"MAG account: "<<result.message<<" This time MAG signs in with your player name instead.\n";
}
int main(int argc,char** argv) {
 umask(0077);
 try {
 // First-run choices; the game validates and saves them in the player's settings file.
 std::map<std::string,std::string> choices;std::string account;
 // --no-prompt (or MAG_SKIP_SETUP=1 from the caller): no settings window, for scripts and test lanes.
 bool prompt=true;
 for(int i=1;i<argc;i++){std::string a=argv[i];
  if(a=="--help"||a=="-h"){std::cout<<usage<<"\n";return 0;}
  if(a=="--no-prompt"){prompt=false;continue;}
  if(a=="--link"||a=="--unlink"||a=="--link-token"){account=a;continue;}
  std::string key=a.rfind("--server",0)==0?"MAG_PLAYER_SET_SERVER":a.rfind("--name",0)==0?"MAG_PLAYER_SET_NAME":a.rfind("--portal-url",0)==0?"portal_url":"";
  if(key.empty())fail(std::string(usage)+(a.rfind("-",0)==0?" (unknown option "+a+")":" (unexpected argument; a device token goes on standard input)"));
  auto eq=a.find('=');if(eq!=std::string::npos)choices[key]=a.substr(eq+1);else if(i+1<argc&&(a=="--server"||a=="--name"||a=="--portal-url"))choices[key]=argv[++i];else fail(usage);}
 // --portal-url names the portal a token is for; --link-token takes the account's name.
 if((choices.count("portal_url")&&account!="--link-token")||(account=="--link-token"&&choices.count("MAG_PLAYER_SET_NAME")))fail(usage);
 const char* config=getenv("XDG_CONFIG_HOME");fs::path settings;
 if(config&&*config=='/')settings=fs::path(config)/"MAG";else if(const char* h=getenv("HOME");h&&*h=='/')settings=fs::path(h)/".config/MAG";
 else fail("Cannot find your configuration folder (set HOME or XDG_CONFIG_HOME)");
 fs::create_directories(settings.parent_path());private_directory(settings);settings/="settings.json";
 const fs::path link_file=mag::portal::link_path(settings);bool settings_usable=true;
 if(account=="--unlink")return unlink_account(link_file);
 if(account=="--link"){auto chosen=player_settings(settings,choices,settings_usable);
  if(!settings_usable)fail("Fix or delete "+settings.string()+" first: it is damaged.");
  return link_account(link_file,mag::portal::portal_for(chosen.server_address,chosen.portal_url),settings);}
 if(account=="--link-token")return link_token(link_file,settings,choices);
 char self[65536];auto n=readlink("/proc/self/exe",self,sizeof self-1);if(n<=0||n==(ssize_t)sizeof self-1)fail("Cannot locate MAG executable");self[n]=0;
 // The game files are in the MAG folder, beside the folder that holds this executable.
 fs::path app=fs::path(self).parent_path();root=app.parent_path();auto vfs=(app/MAG_GAME_ROOT).lexically_normal();
 {std::error_code none;if(!fs::is_directory(vfs/"PS3_GAME",none))fail("The game files are missing. Keep the game folder beside the folder that holds MAG: "+(vfs/"PS3_GAME").string());}
 auto launcher=app/"bin/MagRecomp",engine=app/"bin/MagEngine",le=vfs/"PS3_GAME/USRDIR/EBOOT.elf",ee=vfs/"PS3_GAME/USRDIR/bin/mage_g.elf",es=vfs/"PS3_GAME/USRDIR/bin/mage_g.self";
 check(launcher,MAG_LAUNCHER_SHA256);check(engine,MAG_ENGINE_SHA256);check(le,MAG_LAUNCHER_ELF_SHA256);check(ee,MAG_ENGINE_ELF_SHA256);check(es,MAG_ENGINE_SELF_SHA256);auto key=key_hash(le);
 unsigned char random[16];int rng=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(rng<0)fail("No system random source");size_t got=0;while(got<16){auto c=read(rng,random+got,16-got);if(c<0&&errno==EINTR)continue;if(c<=0)fail("No system randomness");got+=c;}close(rng);auto token=hex(random,16);
 auto logs=root/"logs"/("launch-"+token);private_directory(logs.parent_path());if(!fs::create_directory(logs))fail("Cannot create private launch directory");auto request=logs/"handoff-request.json";private_directory(root/"home");private_directory(root/"home/gamedata");private_directory(root/"home/player");{std::error_code none;fs::remove(root/"home/player/launch-message.txt",none);}
 std::map<std::string,std::string> env={{"PATH","/usr/bin:/bin"},{"HOME",(root/"home").string()},{"LANG","C.UTF-8"},{"LD_LIBRARY_PATH",(app/"lib").string()},{"PS3_VFS_ROOT",vfs.string()},{"PS3_BOOT_PATH","/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN"},{"PS3RECOMP_VK_WINDOW","1"},{"PS3RECOMP_VK_GUEST_PROGRAMS","1"},{"PS3_HLE_UNRESOLVED","halt"},{"PS3_SYSCALL_UNRESOLVED","halt"},{"MAG_HANDOFF_REQUEST",request.string()},{"MAG_HANDOFF_TOKEN",token},{"MAG_ENGINE_BINARY",engine.string()},{"MAG_ENGINE_ELF",ee.string()},{"MAG_ENGINE_SELF",es.string()},{"MAG_ENGINE_BINARY_SHA256",MAG_ENGINE_SHA256},{"MAG_ENGINE_ELF_SHA256",MAG_ENGINE_ELF_SHA256},{"MAG_ENGINE_SELF_SHA256",MAG_ENGINE_SELF_SHA256},{"MAG_ENGINE_KEY_SHA256",key},
  {"PS3_GAMEDATA_ROOT",(root/"home/gamedata").string()},{"MAG_PLAYER_SETTINGS_PATH",settings.string()},{"MAG_PLAYER_OVERLAY_ROOT",(root/"home/player").string()},{"MAG_PLAYER_SETUP","1"}};
 for(auto& c:choices)env[c.first]=c.second;
 // This supervisor opens setup itself, below; the launcher never does.
 env["MAG_SKIP_SETUP"]="1";
 // Required runtime defaults recorded at packaging (movie playback for decoder builds).
 for(auto& d:std::map<std::string,std::string>MAG_RUNTIME_DEFAULTS)env[d.first]=d.second;
 // Preserve only desktop transport/session variables, never inherited game diagnostics.
 for(auto name:{"DISPLAY","WAYLAND_DISPLAY","XDG_RUNTIME_DIR","DBUS_SESSION_BUS_ADDRESS","XAUTHORITY","PULSE_SERVER","PIPEWIRE_REMOTE"})if(auto value=getenv(name))env[name]=value;
 const fs::path ticket=root/"home/player/portal-ticket.bin";
 // Setup at every launch: the settings window, filled with the saved settings, so Play MAG is one
 // click. Command-line choices, --no-prompt, MAG_SKIP_SETUP and a session without a desktop go
 // straight in.
 const char* skip=getenv("MAG_SKIP_SETUP");
 const bool desktop=env.count("DISPLAY")||env.count("WAYLAND_DISPLAY");
 const bool interactive=prompt&&desktop&&!(skip&&*skip&&std::string(skip)!="0");
 signal(SIGINT,interrupted);signal(SIGTERM,interrupted);signal(SIGHUP,interrupted);
 if(interactive&&choices.empty()){
  int chosen=run_settings(engine,logs,env,nullptr);
  if(chosen==3)return 0;  // Cancel: the player chose not to play
  if(chosen)std::cerr<<"The settings window could not open; starting with the saved settings.\n";
 }
 auto launch_settings=player_settings(settings,choices,settings_usable);
 portal_ticket(env,link_file,ticket,launch_settings,true);
 int result=run(launcher,le,logs,env,"launcher.log");
 // A server needs a player name. When the game reports none, the settings window asks for it (or
 // for the account link, which supplies it), then the launcher starts again.
 {std::error_code none;const auto marker=root/"home/player/needs-player-name";
  for(int attempt=2;result!=90&&interactive&&attempt<=4&&fs::exists(fs::symlink_status(marker,none));attempt++){
   int answer=run_settings(engine,logs,env,"--notice=player-name");
   if(answer==3)return 0;
   if(answer)break;  // no window: the launch message below explains
   fs::remove(marker,none);fs::remove(root/"home/player/launch-message.txt",none);
   portal_ticket(env,link_file,ticket,player_settings(settings,choices,settings_usable),true);  // linked meanwhile
   result=run(launcher,le,logs,env,("launcher-"+std::to_string(attempt)+".log").c_str());
  }}
 if(result!=90){
  // A launch the player must correct (such as choosing a name) explains itself.
  std::error_code none;auto note=root/"home/player/launch-message.txt";
  if(fs::is_regular_file(fs::symlink_status(note,none))){auto text=read_file(note,4096);fs::remove(note,none);fail(text);}
  if(result)fail("The game stopped before it started (exit "+std::to_string(result)+"). Details: "+(logs/"launcher.log").string());return 0;}
 auto expected="{\"version\":1,\"token\":\""+token+"\",\"guest_path\":\"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self\",\"kind\":\"mage_g\"}\n";
 if(read_file(request,4096)!=expected)fail("Engine handoff request identity mismatch");
 check(engine,MAG_ENGINE_SHA256);check(ee,MAG_ENGINE_ELF_SHA256);check(es,MAG_ENGINE_SELF_SHA256);
 for(auto i=env.begin();i!=env.end();)if(i->first.find("MAG_HANDOFF_")==0||i->first.find("MAG_ENGINE_")==0||i->first=="MAG_PLAYER_SETUP"||i->first=="MAG_PLAYER_SET_SERVER"||i->first=="MAG_PLAYER_SET_NAME"||i->first=="MAG_SKIP_SETUP")i=env.erase(i);else ++i;
 // The engine signs in again (MAS, statistics) and gets its own fresh ticket; it reads the name the
 // launcher saved, so the settings are read again here.
 {std::map<std::string,std::string> none;portal_ticket(env,link_file,ticket,player_settings(settings,none,settings_usable),false);}
 env["PS3_BOOT_PATH"]="/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self";return run(engine,ee,logs,env,"engine.log");
 }catch(const std::exception& e){std::string msg="MAG could not start: "+std::string(e.what())+"\n";std::cerr<<msg;if(!root.empty()){int fd=open((root/"launch-error.log").c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd>=0){write(fd,msg.data(),msg.size());close(fd);}}return 1;}
}
'''


# Portable sources the supervisor compiles with it (HTTPS through OpenSSL's libssl).
SUPERVISOR_SOURCES = ('player_portal.h', 'player_portal.cpp', 'player_portal_openssl.cpp', 'player_settings_portable.h',
                      'player_settings_file.cpp', 'player_settings_text.h', 'player_sign_in.h')


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
    """Build the MAG folder's MAG and game subfolders from a finished workspace; launches nothing."""
    from .native_package import _ordinary, _digest, check_workspace, stage_game, package_receipt, GAME_FOLDER, WORKSPACE_GAME
    token = token or Cancellation(); events = events or (lambda event: None)
    workspace, destination = Path(workspace).absolute(), Path(destination).absolute()
    if destination.exists() or destination.is_symlink(): raise InstallerError('MAG folder destination already exists')
    if destination.name != 'MAG' or not destination.parent.is_dir(): raise InstallerError('Choose a new MAG folder under the installation directory')
    workspace, folder = check_workspace(workspace, destination.parent)
    manifest = json.loads(contained(workspace, 'profiles/retail-v1.0.json').read_text(encoding='utf-8'))
    framework = json.loads(contained(workspace, 'profiles/framework.json').read_text(encoding='utf-8'))
    records, originals = {}, {}
    for role, folder_name, image in [('launcher', 'build', 'EBOOT'), ('engine', 'build-engine', 'mage_g')]:
        binary = _ordinary(contained(workspace, folder_name + '/MagRecomp'))
        receipt = _ordinary(binary.with_suffix('.receipt.json'))
        record = json.loads(receipt.read_text(encoding='utf-8')); expected = manifest['images'][image]
        if (record.get('binary_sha256') != _digest(binary, token) or record.get('elf_sha256') != expected['elf_sha256'] or record.get('image_name') != image):
            raise InstallerError(f'Native source receipt mismatch: {receipt}')
        if record.get('sdk_revision') not in set(framework['strict_runtime_revisions']) & set(framework['strict_hle_runtime_revisions']):
            raise InstallerError('Packaged runtime must support strict syscall/HLE policies')
        relative = WORKSPACE_GAME + '/PS3_GAME/USRDIR/' + ('EBOOT.elf' if role == 'launcher' else 'bin/mage_g.elf')
        if _digest(_ordinary(contained(workspace, relative)), token) != record['elf_sha256']:
            raise InstallerError(f'Native runtime ELF identity mismatch: {relative}')
        records[role], originals[role] = record, (binary, receipt)
    destination.mkdir(mode=0o700)
    try:
        game = stage_game(workspace, folder, token, events)
        bin_dir = destination / 'bin'; bin_dir.mkdir()
        resources = destination / 'resources'; resources.mkdir()
        package_receipts = resources / 'package-receipts'; package_receipts.mkdir()
        for role, name in [('launcher', 'MagRecomp'), ('engine', 'MagEngine')]:
            source, receipt = originals[role]
            shutil.copyfile(source, bin_dir / name)
            (bin_dir / name).chmod(0o755)
            record = package_receipt(records[role], bin_dir / name, receipt, token)
            (package_receipts / (role + '.json')).write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        game_relative = Path(os.path.relpath(game, destination)).as_posix()
        engine_self = manifest['images']['mage_g']['source_sha256']
        runtime = {'schema_version': 2, 'game': game_relative,
                   'launcher': 'bin/MagRecomp', 'engine': 'bin/MagEngine',
                   'engine_self_sha256': engine_self, 'signing': 'unsigned-local'}
        scratch = destination / '.supervisor-build'; scratch.mkdir()
        definitions = {
            'MAG_GAME_ROOT': game_relative,
            'MAG_LAUNCHER_SHA256': _digest(bin_dir / 'MagRecomp', token),
            'MAG_ENGINE_SHA256': _digest(bin_dir / 'MagEngine', token),
            'MAG_LAUNCHER_ELF_SHA256': records['launcher']['elf_sha256'],
            'MAG_ENGINE_ELF_SHA256': records['engine']['elf_sha256'],
            'MAG_ENGINE_SELF_SHA256': engine_self,
        }
        from .native_package import playback_environment
        defaults = playback_environment(records)
        runtime['environment'] = defaults
        (scratch / 'supervisor_config.h').write_text(''.join(
            '#define ' + key + ' ' + json.dumps(value, ensure_ascii=False) + '\n'
            for key, value in definitions.items()) + '#define MAG_RUNTIME_DEFAULTS {' + ','.join(
            '{' + json.dumps(key) + ',' + json.dumps(value) + '}' for key, value in sorted(defaults.items())) + '}\n',
            encoding='utf-8')
        source = scratch / 'supervisor.cpp'
        source.write_text(_SUPERVISOR, encoding='utf-8')
        # The account link and ticket fetch, and the settings rules, from the workspace's own
        # source: the same code the game binaries were built from.
        for name in SUPERVISOR_SOURCES:
            shutil.copyfile(_ordinary(contained(workspace, 'src/' + name)), scratch / name)
        from .native_package import _run_tool
        compiler = config.get('linux_toolchain', {}).get('cxx', 'c++')
        _run_tool([compiler, '-std=c++20', '-O2', '-I' + str(scratch), str(source),
                   *[str(scratch / name) for name in SUPERVISOR_SOURCES if name.endswith('.cpp')],
                   '-o', str(destination / 'MAG'), '-lssl', '-lcrypto', '-Wl,-rpath,$ORIGIN/lib'], token)
        _ordinary(destination / 'MAG').chmod(0o755)
        libraries = destination / 'lib'; libraries.mkdir()
        runtime['libraries'] = _copy_dependencies(
            [destination / 'MAG', bin_dir / 'MagRecomp', bin_dir / 'MagEngine'], libraries, token,
            [config['ffmpeg'][name] for name in ('avcodec', 'avformat', 'avutil', 'swscale')]
            if 'ffmpeg' in config else [])
        (resources / 'runtime.json').write_text(json.dumps(runtime, indent=2) + '\n', encoding='utf-8')
        shutil.rmtree(scratch)
        events({'event': 'app-packaged', 'output': str(destination), 'launcher': str(destination / 'MAG'),
                'message': 'MAG was packaged. Start it with MAG; its MAG folder is self-contained, so move it as a whole.'})
        return destination
    except BaseException:
        shutil.rmtree(destination, ignore_errors=True)
        shutil.rmtree(folder / GAME_FOLDER, ignore_errors=True)  # check_workspace: it was not there
        raise
