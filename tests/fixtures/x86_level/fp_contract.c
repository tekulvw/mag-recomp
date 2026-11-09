/* Floating-point contraction regression: is a*b + c rounded twice (multiply, then add), as the PS3
 * does unless the guest asked for a fused multiply-add, or fused into one rounding?
 *
 * The inputs make the two answers differ: for a = 1 + e and b = 1 - e the exact product 1 - e*e
 * rounds to 1, so the separate result is exactly 0 while a fused result keeps -e*e. The inputs come
 * from volatile objects and each case sits in its own non-inlined function, so the compiler sees
 * a run-time multiply and add it is free to contract unless told not to.
 *
 * Prints one line per case, "<case> separate" or "<case> fused", and exits 0 only when every case
 * rounded separately. */
#include <stdio.h>

#define N 64

typedef float v4f __attribute__((vector_size(16)));

static volatile double vda = 1.0 + 0x1p-27, vdb = 1.0 - 0x1p-27, vdc = -1.0;
static volatile float vfa = 1.0f + 0x1p-13f, vfb = 1.0f - 0x1p-13f, vfc = -1.0f;

/* One expression: what Clang contracts by default once FMA is available. */
__attribute__((noinline)) static double double_expression(double a, double b, double c) { return a * b + c; }
__attribute__((noinline)) static float float_expression(float a, float b, float c) { return a * b + c; }

/* Across statements: what GCC contracts by default (-ffp-contract=fast). */
__attribute__((noinline)) static double double_statements(double a, double b, double c)
{
    double product = a * b;
    return product + c;
}

/* The vector forms the SPU code is written in, and a loop the optimizer may vectorize. */
__attribute__((noinline)) static v4f vector_expression(v4f a, v4f b, v4f c) { return a * b + c; }
__attribute__((noinline)) static void float_loop(float *out, const float *a, const float *b, const float *c)
{
    for (int i = 0; i < N; ++i) out[i] = a[i] * b[i] + c[i];
}

static int report(const char *name, int separate)
{
    printf("%s %s\n", name, separate ? "separate" : "fused");
    return separate ? 0 : 1;
}

int main(void)
{
    /* The separately rounded answers, forced through volatile stores. */
    volatile double dproduct = vda * vdb;
    volatile float fproduct = vfa * vfb;
    const double dseparate = dproduct + vdc;
    const float fseparate = fproduct + vfc;
    if (dseparate != 0.0 || fseparate != 0.0f) {
        printf("reference rounding is not round-to-nearest\n");
        return 2;
    }

    int failures = 0;
    failures += report("double-expression", double_expression(vda, vdb, vdc) == dseparate);
    failures += report("double-statements", double_statements(vda, vdb, vdc) == dseparate);
    failures += report("float-expression", float_expression(vfa, vfb, vfc) == fseparate);

    v4f va = {vfa, vfa, vfa, vfa}, vb = {vfb, vfb, vfb, vfb}, vc = {vfc, vfc, vfc, vfc};
    v4f vr = vector_expression(va, vb, vc);
    failures += report("float-vector", vr[0] == fseparate && vr[1] == fseparate && vr[2] == fseparate && vr[3] == fseparate);

    float a[N], b[N], c[N], r[N];
    for (int i = 0; i < N; ++i) { a[i] = vfa; b[i] = vfb; c[i] = vfc; }
    float_loop(r, a, b, c);
    int loop_separate = 1;
    for (int i = 0; i < N; ++i) loop_separate &= r[i] == fseparate;
    failures += report("float-loop", loop_separate);
    return failures ? 1 : 0;
}
