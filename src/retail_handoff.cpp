#include "retail_handoff.h"
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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

void check_file(const char* path, const char* expected) {
    if (!path || path[0]!='/' || !lower_hex(expected,64))
        reject("prepared files require absolute paths and SHA256 identities");
    // Nonblocking open prevents an unexpected FIFO from hanging before fstat.
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) reject("prepared file could not be opened");
    struct stat info{};
    if (fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_size<0) {
        close(fd);reject("prepared file is not a regular file");
    }
    CC_SHA256_CTX hash;CC_SHA256_Init(&hash);
    uint8_t buffer[65536];
    uint64_t remaining=static_cast<uint64_t>(info.st_size);
    while (remaining) {
        size_t amount=remaining<sizeof(buffer)?static_cast<size_t>(remaining):sizeof(buffer);
        ssize_t count=read(fd,buffer,amount);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) { close(fd);reject("prepared file changed or could not be read"); }
        CC_SHA256_Update(&hash,buffer,static_cast<CC_LONG>(count));
        remaining-=static_cast<uint64_t>(count);
    }
    ssize_t trailing;
    do { trailing=read(fd,buffer,1); } while (trailing<0 && errno==EINTR);
    close(fd);
    if (trailing!=0) reject("prepared file changed while hashing");
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(digest,&hash);
    check_digest(digest,expected);
}

void validate_mapping(ppu_context* ctx) {
    if (!ctx) reject("missing PPU context");
    if (!lower_hex(std::getenv("MAG_HANDOFF_TOKEN"),32)) reject("invalid handoff token");
    const char* request=std::getenv("MAG_HANDOFF_REQUEST");
    if (!request || request[0]!='/') reject("handoff request requires an absolute path");
    const uint8_t* key=guest_bytes(ctx->gpr[3],16);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(key,16,digest);
    check_digest(digest,std::getenv("MAG_ENGINE_KEY_SHA256"));
    char path[1024];bool terminated=false;
    for (size_t i=0;i<sizeof(path);i++) {
        if (ctx->gpr[4]>UINT64_MAX-i) reject("guest path address overflow");
        path[i]=static_cast<char>(*guest_bytes(ctx->gpr[4]+i,1));
        if (!path[i]) { terminated=true;break; }
    }
    if (!terminated || std::strcmp(path,guest_path)) reject("unsupported guest executable path");
    check_file(std::getenv("MAG_ENGINE_BINARY"),std::getenv("MAG_ENGINE_BINARY_SHA256"));
    check_file(std::getenv("MAG_ENGINE_ELF"),std::getenv("MAG_ENGINE_ELF_SHA256"));
    check_file(std::getenv("MAG_ENGINE_SELF"),std::getenv("MAG_ENGINE_SELF_SHA256"));
}

void available(ppu_context* ctx) {
    validate_mapping(ctx);
    std::fprintf(stderr,"[mag-handoff] exact prepared engine is available\n");
    ctx->gpr[3]=0;
}

[[noreturn]] void publish_request() {
    const char* destination=std::getenv("MAG_HANDOFF_REQUEST");
    std::string temp=std::string(destination)+".tmp."+std::to_string(getpid());
    int fd=open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) reject("could not create exclusive handoff request");
    // Token is lowercase hex and the other fields are constants: no escaping needed.
    std::string body="{\"version\":1,\"token\":\"";
    body+=std::getenv("MAG_HANDOFF_TOKEN");
    body+="\",\"guest_path\":\"";body+=guest_path;body+="\",\"kind\":\"mage_g\"}\n";
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
    std::fprintf(stderr,"[mag-handoff] launcher requested mage_g process replacement\n");
    std::fflush(stdout);std::fflush(stderr);
    std::_Exit(90);
}

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
#ifdef __APPLE__
    for (const char* key:required_env) {
        const char* value=std::getenv(key);
        if (!value || !*value) return;
    }
    ps3_hle_register_ctx(0xAD218FAF,"sceNpDrmIsAvailable [MAG prepared image]",available);
    ps3_hle_register_ctx(0xE6C8F3F9,"sceNpDrmProcessExitSpawn2 [MAG prepared image]",spawn);
#endif
}
