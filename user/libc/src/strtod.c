/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- decimal and hexadecimal string to double.
 *
 * Not correctly rounded in every case, but exact wherever it can cheaply be:
 * powers of ten up to 10^22 are representable in a double, so a mantissa of at
 * most 19 digits scaled by one of those is a single correctly-rounded multiply.
 * Outside that window the scaling is done in steps and can be off by an ulp or
 * two, which is the usual trade for a strtod that is not a research project.
 *
 * Hex float syntax (0x1.8p3) is supported because Lua's lexer accepts it.
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>

/* Exactly representable: 10^0 through 10^22. */
static const double POWERS_OF_TEN[23] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10,
    1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };

static double scale_by_power_of_ten(double value, int exponent)
{
    if (exponent == 0)
        return value;

    if (exponent > 0) {
        while (exponent > 22) {
            value *= 1e22;
            exponent -= 22;
            if (value == HUGE_VAL)
                return value;
        }
        return value * POWERS_OF_TEN[exponent];
    }

    exponent = -exponent;
    while (exponent > 22) {
        value /= 1e22;
        exponent -= 22;
        if (value == 0.0)
            return value;
    }
    /* Dividing rather than multiplying by the reciprocal: the reciprocal of a
     * power of ten is not exact, and multiplying by it would add a rounding
     * step this way avoids. */
    return value / POWERS_OF_TEN[exponent];
}

static double parse_hex(const char* s, char** end, int negative)
{
    double value = 0.0;
    int any_digits = 0;
    int exponent = 0;

    for (; isxdigit((unsigned char)*s); ++s) {
        int const digit = isdigit((unsigned char)*s) ? *s - '0' : (tolower(*s) - 'a' + 10);
        value = value * 16.0 + digit;
        any_digits = 1;
    }

    if (*s == '.') {
        ++s;
        for (; isxdigit((unsigned char)*s); ++s) {
            int const digit = isdigit((unsigned char)*s) ? *s - '0' : (tolower(*s) - 'a' + 10);
            value = value * 16.0 + digit;
            exponent -= 4; /* each hex digit after the point is a factor of 16 */
            any_digits = 1;
        }
    }

    if (!any_digits) {
        if (end)
            *end = 0;
        return 0.0;
    }

    if (*s == 'p' || *s == 'P') {
        const char* const before = s;
        ++s;
        int sign = 1;
        if (*s == '+')
            ++s;
        else if (*s == '-') {
            sign = -1;
            ++s;
        }
        if (isdigit((unsigned char)*s)) {
            int binary_exponent = 0;
            for (; isdigit((unsigned char)*s); ++s)
                binary_exponent = binary_exponent * 10 + (*s - '0');
            exponent += sign * binary_exponent;
        } else {
            /* A 'p' with no digits is not part of the number. */
            s = before;
        }
    }

    if (end)
        *end = (char*)s;

    value = ldexp(value, exponent);
    return negative ? -value : value;
}

double strtod(const char* s, char** end)
{
    const char* const start = s;

    while (isspace((unsigned char)*s))
        ++s;

    int negative = 0;
    if (*s == '+')
        ++s;
    else if (*s == '-') {
        negative = 1;
        ++s;
    }

    /* Infinities and NaNs, which the standard requires to be accepted. */
    if (tolower(s[0]) == 'i' && tolower(s[1]) == 'n' && tolower(s[2]) == 'f') {
        s += 3;
        if (tolower(s[0]) == 'i' && tolower(s[1]) == 'n' && tolower(s[2]) == 'i'
            && tolower(s[3]) == 't' && tolower(s[4]) == 'y')
            s += 5;
        if (end)
            *end = (char*)s;
        return negative ? -HUGE_VAL : HUGE_VAL;
    }
    if (tolower(s[0]) == 'n' && tolower(s[1]) == 'a' && tolower(s[2]) == 'n') {
        s += 3;
        if (end)
            *end = (char*)s;
        return negative ? -nan("") : nan("");
    }

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char* hex_end = 0;
        double const value = parse_hex(s + 2, &hex_end, negative);
        if (hex_end == 0) {
            /* "0x" with no digits after it: the number was just the 0. */
            if (end)
                *end = (char*)(s + 1);
            return 0.0;
        }
        if (end)
            *end = hex_end;
        return value;
    }

    /*
     * Accumulate significant digits into an integer rather than into a double.
     * A double loses precision after the sixteenth digit, so building the
     * mantissa this way and scaling once at the end is both faster and more
     * accurate than multiplying as we go.
     */
    unsigned long long mantissa = 0;
    int significant_digits = 0;
    int exponent = 0;
    int any_digits = 0;

    for (; isdigit((unsigned char)*s); ++s) {
        any_digits = 1;
        if (significant_digits < 19) {
            mantissa = mantissa * 10 + (unsigned)(*s - '0');
            if (mantissa != 0)
                ++significant_digits;
        } else {
            /* Past the point where more digits can change the value; keep
             * track of the magnitude and drop them. */
            ++exponent;
        }
    }

    if (*s == '.') {
        ++s;
        for (; isdigit((unsigned char)*s); ++s) {
            any_digits = 1;
            if (significant_digits < 19) {
                mantissa = mantissa * 10 + (unsigned)(*s - '0');
                if (mantissa != 0)
                    ++significant_digits;
                --exponent;
            }
        }
    }

    if (!any_digits) {
        /* Nothing was consumed, so the caller should see the original string. */
        if (end)
            *end = (char*)start;
        return 0.0;
    }

    if (*s == 'e' || *s == 'E') {
        const char* const before = s;
        ++s;
        int sign = 1;
        if (*s == '+')
            ++s;
        else if (*s == '-') {
            sign = -1;
            ++s;
        }
        if (isdigit((unsigned char)*s)) {
            int decimal_exponent = 0;
            for (; isdigit((unsigned char)*s); ++s) {
                if (decimal_exponent < 100000)
                    decimal_exponent = decimal_exponent * 10 + (*s - '0');
            }
            exponent += sign * decimal_exponent;
        } else {
            s = before;
        }
    }

    if (end)
        *end = (char*)s;

    double value = scale_by_power_of_ten((double)mantissa, exponent);

    if (value == HUGE_VAL || value == -HUGE_VAL)
        errno = ERANGE;

    return negative ? -value : value;
}

float strtof(const char* s, char** end)
{
    return (float)strtod(s, end);
}
double atof(const char* s)
{
    return strtod(s, 0);
}
