/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- floating point maths.
 *
 * Three implementation strategies, picked per function:
 *
 *   - Bit manipulation where the answer is exact and obvious from the
 *     representation: sign, rounding, and exponent surgery.
 *   - A single SSE instruction where the hardware has one: sqrt.
 *   - The x87 unit for the transcendentals. It has genuine hardware log,
 *     exp, sine and arctangent, which is a great deal less code than
 *     polynomial approximations and accurate enough for anything this
 *     system will run. See the accuracy note in <math.h>.
 *
 * The x87 sequences all leave the stack balanced. An unbalanced one does not
 * fail immediately -- it fails eight calls later when the stack overflows --
 * so each is written to consume exactly what it pushes.
 */

#include <math.h>

typedef union {
    double value;
    unsigned long bits;
} DoubleBits;

#define SIGN_BIT (1UL << 63)
#define EXPONENT_MASK 0x7FF
#define MANTISSA_BITS 52
#define EXPONENT_BIAS 1023

/* --- sign and classification ------------------------------------------- */

double fabs(double x)
{
    DoubleBits v = { x };
    v.bits &= ~SIGN_BIT;
    return v.value;
}

double copysign(double x, double y)
{
    DoubleBits a = { x };
    DoubleBits b = { y };
    a.bits = (a.bits & ~SIGN_BIT) | (b.bits & SIGN_BIT);
    return a.value;
}

double nan(const char* tag)
{
    (void)tag;
    return __builtin_nan("");
}

/* --- square root -------------------------------------------------------- */

double sqrt(double x)
{
    /* One instruction, correctly rounded by the hardware. */
    double result;
    __asm__("sqrtsd %1, %0" : "=x"(result) : "x"(x));
    return result;
}

/* --- rounding ----------------------------------------------------------- */

double trunc(double x)
{
    DoubleBits v = { x };
    int const exponent = (int)((v.bits >> MANTISSA_BITS) & EXPONENT_MASK) - EXPONENT_BIAS;

    /* Already integral, or infinite, or NaN: nothing to remove. */
    if (exponent >= MANTISSA_BITS)
        return x;
    /* |x| < 1 truncates to zero, keeping the sign. */
    if (exponent < 0) {
        v.bits &= SIGN_BIT;
        return v.value;
    }

    v.bits &= ~((1UL << (MANTISSA_BITS - exponent)) - 1);
    return v.value;
}

double floor(double x)
{
    double const truncated = trunc(x);
    /* trunc rounds toward zero, so a negative number with a fraction has been
     * rounded the wrong way. */
    if (x < 0.0 && truncated != x)
        return truncated - 1.0;
    return truncated;
}

double ceil(double x)
{
    double const truncated = trunc(x);
    if (x > 0.0 && truncated != x)
        return truncated + 1.0;
    return truncated;
}

double round(double x)
{
    /* Halfway cases go away from zero, which is what round(3) specifies and
     * what rint would not do. */
    double const shifted = fabs(x) + 0.5;
    double result = trunc(shifted);
    /* trunc(0.5 + 0.5) is 1, but adding 0.5 to a large value can round up in
     * the addition itself; guard the one case where that matters. */
    if (result > shifted)
        result -= 1.0;
    return copysign(result, x);
}

double rint(double x)
{
    /* Round to the current mode, which is round-to-nearest-even by default. */
    double result;
    __asm__("frndint" : "=t"(result) : "0"(x));
    return result;
}

double nearbyint(double x)
{
    return rint(x);
}

/* --- exponent surgery ---------------------------------------------------- */

double frexp(double x, int* exponent)
{
    DoubleBits v = { x };
    int raw = (int)((v.bits >> MANTISSA_BITS) & EXPONENT_MASK);

    /* Zero, infinity and NaN are returned unchanged with a zero exponent. */
    if (raw == EXPONENT_MASK || x == 0.0) {
        *exponent = 0;
        return x;
    }

    if (raw == 0) {
        /* Subnormal: scale it into the normal range and account for that. */
        v.value = x * 18014398509481984.0; /* 2^54 */
        raw = (int)((v.bits >> MANTISSA_BITS) & EXPONENT_MASK) - 54;
    }

    *exponent = raw - (EXPONENT_BIAS - 1);
    /* Force the exponent to -1 so the result lands in [0.5, 1). */
    v.bits = (v.bits & ~((unsigned long)EXPONENT_MASK << MANTISSA_BITS))
        | ((unsigned long)(EXPONENT_BIAS - 1) << MANTISSA_BITS);
    return v.value;
}

