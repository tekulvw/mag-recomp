#include "retail_handoff.h"
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__) || defined(_WIN32) || defined(__linux__)
#include <cerrno>
#include <string>
#if defined(__APPLE__) || defined(__linux__)
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <cwchar>
#endif

extern "C" {
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
}

namespace {
constexpr char guest_path[]="/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self";
constexpr const char* required_env[]={
    "MAG_HANDOFF_REQUEST", "MAG_HANDOFF_TOKEN", "MAG_ENGINE_BINARY",
    "MAG_ENGINE_ELF", "MAG_ENGINE_SELF", "MAG_ENGINE_BINARY_SHA256",
    "MAG_ENGINE_ELF_SHA256", "MAG_ENGINE_SELF_SHA256", "MAG_ENGINE_KEY_SHA256"
};

[[noreturn]] void reject(const char* reason) {
    std::fprintf(stderr,"[mag-handoff] rejected: %s\n",reason);
    std::fflush(stderr);
    std::_Exit(89);
}

bool lower_hex(const char* value, size_t length) {
    if (!value || std::strlen(value)!=length) return false;
    for (size_t i=0;i<length;i++)
        if (!((value[i]>='0' && value[i]<='9') || (value[i]>='a' && value[i]<='f')))
            return false;
    return true;
}

const uint8_t* guest_bytes(uint64_t address, size_t length) {
    // The runtime uses zero to denote a fully backed 32-bit guest space.
    const uint64_t vm_end=ppu_vm_size?ppu_vm_size:UINT64_C(0x100000000);
    if (!vm_base || !address || address>=vm_end || length>vm_end-address)
        reject("guest pointer outside known VM bounds");
    return vm_base+address;
}

#ifdef __APPLE__
constexpr size_t digest_length=CC_SHA256_DIGEST_LENGTH;
struct Sha256 {
    CC_SHA256_CTX state;
    Sha256() { CC_SHA256_Init(&state); }
    void update(const void* data, size_t length) { CC_SHA256_Update(&state,data,static_cast<CC_LONG>(length)); }
    void final(unsigned char* digest) { CC_SHA256_Final(digest,&state); }
};

#elif defined(__linux__)
constexpr size_t digest_length=32;
struct Sha256 {
    EVP_MD_CTX* state=EVP_MD_CTX_new();
    Sha256() { if (!state || EVP_DigestInit_ex(state,EVP_sha256(),nullptr)!=1) reject("SHA256 unavailable"); }
    ~Sha256() { EVP_MD_CTX_free(state); }
    void update(const void* data,size_t length) { if(EVP_DigestUpdate(state,data,length)!=1) reject("SHA256 update failed"); }
    void final(unsigned char* digest) { unsigned n=0;if(EVP_DigestFinal_ex(state,digest,&n)!=1 || n!=32) reject("SHA256 finish failed"); }
};
#endif
#if defined(__APPLE__) || defined(__linux__)
bool absolute_path(const char* path) { return path && path[0]=='/'; }
#else
constexpr size_t digest_length=32;
// One process-wide provider; the object for each hash is owned by BCrypt.
struct Sha256 {
    BCRYPT_HASH_HANDLE hash=nullptr;
    Sha256() {
        static BCRYPT_ALG_HANDLE algorithm=[] {
            BCRYPT_ALG_HANDLE handle=nullptr;
            if (BCryptOpenAlgorithmProvider(&handle,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) handle=nullptr;
            return handle;
        }();
        if (!algorithm || BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0) reject("SHA256 is unavailable");
    }
    ~Sha256() { if (hash) BCryptDestroyHash(hash); }
    void update(const void* data, size_t length) {
        if (BCryptHashData(hash,static_cast<PUCHAR>(const_cast<void*>(data)),static_cast<ULONG>(length),0)<0) reject("SHA256 update failed");
    }
    void final(unsigned char* digest) {
        if (BCryptFinishHash(hash,digest,static_cast<ULONG>(digest_length),0)<0) reject("SHA256 finish failed");
    }
};

// Drive-letter or UNC; the supervisor supplies only full local paths.
bool absolute_path(const char* path) {
    if (!path) return false;
    const bool drive=((path[0]>='A' && path[0]<='Z') || (path[0]>='a' && path[0]<='z')) && path[1]==':' && (path[2]=='\\' || path[2]=='/');
    return drive || (path[0]=='\\' && path[1]=='\\');
}

// Environment values are read wide so non-ANSI installation folders survive.
std::wstring wide_env(const char* key) {
    std::wstring name(key,key+std::strlen(key));
    const wchar_t* value=_wgetenv(name.c_str());
    return value?std::wstring(value):std::wstring();
}
#endif

void sha256(const void* data, size_t length, unsigned char* digest) {
    Sha256 state;state.update(data,length);state.final(digest);
}

void check_digest(const unsigned char* digest, const char* expected) {
    if (!lower_hex(expected,64)) reject("invalid configured SHA256");
    char actual[65];
    constexpr char hex[]="0123456789abcdef";
    for (size_t i=0;i<32;i++) {
        actual[i*2]=hex[digest[i]>>4];actual[i*2+1]=hex[digest[i]&15];
    }
    actual[64]=0;
    if (std::strcmp(actual,expected)) reject("prepared input identity mismatch");
}

#if defined(__APPLE__) || defined(__linux__)
void check_file(const char* key, const char* expected) {
    const char* path=std::getenv(key);
    if (!absolute_path(path) || !lower_hex(expected,64))
        reject("prepared files require absolute paths and SHA256 identities");
    // Nonblocking open prevents an unexpected FIFO from hanging before fstat.
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) reject("prepared file could not be opened");
    struct stat info{};
    if (fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_size<0) {
        close(fd);reject("prepared file is not a regular file");
    }
    Sha256 hash;
    uint8_t buffer[65536];
    uint64_t remaining=static_cast<uint64_t>(info.st_size);
    while (remaining) {
        size_t amount=remaining<sizeof(buffer)?static_cast<size_t>(remaining):sizeof(buffer);
        ssize_t count=read(fd,buffer,amount);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) { close(fd);reject("prepared file changed or could not be read"); }
        hash.update(buffer,static_cast<size_t>(count));
        remaining-=static_cast<uint64_t>(count);
    }
    ssize_t trailing;
    do { trailing=read(fd,buffer,1); } while (trailing<0 && errno==EINTR);
    close(fd);
    if (trailing!=0) reject("prepared file changed while hashing");
    unsigned char digest[digest_length];hash.final(digest);
    check_digest(digest,expected);
}
#else
void check_file(const char* key, const char* expected) {
    if (!absolute_path(std::getenv(key)) || !lower_hex(expected,64))
        reject("prepared files require absolute paths and SHA256 identities");
    const std::wstring path=wide_env(key);
    // Refuse reparse points (links/junctions) instead of following them.
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if (file==INVALID_HANDLE_VALUE) reject("prepared file could not be opened");
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (GetFileType(file)!=FILE_TYPE_DISK || !GetFileInformationByHandle(file,&info) ||
        (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file,&size) || size.QuadPart<0) {
        CloseHandle(file);reject("prepared file is not a regular file");
    }
    Sha256 hash;
    static uint8_t buffer[65536];
    uint64_t remaining=static_cast<uint64_t>(size.QuadPart);
    while (remaining) {
        DWORD amount=remaining<sizeof(buffer)?static_cast<DWORD>(remaining):static_cast<DWORD>(sizeof(buffer));
        DWORD count=0;
        if (!ReadFile(file,buffer,amount,&count,nullptr) || !count) { CloseHandle(file);reject("prepared file changed or could not be read"); }
        hash.update(buffer,count);
        remaining-=count;
    }
    DWORD trailing=0;
    const BOOL read_ok=ReadFile(file,buffer,1,&trailing,nullptr);
    CloseHandle(file);
    if (!read_ok || trailing!=0) reject("prepared file changed while hashing");
    unsigned char digest[digest_length];hash.final(digest);
    check_digest(digest,expected);
}
#endif

