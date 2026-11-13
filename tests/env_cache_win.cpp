// Synthetic check of the Windows getenv cache (src/mag_env_cache_win.cpp).
// On Windows it is built like the engine: static CRT, the header force-included,
// so getenv and _putenv_s below are the cached ones. Elsewhere the same checks
// call the cache's functions directly over setenv/unsetenv.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
static char* env_get(const char* name) { return getenv(name); }
static int env_put(const char* name, const char* value) { return (int)_putenv_s(name, value); }
static int env_put_assignment(const char* assignment) { return _putenv(assignment); }
static char* crt_getenv(const char* name);
#else
extern "C" char* mag_env_getenv(const char* name);
extern "C" int mag_env_putenv_s(const char* name, const char* value);
extern "C" int mag_env_putenv(const char* assignment);
static char* env_get(const char* name) { return mag_env_getenv(name); }
static int env_put(const char* name, const char* value) { return mag_env_putenv_s(name, value); }
static int env_put_assignment(const char* assignment) { return mag_env_putenv(assignment); }
static char* crt_getenv(const char* name) { return ::getenv(name); }
#endif

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static bool equals(const char* value, const char* expected)
{
    return value && expected && !std::strcmp(value, expected);
}

static bool cache_off();

static void functional()
{
    // A value the parent process exported is seen as is.
    CHECK(equals(env_get("MAG_ENV_TEST_INHERITED"), "from-parent"));
    CHECK(env_get("MAG_ENV_TEST_ABSENT") == nullptr);
    CHECK(env_get("MAG_ENV_TEST_ABSENT") == nullptr);   // cached absence

    // A write after a cached absence is seen: the cache is invalidated.
    CHECK(env_put("MAG_ENV_TEST_ABSENT", "now-set") == 0);
    CHECK(equals(env_get("MAG_ENV_TEST_ABSENT"), "now-set"));

    // An explicit override replaces an inherited value.
    CHECK(env_put("MAG_ENV_TEST_INHERITED", "override") == 0);
    CHECK(equals(env_get("MAG_ENV_TEST_INHERITED"), "override"));

    // A pointer handed out earlier keeps its old contents.
    CHECK(env_put("MAG_ENV_TEST_VALUE", "first") == 0);
    const char* first = env_get("MAG_ENV_TEST_VALUE");
    CHECK(equals(first, "first"));
    CHECK(env_put("MAG_ENV_TEST_VALUE", "second") == 0);
    CHECK(equals(env_get("MAG_ENV_TEST_VALUE"), "second"));
    if (!cache_off()) CHECK(equals(first, "first"));

    // An empty value removes the variable, as _putenv_s does.
    CHECK(env_put("MAG_ENV_TEST_VALUE", "") == 0);
    CHECK(env_get("MAG_ENV_TEST_VALUE") == nullptr);

    // NAME=VALUE assignments invalidate too.
    CHECK(env_put_assignment("MAG_ENV_TEST_ASSIGN=assigned") == 0);
    CHECK(equals(env_get("MAG_ENV_TEST_ASSIGN"), "assigned"));
    CHECK(env_put_assignment("MAG_ENV_TEST_ASSIGN=changed") == 0);
    CHECK(equals(env_get("MAG_ENV_TEST_ASSIGN"), "changed"));

    // Names with '=' and empty names are not cached, and agree with the CRT.
    CHECK(env_get("") == crt_getenv(""));
    CHECK(env_get("A=B") == nullptr);

#ifdef _WIN32
    // Windows names are case-insensitive.
    CHECK(equals(env_get("mag_env_test_assign"), "changed"));
    // Wide writes invalidate as well.
    CHECK(_wputenv_s(L"MAG_ENV_TEST_WIDE", L"wide") == 0);
    CHECK(equals(env_get("MAG_ENV_TEST_WIDE"), "wide"));
    CHECK(_wputenv(L"MAG_ENV_TEST_WIDE=wider") == 0);
    CHECK(equals(env_get("MAG_ENV_TEST_WIDE"), "wider"));
#endif

    // Back to the starting state, so a second pass checks the same things.
    CHECK(env_put("MAG_ENV_TEST_INHERITED", "from-parent") == 0);
    CHECK(env_put("MAG_ENV_TEST_ABSENT", "") == 0);
}

static void concurrent()
{
    // Readers see one of the two values a writer alternates between, never a torn
    // or freed one, while the cache is invalidated underneath them.
    CHECK(env_put("MAG_ENV_TEST_RACE", "one") == 0);
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::atomic<long> reads{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 8; ++t)
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const char* v = env_get("MAG_ENV_TEST_RACE");
                if (!equals(v, "one") && !equals(v, "two")) bad.fetch_add(1);
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    for (int i = 0; i < 2000; ++i) env_put("MAG_ENV_TEST_RACE", (i & 1) ? "one" : "two");
    stop.store(true);
    for (auto& r : readers) r.join();
    CHECK(bad.load() == 0);
    CHECK(reads.load() > 0);
}

static double ns_per_call(char* (*get)(const char*), const char* name, int threads)
{
    constexpr int kCalls = 200000;
    std::atomic<long long> sink{0};
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t)
        workers.emplace_back([&] {
            long long s = 0;
            for (int i = 0; i < kCalls; ++i) s += (long long)(size_t)get(name);
            sink.fetch_add(s);
        });
    for (auto& w : workers) w.join();
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    return ns / kCalls;
}

static void timing()
{
    // Timing is printed for comparison, never asserted.
    for (int i = 0; i < 110; ++i) {
        char name[48], value[64];
        std::snprintf(name, sizeof name, "MAG_ENV_TEST_FILLER_%03d", i);
        std::snprintf(value, sizeof value, "filler-value-%d-xxxxxxxxxxxxxxxxxxxxxxxx", i);
        env_put(name, value);
    }
    env_put("MAG_ENV_TEST_PRESENT", "1");
    for (int threads : {1, 8})
        for (const char* name : {"MAG_ENV_TEST_PRESENT", "MAG_ENV_TEST_NEVER_SET"})
            std::printf("[env_cache] %-24s threads=%d cached %.1f ns/call, CRT %.1f ns/call\n", name, threads,
                        ns_per_call(env_get, name, threads), ns_per_call(crt_getenv, name, threads));
}

static bool cache_off()
{
    const char* v = std::getenv("MAG_GETENV_CACHE");
    return v && !std::strcmp(v, "0");
}

int main()
{
    functional();
    // Uncached, a concurrent write may free the string a reader still holds:
    // that is the CRT's contract, and the reason the cache keeps every value.
    if (!cache_off()) concurrent();
    functional();   // again, over a table full of stale records
    timing();
    if (failures) {
        std::printf("[env_cache] %d failure(s)\n", failures);
        return 1;
    }
    std::printf("[env_cache] PASS\n");
    return 0;
}

#ifdef _WIN32
// The CRT's own getenv, for the timing comparison.
#undef getenv
extern "C" char* __cdecl getenv(const char* name);
static char* crt_getenv(const char* name) { return getenv(name); }
#endif