double scalbn(double x, int exponent)
{
    /* Multiplying by 2^n in up to three steps keeps every intermediate
     * representable, so a big exponent does not overflow on the way to a
     * result that would have been fine. */
    if (exponent > 1023) {
        x *= 8.98846567431158e307; /* 2^1023 */
        exponent -= 1023;
        if (exponent > 1023) {
            x *= 8.98846567431158e307;
            exponent -= 1023;
            if (exponent > 1023)
                exponent = 1023;
        }
    } else if (exponent < -1022) {
        x *= 2.2250738585072014e-308 * 9007199254740992.0; /* 2^-1022 * 2^53 */
        exponent += 1022 - 53;
        if (exponent < -1022) {
            x *= 2.2250738585072014e-308 * 9007199254740992.0;
            exponent += 1022 - 53;
            if (exponent < -1022)
                exponent = -1022;
        }
    }

    DoubleBits scale;
    scale.bits = (unsigned long)(EXPONENT_BIAS + exponent) << MANTISSA_BITS;
    return x * scale.value;
}

double ldexp(double x, int exponent)
{
    return scalbn(x, exponent);
}

double modf(double x, double* integral_part)
{
    double const integral = trunc(x);
    *integral_part = integral;

    /* An infinite input has no fractional part, and subtracting would give a
     * NaN rather than the signed zero the standard asks for. */
    if (isinf(x))
        return copysign(0.0, x);

    return copysign(x - integral, x);
}

/* --- remainder ----------------------------------------------------------- */

double fmod(double x, double y)
{
    /*
     * fprem reduces by at most a factor of 2^63 per execution and sets C2 when
     * there is more to do, so it is driven in a loop -- inside the asm, so the
     * divisor stays on the x87 stack across iterations instead of being
     * reloaded. The trailing pop discards it: like fscale, fprem does not
     * consume st(1) even though the "u" constraint says the asm will.
     */
    double result;
    unsigned short status;
    __asm__("1:\n\t"
            "fprem\n\t"
            "fnstsw %%ax\n\t"
            "testb $0x04, %%ah\n\t" /* C2: reduction incomplete */
            "jnz 1b\n\t"
            "fstp %%st(1)"
            : "=t"(result), "=a"(status)
            : "0"(x), "u"(y));

    return result;
}

double hypot(double x, double y)
{
    /* Scaling by the larger operand keeps the squares from overflowing when
     * the answer itself is perfectly representable. */
    x = fabs(x);
    y = fabs(y);
    if (x < y) {
        double const swap = x;
        x = y;
        y = swap;
    }
    if (x == 0.0)
        return 0.0;
    double const ratio = y / x;
    return x * sqrt(1.0 + ratio * ratio);
}

double fmin(double x, double y)
{
    if (isnan(x))
        return y;
    if (isnan(y))
        return x;
    return x < y ? x : y;
}

double fmax(double x, double y)
{
    if (isnan(x))
        return y;
    if (isnan(y))
        return x;
    return x > y ? x : y;
}

/* --- exponentials and logarithms ----------------------------------------- */

/*
 * f2xm1 computes 2^f - 1 but only for |f| <= 1, so the exponent is split into
 * an integer part handled by fscale and a fraction handled by f2xm1.
 */
double exp2(double x)
{
    if (isnan(x))
        return x;
    if (x > 1024.0)
        return HUGE_VAL;
    if (x < -1080.0)
        return 0.0;

    double integral;
    __asm__("frndint" : "=t"(integral) : "0"(x));

    double fraction = x - integral;
    __asm__("f2xm1" : "=t"(fraction) : "0"(fraction));
    fraction += 1.0;

    /*
     * fscale multiplies by 2^st(1) but does not pop st(1), while the "u"
     * constraint promises the asm consumed it. Without the explicit pop each
     * call leaks a stack slot and the eighth one returns NaN -- silently, and
     * nowhere near the call that caused it.
     */
    double result;
    __asm__("fscale\n\t"
            "fstp %%st(1)"
            : "=t"(result)
            : "0"(fraction), "u"(integral));
    return result;
}

double exp(double x)
{
    return exp2(x * M_LOG2E);
}

double expm1(double x)
{
    /* Going through exp2 would lose every significant digit for small x, which
     * is the entire reason expm1 exists. f2xm1 computes it directly. */
    double const scaled = x * M_LOG2E;
    if (fabs(scaled) < 1.0) {
        double result;
        __asm__("f2xm1" : "=t"(result) : "0"(scaled));
        return result;
    }
    return exp(x) - 1.0;
}

/* fyl2x computes y * log2(x), which is every logarithm we need with the right
 * constant for y. */
static double log_base(double x, double multiplier)
{
    double result;
    /* fyl2x pops st(1) itself, so the stack is balanced already. */
    __asm__("fyl2x" : "=t"(result) : "0"(x), "u"(multiplier));
    return result;
}

double log(double x)
{
    return log_base(x, M_LN2);
}
double log2(double x)
{
    return log_base(x, 1.0);
}
double log10(double x)
{
    return log_base(x, 0.30102999566398119521); /* log10(2) */
}

