/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- floating point maths.
 *
 * Accuracy: the transcendentals are computed with the x87 unit's hardware
 * instructions rather than software polynomials. That is a deliberate trade --
 * far less code, and accurate to within an ulp or two over the useful range --
 * but it is not a correctly-rounded libm. Two limits worth knowing:
 *
 *   - sin, cos and tan reduce their argument modulo 2*pi using an internal
 *     66-bit constant, so accuracy degrades for very large arguments and is
 *     meaningless past 2^63.
 *   - Results are computed at x87's 80-bit precision and rounded once on the
 *     way out, so they can differ in the last place from a libm that works
 *     purely in double.
 *
 * See docs/roadmap.md if that ever stops being good enough.
 */

#ifndef _MATH_H
#define _MATH_H

#include <sys/types.h>

#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")

#define M_E 2.7182818284590452354
#define M_LOG2E 1.4426950408889634074
#define M_LOG10E 0.43429448190325182765
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_1_PI 0.31830988618379067154
#define M_2_PI 0.63661977236758134308
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4

/* The compiler knows these without a call, and gets them right for every
 * type, so there is no reason to route them through the library. */
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x) __builtin_signbit(x)
#define isgreater(x, y) __builtin_isgreater(x, y)
#define isless(x, y) __builtin_isless(x, y)
#define isunordered(x, y) __builtin_isunordered(x, y)

double fabs(double x);
double copysign(double x, double y);
double sqrt(double x);
double fmod(double x, double y);
double fmin(double x, double y);
double fmax(double x, double y);
double hypot(double x, double y);
double nan(const char* tag);

double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double rint(double x);
double nearbyint(double x);

double frexp(double x, int* exponent);
double ldexp(double x, int exponent);
double scalbn(double x, int exponent);
double modf(double x, double* integral_part);

double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double pow(double base, double exponent);
double cbrt(double x);

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

double sinh(double x);
double cosh(double x);
double tanh(double x);

/* float variants, as thin wrappers. Lua only needs these if built with
 * LUA_FLOAT_TYPE set to float, but a C program may reference them. */
float fabsf(float x);
float sqrtf(float x);
float floorf(float x);
float ceilf(float x);
float fmodf(float x, float y);
float powf(float x, float y);
float expf(float x);
float logf(float x);

#endif /* _MATH_H */
