/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 -- libm accuracy check, run on the host.
 *
 * Our libm is compiled natively, its symbols prefixed, and every function
 * compared against the host's glibc in ULPs. That is worth doing on the host
 * rather than in the OS because glibc is the reference to compare against,
 * and because a numerical regression is much easier to read here.
 *
 * Run it with tools/check-libm.sh.
 *
 * The tolerances are deliberately tight. The x87 transcendentals land within
 * a few ulp of correctly rounded; anything much worse means a real bug, and
 * the first version of this file caught exactly one -- an x87 stack slot
 * leaked per call, so the eighth call onwards returned NaN.
 */

#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* clang-format off */
/* One declaration per function under test, kept compact on purpose. */
#define D(n) double shitos_##n(double);
#define D2(n) double shitos_##n(double, double);
D(sqrt)
D(fabs) D(floor) D(ceil) D(trunc) D(round) D(rint) D(exp) D(exp2) D(expm1) D(log) D(log2) D(log10)
    D(log1p) D(sin) D(cos) D(tan) D(asin) D(acos) D(atan) D(sinh) D(cosh) D(tanh) D(cbrt) D2(pow)
        D2(fmod) D2(atan2) D2(hypot) double shitos_ldexp(double, int);
double shitos_frexp(double, int*);
double shitos_modf(double, double*);
/* clang-format on */

static long ulp_diff(double a, double b)
{
    if (isnan(a) && isnan(b))
        return 0;
    if (a == b)
        return 0;
    if (isnan(a) != isnan(b) || isinf(a) != isinf(b))
        return 1L << 40;
    long ia, ib;
    memcpy(&ia, &a, 8);
    memcpy(&ib, &b, 8);
    if (ia < 0)
        ia = 0x8000000000000000L - ia;
    if (ib < 0)
        ib = 0x8000000000000000L - ib;
    long d = ia - ib;
    return d < 0 ? -d : d;
}

static int failures = 0;

static void check1(const char* name, double (*ours)(double), double (*theirs)(double),
    const double* inputs, int count, long tolerance)
{
    long worst = 0;
    double worst_at = 0;
    for (int i = 0; i < count; ++i) {
        long d = ulp_diff(ours(inputs[i]), theirs(inputs[i]));
        if (d > worst) {
            worst = d;
            worst_at = inputs[i];
        }
    }
    int bad = worst > tolerance;
    failures += bad;
    printf("  %-8s max %4ld ulp  %s%s\n", name, worst, bad ? "FAIL at x=" : "ok", bad ? "" : "");
    if (bad)
        printf("           worst input: %.17g -> ours %.17g theirs %.17g\n", worst_at,
            ours(worst_at), theirs(worst_at));
}

static void check2(const char* name, double (*ours)(double, double),
    double (*theirs)(double, double), const double* xs, int nx, const double* ys, int ny,
    long tolerance)
{
    long worst = 0;
    double wx = 0, wy = 0;
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < ny; ++j) {
            long d = ulp_diff(ours(xs[i], ys[j]), theirs(xs[i], ys[j]));
            if (d > worst) {
                worst = d;
                wx = xs[i];
                wy = ys[j];
            }
        }
    int bad = worst > tolerance;
    failures += bad;
    printf("  %-8s max %4ld ulp  %s\n", name, worst, bad ? "FAIL" : "ok");
    if (bad)
        printf("           worst: (%.17g, %.17g) -> ours %.17g theirs %.17g\n", wx, wy,
            ours(wx, wy), theirs(wx, wy));
}