double log1p(double x)
{
    /* fyl2xp1 takes the argument already offset by one, so no precision is
     * lost forming 1 + x when x is tiny. Its input range is limited. */
    if (fabs(x) < 0.29) {
        double result;
        __asm__("fyl2xp1" : "=t"(result) : "0"(x), "u"(M_LN2));
        return result;
    }
    return log(1.0 + x);
}

double pow(double base, double exponent)
{
    /* The special cases come first because 0^0, 1^inf and friends are defined
     * by the standard to be values that the general path would produce NaN for. */
    if (exponent == 0.0)
        return 1.0;
    if (isnan(base) || isnan(exponent))
        return base == 1.0 ? 1.0 : __builtin_nan("");
    if (base == 1.0)
        return 1.0;

    if (base == 0.0) {
        if (exponent < 0.0)
            return HUGE_VAL;
        /* A negative zero raised to an odd integer keeps its sign. */
        double half;
        int const is_odd_integer
            = (modf(exponent / 2.0, &half) != 0.0) && trunc(exponent) == exponent;
        return is_odd_integer ? base : 0.0;
    }

    if (base < 0.0) {
        /* Only an integer exponent is defined here; anything else is a domain
         * error rather than a value. */
        if (trunc(exponent) != exponent)
            return __builtin_nan("");
        double half;
        int const is_odd = modf(exponent / 2.0, &half) != 0.0;
        double const magnitude = exp2(exponent * log2(-base));
        return is_odd ? -magnitude : magnitude;
    }

    return exp2(exponent * log2(base));
}

double cbrt(double x)
{
    if (x == 0.0 || isnan(x) || isinf(x))
        return x;
    /* Sign is handled outside so the logarithm only ever sees a positive
     * argument. */
    double const magnitude = exp2(log2(fabs(x)) / 3.0);
    return copysign(magnitude, x);
}

/* --- trigonometry --------------------------------------------------------- */

double sin(double x)
{
    if (isinf(x) || isnan(x))
        return __builtin_nan("");
    double result;
    __asm__("fsin" : "=t"(result) : "0"(x));
    return result;
}

double cos(double x)
{
    if (isinf(x) || isnan(x))
        return __builtin_nan("");
    double result;
    __asm__("fcos" : "=t"(result) : "0"(x));
    return result;
}

double tan(double x)
{
    if (isinf(x) || isnan(x))
        return __builtin_nan("");
    double result;
    double discard;
    /* fptan pushes the tangent and then a literal 1.0, so both come off. */
    __asm__("fptan" : "=t"(discard), "=u"(result) : "0"(x));
    return result;
}

double atan2(double y, double x)
{
    double result;
    /* fpatan pops st(1) itself. */
    __asm__("fpatan" : "=t"(result) : "0"(x), "u"(y));
    return result;
}

double atan(double x)
{
    return atan2(x, 1.0);
}

double asin(double x)
{
    if (fabs(x) > 1.0)
        return __builtin_nan("");
    /* asin(x) = atan2(x, sqrt(1 - x^2)), which is exact at the endpoints. */
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x)
{
    if (fabs(x) > 1.0)
        return __builtin_nan("");
    return atan2(sqrt(1.0 - x * x), x);
}

/* --- hyperbolics ---------------------------------------------------------- */

double sinh(double x)
{
    /* expm1 rather than exp keeps the small-x case from cancelling to zero. */
    double const magnitude = fabs(x);
    if (magnitude < 1.0) {
        double const e = expm1(magnitude);
        double const result = 0.5 * (e + e / (e + 1.0));
        return copysign(result, x);
    }
    double const e = exp(magnitude);
    return copysign(0.5 * (e - 1.0 / e), x);
}

double cosh(double x)
{
    double const e = exp(fabs(x));
    return 0.5 * (e + 1.0 / e);
}

double tanh(double x)
{
    double const magnitude = fabs(x);
    /* Past this the result is 1 to within a double's precision, and the
     * exponential would overflow on the way to saying so. */
    if (magnitude > 20.0)
        return copysign(1.0, x);
    double const e = expm1(-2.0 * magnitude);
    return copysign(-e / (e + 2.0), x);
}

/* --- float variants -------------------------------------------------------- */

float fabsf(float x)
{
    return (float)fabs(x);
}
float sqrtf(float x)
{
    return (float)sqrt(x);
}
float floorf(float x)
{
    return (float)floor(x);
}
float ceilf(float x)
{
    return (float)ceil(x);
}
float fmodf(float x, float y)
{
    return (float)fmod(x, y);
}
float powf(float x, float y)
{
    return (float)pow(x, y);
}
float expf(float x)
{
    return (float)exp(x);
}
float logf(float x)
{
    return (float)log(x);
}
