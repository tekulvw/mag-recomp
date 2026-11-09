/* Configure-time probe (cmake/MagX86Level.cmake): does the build CPU, and the operating system
 * running on it, support every x86-64-v3 feature? Exits 0 when it does. Prints the features it
 * found missing, so the configure output says why a build stayed at the baseline.
 *
 * Built at the baseline level itself, so it runs on any x86-64 CPU. */
#include <stdio.h>

#if !defined(__x86_64__) && !defined(_M_X64)
#error "the x86-64 level probe only builds for x86-64"
#endif

#if defined(__GNUC__) || defined(__clang__)
/* GCC, Clang and clang-cl: inline assembly needs no target feature, unlike the xgetbv intrinsic. */
static void cpuid(unsigned leaf, unsigned subleaf, unsigned out[4])
{
    __asm__ volatile("cpuid" : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3]) : "a"(leaf), "c"(subleaf));
}

static unsigned long long xcr0(void)
{
    unsigned low, high;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return ((unsigned long long)high << 32) | low;
}
#else
#include <intrin.h>
static void cpuid(unsigned leaf, unsigned subleaf, unsigned out[4])
{
    int registers[4];
    __cpuidex(registers, (int)leaf, (int)subleaf);
    for (int i = 0; i < 4; ++i) out[i] = (unsigned)registers[i];
}

static unsigned long long xcr0(void) { return _xgetbv(0); }
#endif

enum { EAX, EBX, ECX, EDX };

struct feature {
    const char *name;
    int leaf; /* 1: leaf 1, 7: leaf 7 subleaf 0, 0x81: leaf 0x80000001 */
    int reg;
    int bit;
};

/* x86-64-v2 and x86-64-v3, as the psABI defines them (v1 is every x86-64 CPU). */
static const struct feature features[] = {
    {"sse3", 1, ECX, 0},     {"ssse3", 1, ECX, 9},   {"fma", 1, ECX, 12},    {"cx16", 1, ECX, 13},
    {"sse4.1", 1, ECX, 19},  {"sse4.2", 1, ECX, 20}, {"movbe", 1, ECX, 22},  {"popcnt", 1, ECX, 23},
    {"osxsave", 1, ECX, 27}, {"avx", 1, ECX, 28},    {"f16c", 1, ECX, 29},   {"bmi1", 7, EBX, 3},
    {"avx2", 7, EBX, 5},     {"bmi2", 7, EBX, 8},    {"lahf", 0x81, ECX, 0}, {"lzcnt", 0x81, ECX, 5},
};

int main(void)
{
    unsigned basic[4], leaf1[4] = {0}, leaf7[4] = {0}, extended[4], ext1[4] = {0};
    cpuid(0, 0, basic);
    cpuid(0x80000000u, 0, extended);
    if (basic[EAX] >= 1) cpuid(1, 0, leaf1);
    if (basic[EAX] >= 7) cpuid(7, 0, leaf7);
    if (extended[EAX] >= 0x80000001u) cpuid(0x80000001u, 0, ext1);

    int missing = 0;
    for (unsigned i = 0; i < sizeof features / sizeof features[0]; ++i) {
        const struct feature *f = &features[i];
        const unsigned *regs = f->leaf == 1 ? leaf1 : f->leaf == 7 ? leaf7 : ext1;
        if (!((regs[f->reg] >> f->bit) & 1u)) {
            printf("%s %s", missing ? "" : "missing:", f->name);
            missing = 1;
        }
    }
    /* AVX needs the operating system to save the YMM state too (XCR0 bits 1 and 2). xgetbv is
     * only valid once OSXSAVE says the OS enabled it. */
    if ((leaf1[ECX] >> 27) & 1u) {
        if ((xcr0() & 0x6u) != 0x6u) {
            printf("%s os-ymm-state", missing ? "" : "missing:");
            missing = 1;
        }
    }
    puts(missing ? "" : "x86-64-v3");
    return missing;
}