void validate_mapping(ppu_context* ctx) {
    if (!ctx) reject("missing PPU context");
    if (!lower_hex(std::getenv("MAG_HANDOFF_TOKEN"),32)) reject("invalid handoff token");
    const char* request=std::getenv("MAG_HANDOFF_REQUEST");
    if (!absolute_path(request)) reject("handoff request requires an absolute path");
    const uint8_t* key=guest_bytes(ctx->gpr[3],16);
    unsigned char digest[digest_length];sha256(key,16,digest);
    check_digest(digest,std::getenv("MAG_ENGINE_KEY_SHA256"));
    char path[1024];bool terminated=false;
    for (size_t i=0;i<sizeof(path);i++) {
        if (ctx->gpr[4]>UINT64_MAX-i) reject("guest path address overflow");
        path[i]=static_cast<char>(*guest_bytes(ctx->gpr[4]+i,1));
        if (!path[i]) { terminated=true;break; }
    }
    if (!terminated || std::strcmp(path,guest_path)) reject("unsupported guest executable path");
    check_file("MAG_ENGINE_BINARY",std::getenv("MAG_ENGINE_BINARY_SHA256"));
    check_file("MAG_ENGINE_ELF",std::getenv("MAG_ENGINE_ELF_SHA256"));
    check_file("MAG_ENGINE_SELF",std::getenv("MAG_ENGINE_SELF_SHA256"));
}