int main(void)
{
    double general[400];
    int n = 0;
    for (double x = -20.0; x <= 20.0; x += 0.137)
        general[n++] = x;
    general[n++] = 0.0;
    general[n++] = -0.0;
    general[n++] = 1.0;
    general[n++] = -1.0;
    general[n++] = 0.5;
    general[n++] = 1e-10;
    general[n++] = 1e10;
    general[n++] = 123456.789;

    double positive[200];
    int p = 0;
    for (double x = 0.001; x < 50.0; x += 0.311)
        positive[p++] = x;
    positive[p++] = 1e-8;
    positive[p++] = 1e8;
    positive[p++] = 1.0;

    double unit[100];
    int u = 0;
    for (double x = -1.0; x <= 1.0; x += 0.0207)
        unit[u++] = x;

    double small[100];
    int s = 0;
    for (double x = -3.0; x <= 3.0; x += 0.061)
        small[s++] = x;

    printf("exact (0 ulp expected):\n");
    check1("sqrt", shitos_sqrt, sqrt, positive, p, 0);
    check1("fabs", shitos_fabs, fabs, general, n, 0);
    check1("floor", shitos_floor, floor, general, n, 0);
    check1("ceil", shitos_ceil, ceil, general, n, 0);
    check1("trunc", shitos_trunc, trunc, general, n, 0);
    check1("round", shitos_round, round, general, n, 0);
    check1("rint", shitos_rint, rint, general, n, 0);
    check2("fmod", shitos_fmod, fmod, general, n, positive, 20, 0);

    printf("transcendental (a few ulp expected):\n");
    check1("exp", shitos_exp, exp, small, s, 4);
    check1("exp2", shitos_exp2, exp2, small, s, 4);
    check1("expm1", shitos_expm1, expm1, small, s, 8);
    check1("log", shitos_log, log, positive, p, 4);
    check1("log2", shitos_log2, log2, positive, p, 4);
    check1("log10", shitos_log10, log10, positive, p, 4);
    check1("log1p", shitos_log1p, log1p, unit, u, 8);
    check1("sin", shitos_sin, sin, small, s, 4);
    check1("cos", shitos_cos, cos, small, s, 4);
    check1("tan", shitos_tan, tan, small, s, 8);
    check1("asin", shitos_asin, asin, unit, u, 4);
    check1("acos", shitos_acos, acos, unit, u, 4);
    check1("atan", shitos_atan, atan, general, n, 4);
    check1("sinh", shitos_sinh, sinh, small, s, 8);
    check1("cosh", shitos_cosh, cosh, small, s, 8);
    check1("tanh", shitos_tanh, tanh, small, s, 8);
    check1("cbrt", shitos_cbrt, cbrt, general, n, 8);
    check2("pow", shitos_pow, pow, positive, 30, small, 20, 16);
    check2("atan2", shitos_atan2, atan2, small, s, small, 20, 4);
    check2("hypot", shitos_hypot, hypot, general, 40, general, 40, 4);

    printf("\ndecomposition:\n");
    int e1, e2;
    double m1, m2;
    int bad = 0;
    for (int i = 0; i < n; ++i) {
        m1 = shitos_frexp(general[i], &e1);
        m2 = frexp(general[i], &e2);
        if (ulp_diff(m1, m2) || e1 != e2) {
            bad++;
        }
    }
    printf("  %-8s %s\n", "frexp", bad ? "FAIL" : "ok");
    failures += (bad != 0);
    bad = 0;
    for (int i = 0; i < n; ++i)
        for (int e = -40; e <= 40; e += 7)
            if (ulp_diff(shitos_ldexp(general[i], e), ldexp(general[i], e)))
                bad++;
    printf("  %-8s %s\n", "ldexp", bad ? "FAIL" : "ok");
    failures += (bad != 0);
    bad = 0;
    for (int i = 0; i < n; ++i) {
        double i1, i2;
        double f1 = shitos_modf(general[i], &i1), f2 = modf(general[i], &i2);
        if (ulp_diff(f1, f2) || ulp_diff(i1, i2))
            bad++;
    }
    printf("  %-8s %s\n", "modf", bad ? "FAIL" : "ok");
    failures += (bad != 0);

    printf("\n%s\n", failures ? "SOME FUNCTIONS ARE OUT OF TOLERANCE" : "all within tolerance");
    return failures != 0;
}
