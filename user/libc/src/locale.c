/* SPDX-License-Identifier: MIT */
/* shit os 2 libc -- locales. */

#include <locale.h>
#include <string.h>

static struct lconv s_c_locale = {
    .decimal_point = (char*)".",
    .thousands_sep = (char*)"",
    .grouping = (char*)"",
    .int_curr_symbol = (char*)"",
    .currency_symbol = (char*)"",
    .mon_decimal_point = (char*)"",
    .mon_thousands_sep = (char*)"",
    .mon_grouping = (char*)"",
    .positive_sign = (char*)"",
    .negative_sign = (char*)"",
    /* CHAR_MAX means "not available in this locale", which is the correct
     * answer for all of these in the C locale. */
    .int_frac_digits = 127,
    .frac_digits = 127,
    .p_cs_precedes = 127,
    .p_sep_by_space = 127,
    .n_cs_precedes = 127,
    .n_sep_by_space = 127,
    .p_sign_posn = 127,
    .n_sign_posn = 127,
    .int_p_cs_precedes = 127,
    .int_p_sep_by_space = 127,
    .int_n_cs_precedes = 127,
    .int_n_sep_by_space = 127,
    .int_p_sign_posn = 127,
    .int_n_sign_posn = 127,
};

char* setlocale(int category, const char* locale)
{
    (void)category;

    /* Querying, or asking for the only locale there is. */
    if (locale == 0 || locale[0] == '\0' || strcmp(locale, "C") == 0
        || strcmp(locale, "POSIX") == 0)
        return (char*)"C";

    /* Anything else genuinely is not available; saying so is more useful than
     * accepting it and behaving as C regardless. */
    return 0;
}

struct lconv* localeconv(void)
{
    return &s_c_locale;
}