void available(ppu_context* ctx) {
    validate_mapping(ctx);
    std::fprintf(stderr,"[mag-handoff] exact prepared engine is available\n");
    ctx->gpr[3]=0;
}

std::string request_body() {
    // Token is lowercase hex and the other fields are constants: no escaping needed.
    std::string body="{\"version\":1,\"token\":\"";
    body+=std::getenv("MAG_HANDOFF_TOKEN");
    body+="\",\"guest_path\":\"";body+=guest_path;body+="\",\"kind\":\"mage_g\"}\n";
    return body;
}

[[noreturn]] void requested() {
    std::fprintf(stderr,"[mag-handoff] launcher requested mage_g process replacement\n");
    std::fflush(stdout);std::fflush(stderr);
    std::_Exit(90);
}

#if defined(__APPLE__) || defined(__linux__)
[[noreturn]] void publish_request() {
    const char* destination=std::getenv("MAG_HANDOFF_REQUEST");
    std::string temp=std::string(destination)+".tmp."+std::to_string(getpid());
    int fd=open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) reject("could not create exclusive handoff request");
    const std::string body=request_body();
    size_t written=0;bool failed=false;
    while (written<body.size()) {
        ssize_t count=write(fd,body.data()+written,body.size()-written);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) { failed=true;break; }
        written+=static_cast<size_t>(count);
    }
    if (fsync(fd)) failed=true;
    if (close(fd)) failed=true;
    // link publishes atomically and refuses to replace an existing destination.
    if (!failed && link(temp.c_str(),destination)) failed=true;
    if (unlink(temp.c_str())) failed=true;
    if (failed) reject("could not publish exclusive handoff request");
    requested();
}
#else
[[noreturn]] void publish_request() {
    const std::wstring destination=wide_env("MAG_HANDOFF_REQUEST");
    const std::wstring temp=destination+L".tmp."+std::to_wstring(GetCurrentProcessId());
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if (file==INVALID_HANDLE_VALUE) reject("could not create exclusive handoff request");
    const std::string body=request_body();
    DWORD written=0;
    bool failed=!WriteFile(file,body.data(),static_cast<DWORD>(body.size()),&written,nullptr) || written!=body.size();
    if (!FlushFileBuffers(file)) failed=true;
    if (!CloseHandle(file)) failed=true;
    // Without MOVEFILE_REPLACE_EXISTING the rename refuses an existing destination.
    if (!failed && !MoveFileExW(temp.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)) failed=true;
    if (failed) { DeleteFileW(temp.c_str());reject("could not publish exclusive handoff request"); }
    requested();
}
#endif

void spawn(ppu_context* ctx) {
    validate_mapping(ctx);
    if (ctx->gpr[5]) {
        const uint8_t* first=guest_bytes(ctx->gpr[5],4);
        if (first[0] || first[1] || first[2] || first[3])
            reject("nonempty BE32 spawn argv is unsupported");
    }
    if (ctx->gpr[6] || ctx->gpr[7] || ctx->gpr[8] || ctx->gpr[9]!=1001 ||
        ctx->gpr[10]!=UINT64_C(0x4000000000000000))
        reject("unsupported spawn environment, data, priority or flags");
    publish_request();
}
} // namespace
#endif

extern "C" void mag_register_handoff(void) {
#if defined(__APPLE__) || defined(_WIN32) || defined(__linux__)
    for (const char* key:required_env) {
        const char* value=std::getenv(key);
        if (!value || !*value) return;
    }
    ps3_hle_register_ctx(0xAD218FAF,"sceNpDrmIsAvailable [MAG prepared image]",available);
    ps3_hle_register_ctx(0xE6C8F3F9,"sceNpDrmProcessExitSpawn2 [MAG prepared image]",spawn);
#endif
}
