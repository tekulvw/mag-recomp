/* Process-wide getenv cache for Windows builds.
 *
 * The runtime reads many of its diagnostic switches with getenv on hot paths,
 * some per FIFO packet or per guest memory access. The static UCRT getenv takes
 * the CRT environment lock and compares every variable on each call: about
 * 1.8 us per call with ~110 variables, and over 16 us when eight threads poll at
 * once, because the lock serialises them. src/mag_env_cache_win.h, force-
 * included into the runtime and MAG's sources, routes those calls here.
 *
 * The first lookup of a name asks the CRT and publishes an immutable record
 * holding a copy of the value; later lookups are a hash probe and a
 * case-insensitive compare, as Windows names are. _putenv, _putenv_s, _wputenv
 * and _wputenv_s forward to the CRT and then invalidate every record, so values
 * written at run time (the runtime defaults, the player settings) are seen
 * exactly as before. SetEnvironmentVariable changes only the OS block, which
 * the CRT's getenv never reads, so it needs no invalidation. Records are never
 * freed: a reader may still hold one across an invalidation or replacement, and
 * the set of names is small. The returned pointers therefore stay valid for the
 * life of the process. MAG_GETENV_CACHE=0 forwards every lookup to the CRT.
 *
 * Built off Windows only by the synthetic test (MAG_ENV_CACHE_POSIX_TEST),
 * which maps the CRT calls onto getenv/setenv/unsetenv. */
#define MAG_ENV_CACHE_IMPLEMENTATION 1
#undef getenv
#undef _putenv
#undef _putenv_s
#undef _wputenv
#undef _wputenv_s
#undef putenv

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>
#else
#include <mutex>
#include <string>
#include <strings.h>
typedef int errno_t;
#endif

namespace {

struct Record {
    uint64_t hash, generation;
    char* name;
    char* value;
};

constexpr unsigned kSlots = 4096, kProbe = 16;
std::atomic<Record*> g_slots[kSlots];
std::atomic<uint64_t> g_generation{1};
std::atomic<int> g_enabled{-1};

#ifdef _WIN32
SRWLOCK g_write = SRWLOCK_INIT;
struct WriteLock {
    WriteLock() { AcquireSRWLockExclusive(&g_write); }
    ~WriteLock() { ReleaseSRWLockExclusive(&g_write); }
};
int same_name(const char* a, const char* b) { return !_stricmp(a, b); }
#else
std::mutex g_write_mutex;
struct WriteLock {
    WriteLock() { g_write_mutex.lock(); }
    ~WriteLock() { g_write_mutex.unlock(); }
};
int same_name(const char* a, const char* b) { return !strcmp(a, b); }
#endif

uint64_t name_hash(const char* name)
{
    uint64_t h = 1469598103934665603ull;
    for (; *name; ++name) {
        unsigned char c = (unsigned char)*name;
#ifdef _WIN32
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
#endif
        h ^= c;
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}

char* copy_string(const char* s)
{
    if (!s) return nullptr;
    const size_t n = strlen(s) + 1;
    char* d = static_cast<char*>(malloc(n));
    if (d) memcpy(d, s, n);
    return d;
}

bool cache_enabled()
{
    int enabled = g_enabled.load(std::memory_order_acquire);
    if (enabled < 0) {
        WriteLock lock;
        const char* v = ::getenv("MAG_GETENV_CACHE");
        enabled = !(v && !strcmp(v, "0"));
        g_enabled.store(enabled, std::memory_order_release);
    }
    return enabled != 0;
}

void invalidate()
{
    g_generation.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace

extern "C" char* mag_env_getenv(const char* name)
{
    if (!name || !*name || strchr(name, '=') || !cache_enabled()) return ::getenv(name);
    const uint64_t hash = name_hash(name);
    const uint64_t generation = g_generation.load(std::memory_order_acquire);
    const unsigned home = (unsigned)(hash & (kSlots - 1));
    for (unsigned probe = 0; probe < kProbe; ++probe) {
        Record* r = g_slots[(home + probe) & (kSlots - 1)].load(std::memory_order_acquire);
        if (!r) break;
        if (r->generation == generation && r->hash == hash && same_name(r->name, name))
            return r->value;
    }

    WriteLock lock;
    const uint64_t now = g_generation.load(std::memory_order_acquire);
    const char* live = ::getenv(name);
    Record* fresh = new (std::nothrow) Record{hash, now, copy_string(name), copy_string(live)};
    if (!fresh || !fresh->name || (live && !fresh->value)) {
        if (fresh) { free(fresh->name); free(fresh->value); delete fresh; }
        return ::getenv(name);   /* out of memory: the CRT's own pointer */
    }
    /* The first empty, stale or same-name slot of the window takes the record;
     * a full window replaces its home slot. Replaced records stay allocated. */
    unsigned slot = home;
    for (unsigned probe = 0; probe < kProbe; ++probe) {
        const unsigned s = (home + probe) & (kSlots - 1);
        Record* r = g_slots[s].load(std::memory_order_relaxed);
        if (!r || r->generation != now || (r->hash == hash && same_name(r->name, name))) { slot = s; break; }
    }
    g_slots[slot].store(fresh, std::memory_order_release);
    return fresh->value;
}

#ifdef _WIN32
extern "C" int mag_env_putenv(const char* assignment)
{
    WriteLock lock;
    const int rc = ::_putenv(assignment);
    invalidate();
    return rc;
}

extern "C" errno_t mag_env_putenv_s(const char* name, const char* value)
{
    WriteLock lock;
    const errno_t rc = ::_putenv_s(name, value);
    invalidate();
    return rc;
}

extern "C" int mag_env_wputenv(const wchar_t* assignment)
{
    WriteLock lock;
    const int rc = ::_wputenv(assignment);
    invalidate();
    return rc;
}

extern "C" errno_t mag_env_wputenv_s(const wchar_t* name, const wchar_t* value)
{
    WriteLock lock;
    const errno_t rc = ::_wputenv_s(name, value);
    invalidate();
    return rc;
}
#else
/* Test build: the CRT's semantics on POSIX calls. An empty value removes the
 * variable, as _putenv_s does. */
extern "C" errno_t mag_env_putenv_s(const char* name, const char* value)
{
    WriteLock lock;
    const int rc = value && *value ? setenv(name, value, 1) : unsetenv(name);
    invalidate();
    return rc ? 22 : 0;
}

extern "C" int mag_env_putenv(const char* assignment)
{
    const char* eq = assignment ? strchr(assignment, '=') : nullptr;
    if (!eq) return -1;
    const std::string name(assignment, eq);
    return mag_env_putenv_s(name.c_str(), eq + 1) ? -1 : 0;
}
#endif
