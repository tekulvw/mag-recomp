/* Process-wide getenv cache for the engine binary (macOS).
 *
 * The SDK reads many of its diagnostic switches with getenv on hot paths, some
 * per FIFO packet. Darwin's getenv scans the whole environment under a lock;
 * with the runner's 100+ variables that was a quarter of the FIFO thread's
 * wall time in the menu. Defining getenv here binds every statically linked
 * caller (SDK, MAG, Rust std) to this lookup instead.
 *
 * The first lookup of a name scans environ directly and publishes an immutable
 * record; later lookups are a hash probe and a strcmp. setenv, unsetenv and
 * putenv forward to libSystem and invalidate every record, so values written at
 * runtime are seen exactly as before. Probes are bounded; a full probe window
 * replaces its home slot. Records are never freed: a reader may still hold
 * one across an invalidation or replacement, and the set of names is small.
 * MAG_GETENV_CACHE=0 forwards every lookup to the plain scan. */
#include <atomic>
#include <crt_externs.h>
#include <dlfcn.h>
#include <mutex>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct Record {
    uint64_t hash, generation;
    char* name;
    char* value;
};

constexpr unsigned kSlots = 4096, kProbe = 16;
std::atomic<Record*> g_slots[kSlots];
std::atomic<uint64_t> g_generation{1};
std::mutex g_write;

uint64_t name_hash(const char* name)
{
    uint64_t h = 1469598103934665603ull;
    for (; *name; ++name) { h ^= (unsigned char)*name; h *= 1099511628211ull; }
    return h ? h : 1;
}

/* The lookup getenv itself performs, without its lock. */
char* scan_environ(const char* name)
{
    const size_t len = strlen(name);
    for (char** e = *_NSGetEnviron(); e && *e; ++e)
        if (!strncmp(*e, name, len) && (*e)[len] == '=') return *e + len + 1;
    return nullptr;
}

bool cache_enabled()
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* v = scan_environ("MAG_GETENV_CACHE");
        enabled = !(v && !strcmp(v, "0"));
    }
    return enabled;
}

void invalidate()
{
    g_generation.fetch_add(1, std::memory_order_acq_rel);
}

template <class F> F next_symbol(const char* name)
{
    return reinterpret_cast<F>(dlsym(RTLD_NEXT, name));
}

} // namespace

extern "C" char* getenv(const char* name)
{
    if (!name || !*name || strchr(name, '=')) return nullptr;
    if (!cache_enabled()) return scan_environ(name);
    const uint64_t hash = name_hash(name);
    const uint64_t generation = g_generation.load(std::memory_order_acquire);
    unsigned slot = (unsigned)(hash & (kSlots - 1));
    for (unsigned probe = 0; probe < kProbe; ++probe, slot = (slot + 1) & (kSlots - 1)) {
        Record* r = g_slots[slot].load(std::memory_order_acquire);
        if (!r) break;
        if (r->generation == generation && r->hash == hash && !strcmp(r->name, name))
            return r->value;
        if (r->generation != generation) break;   /* stale table: refill below */
    }
    std::lock_guard<std::mutex> lock(g_write);
    const uint64_t now = g_generation.load(std::memory_order_acquire);
    char* value = scan_environ(name);
    Record* fresh = new (std::nothrow) Record{hash, now, strdup(name), value};
    if (!fresh || !fresh->name) return value;
    const unsigned home = (unsigned)(hash & (kSlots - 1));
    slot = home;
    for (unsigned probe = 0; probe < kProbe; ++probe, slot = (slot + 1) & (kSlots - 1)) {
        Record* r = g_slots[slot].load(std::memory_order_relaxed);
        if (!r || r->generation != now) { g_slots[slot].store(fresh, std::memory_order_release); return value; }
        if (r->hash == hash && !strcmp(r->name, name)) return value;   /* raced: already cached */
    }
    g_slots[home].store(fresh, std::memory_order_release);
    return value;
}

extern "C" int setenv(const char* name, const char* value, int overwrite)
{
    static auto real = next_symbol<int (*)(const char*, const char*, int)>("setenv");
    std::lock_guard<std::mutex> lock(g_write);
    const int rc = real(name, value, overwrite);
    invalidate();
    return rc;
}

extern "C" int unsetenv(const char* name)
{
    static auto real = next_symbol<int (*)(const char*)>("unsetenv");
    std::lock_guard<std::mutex> lock(g_write);
    const int rc = real(name);
    invalidate();
    return rc;
}

extern "C" int putenv(char* string)
{
    static auto real = next_symbol<int (*)(char*)>("putenv");
    std::lock_guard<std::mutex> lock(g_write);
    const int rc = real(string);
    invalidate();
    return rc;
}
