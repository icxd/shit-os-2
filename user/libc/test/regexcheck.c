/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shit os 2 -- check our regex engine against the host's.
 *
 * A regex engine is exactly the kind of code that looks right and is not: the
 * behaviour that matters is spread over hundreds of small interactions between
 * dialects, anchors, greediness and capture rules, and no reasonable number of
 * hand-written assertions covers it. So the same corpus is run through both
 * ours and glibc's, and every disagreement is reported -- match or not, where
 * it matched, and what each group captured.
 *
 * This is the same trick tools/check-libm.sh plays with the maths functions,
 * for the same reason: the host has a reference implementation and the target
 * does not.
 */

#include <stdio.h>
#include <string.h>

/* Ours, with every symbol prefixed so it can sit next to glibc's. */
#include "shitos-regex.h"

/* glibc's, under its own names. */
#include <regex.h>

static int s_cases;
static int s_failures;

static const char* dialect_name(int cflags)
{
    return (cflags & REG_EXTENDED) ? "ERE" : "BRE";
}

static void report(const char* pattern, const char* subject, int cflags, const char* what,
    const char* ours, const char* theirs)
{
    ++s_failures;
    printf("  FAIL %s /%s/ against \"%s\"\n", dialect_name(cflags), pattern, subject);
    printf("       %s: ours %s, glibc %s\n", what, ours, theirs);
}

#define MAX_GROUPS 10

static void compare(const char* pattern, const char* subject, int cflags, int eflags)
{
    ++s_cases;

    shitos_regex_t mine;
    regex_t theirs;

    /* Our flag values are our own, so translate rather than assuming. */
    int mine_cflags = 0;
    if (cflags & REG_EXTENDED)
        mine_cflags |= SHITOS_REG_EXTENDED;
    if (cflags & REG_ICASE)
        mine_cflags |= SHITOS_REG_ICASE;
    if (cflags & REG_NEWLINE)
        mine_cflags |= SHITOS_REG_NEWLINE;

    int mine_eflags = 0;
    if (eflags & REG_NOTBOL)
        mine_eflags |= SHITOS_REG_NOTBOL;
    if (eflags & REG_NOTEOL)
        mine_eflags |= SHITOS_REG_NOTEOL;

    int const mine_compiled = shitos_regcomp(&mine, pattern, mine_cflags);
    int const their_compiled = regcomp(&theirs, pattern, cflags);

    if ((mine_compiled == 0) != (their_compiled == 0)) {
        char ours[32];
        char them[32];
        snprintf(ours, sizeof(ours), "%s", mine_compiled ? "rejected" : "accepted");
        snprintf(them, sizeof(them), "%s", their_compiled ? "rejected" : "accepted");
        report(pattern, subject, cflags, "compile", ours, them);
        if (!mine_compiled)
            shitos_regfree(&mine);
        if (!their_compiled)
            regfree(&theirs);
        return;
    }

    if (mine_compiled != 0)
        return; /* both rejected it, which is agreement enough */

    if (mine.re_nsub != theirs.re_nsub) {
        char ours[32];
        char them[32];
        snprintf(ours, sizeof(ours), "%zu", mine.re_nsub);
        snprintf(them, sizeof(them), "%zu", theirs.re_nsub);
        report(pattern, subject, cflags, "group count", ours, them);
    }

    shitos_regmatch_t mine_groups[MAX_GROUPS];
    regmatch_t their_groups[MAX_GROUPS];
    for (int i = 0; i < MAX_GROUPS; ++i) {
        mine_groups[i].rm_so = mine_groups[i].rm_eo = -1;
        their_groups[i].rm_so = their_groups[i].rm_eo = -1;
    }

    int const mine_result = shitos_regexec(&mine, subject, MAX_GROUPS, mine_groups, mine_eflags);
    int const their_result = regexec(&theirs, subject, MAX_GROUPS, their_groups, eflags);

    if ((mine_result == 0) != (their_result == 0)) {
        report(pattern, subject, cflags, "match", mine_result == 0 ? "matched" : "no match",
            their_result == 0 ? "matched" : "no match");
        shitos_regfree(&mine);
        regfree(&theirs);
        return;
    }

    if (mine_result == 0) {
        /* Only the groups the pattern actually has are meaningful. */
        size_t const groups = theirs.re_nsub < MAX_GROUPS - 1 ? theirs.re_nsub : MAX_GROUPS - 1;
        for (size_t i = 0; i <= groups; ++i) {
            if (mine_groups[i].rm_so == their_groups[i].rm_so
                && mine_groups[i].rm_eo == their_groups[i].rm_eo)
                continue;

            char ours[64];
            char them[64];
            char what[32];
            snprintf(what, sizeof(what), i == 0 ? "extent" : "group %zu", i);
            snprintf(ours, sizeof(ours), "[%d,%d)", (int)mine_groups[i].rm_so,
                (int)mine_groups[i].rm_eo);
            snprintf(them, sizeof(them), "[%d,%d)", (int)their_groups[i].rm_so,
                (int)their_groups[i].rm_eo);
            report(pattern, subject, cflags, what, ours, them);
            break;
        }
    }

    shitos_regfree(&mine);
    regfree(&theirs);
}

/* Runs a pattern against a set of subjects in whichever dialects make sense. */
static void sweep(const char* pattern, const char* const* subjects, size_t count, int dialects)
{
    for (size_t i = 0; i < count; ++i) {
        if (dialects & 1)
            compare(pattern, subjects[i], 0, 0);
        if (dialects & 2)
            compare(pattern, subjects[i], REG_EXTENDED, 0);
        if (dialects & 2)
            compare(pattern, subjects[i], REG_EXTENDED | REG_ICASE, 0);
    }
}

#define BRE 1
#define ERE 2
#define BOTH 3

int main(void)
{
    static const char* const SUBJECTS[] = {
        "",
        "a",
        "ab",
        "abc",
        "aaa",
        "abab",
        "hello world",
        "Hello World",
        "foo.bar",
        "192.168.1.1",
        "  leading",
        "trailing  ",
        "one\ntwo",
        "a1b2c3",
        "[bracketed]",
        "under_score",
        "aaaaaaaaab",
        "xyzzy",
        "The quick brown fox",
        "-42",
        "aa",
        "abcabcabc",
        "AbCdEf",
        "  ",
        "a.b.c",
        "3.14159",
        "key=value",
        "\ttabbed",
        "MiXeD case HERE",
        "aaaaaaaaaaaaaaaaaaaa",
    };
    static size_t const SUBJECT_COUNT = sizeof(SUBJECTS) / sizeof(SUBJECTS[0]);

    /* Patterns valid in both dialects. */
    static const char* const SHARED[] = {
        "a",
        "abc",
        ".",
        "..",
        ".*",
        "a*",
        "a*b",
        "^a",
        "a$",
        "^a$",
        "^",
        "$",
        "^$",
        "[abc]",
        "[^abc]",
        "[a-z]",
        "[a-z]*",
        "[^a-z]",
        "[[:digit:]]",
        "[[:alpha:]][[:digit:]]",
        "[[:space:]]",
        "[]a]",
        "[a-]",
        "[-a]",
        "x[0-9]*y",
        "\\.",
        "\\*",
        "a\\{2\\}",
        "[0-9][0-9]*",
        "\\<the\\>",
        "\\bfox\\b",
        "\\w*",
        "\\s",
        "o\\{1,2\\}",
    };

    /* ERE only: bare braces, plus, question mark, alternation, groups. */
    static const char* const EXTENDED_ONLY[] = {
        "a+",
        "a?",
        "a|b",
        "ab|cd",
        "(a)",
        "(a)(b)",
        "(a|b)c",
        "(ab)+",
        "(a*)*",
        "a{2}",
        "a{2,}",
        "a{1,3}",
        "(a|ab)",
        "(a|ab)(c|bcd)",
        "^(.*)$",
        "([0-9]+)\\.([0-9]+)",
        "(foo|bar)+",
        "[[:alpha:]]+",
        "(.)(.)(.)",
        "x?y?z?",
        "(a?)*b",
        "((a)(b))c",
        "^(a|b)*$",
        "[0-9]{1,3}(\\.[0-9]{1,3}){3}",
        "\\w+",
        "(\\w+) (\\w+)",
        "((a|b)*)c",
        "(a(b(c)))",
        "(a)|(b)",
        "(|a)",
        "(a|)",
        "()",
        "()*",
        "(a){2,3}",
        "([a-c]){2}",
        "(ab|a)(b?)",
        "^(a+)(a*)$",
        "(.*)(.*)",
        "([[:upper:]][[:lower:]]*)+",
        "[-+]?[0-9]+",
        "(foo)?bar",
        "a(b|c|d)e",
        "((((a))))",
        "[^[:space:]]+",
        "(a|ab|abc)(c|bc|)",
        "x{0}",
        "x{0,}",
        "(a{2}){2}",
    };

    /* BRE only: escaped grouping, backreferences, a literal star up front. */
    static const char* const BASIC_ONLY[] = {
        "\\(a\\)",
        "\\(a\\)\\(b\\)",
        "\\(ab\\)*",
        "*a",
        "a\\{1,3\\}",
        "\\(a*\\)b",
        "\\(.\\)\\1",
        "\\(a\\)\\1",
        "^\\(.*\\)$",
        "\\(foo\\|bar\\)",
        "\\(a\\)\\(b\\)\\2\\1",
        "\\(.*\\)\\1",
        "\\(a\\|b\\)*",
        "\\(ab\\)\\{2\\}",
        "a\\{0,\\}",
        "\\(\\)",
        "\\([a-z]\\)\\1*",
    };

    printf("comparing against glibc over %zu subjects\n", SUBJECT_COUNT);

    for (size_t i = 0; i < sizeof(SHARED) / sizeof(SHARED[0]); ++i)
        sweep(SHARED[i], SUBJECTS, SUBJECT_COUNT, BOTH);
    for (size_t i = 0; i < sizeof(EXTENDED_ONLY) / sizeof(EXTENDED_ONLY[0]); ++i)
        sweep(EXTENDED_ONLY[i], SUBJECTS, SUBJECT_COUNT, ERE);
    for (size_t i = 0; i < sizeof(BASIC_ONLY) / sizeof(BASIC_ONLY[0]); ++i)
        sweep(BASIC_ONLY[i], SUBJECTS, SUBJECT_COUNT, BRE);

    /* The anchoring flags, which only mean anything with a pattern that
     * anchors and a subject that could match either way. */
    compare("^a", "a", 0, REG_NOTBOL);
    compare("a$", "a", 0, REG_NOTEOL);
    compare("^a", "a", REG_EXTENDED, REG_NOTBOL);
    compare("a$", "a", REG_EXTENDED, REG_NOTEOL);
    s_cases += 4;

    /* REG_NEWLINE changes what . and a negated set match, and what ^ and $
     * anchor to. */
    static const char* const MULTILINE[] = { "one\ntwo", "a\nb\nc", "\n", "x\n" };
    for (size_t i = 0; i < 4; ++i) {
        compare("^two$", MULTILINE[i], REG_EXTENDED | REG_NEWLINE, 0);
        compare(".*", MULTILINE[i], REG_EXTENDED | REG_NEWLINE, 0);
        compare("[^x]*", MULTILINE[i], REG_EXTENDED | REG_NEWLINE, 0);
        compare("^.", MULTILINE[i], REG_EXTENDED | REG_NEWLINE, 0);
        s_cases += 4;
    }

    /* Patterns that must be rejected, and rejected by both. */
    static const char* const INVALID_ERE[] = { "(", ")", "[", "a{2,1}", "*", "+", "?", "a**+" };
    for (size_t i = 0; i < sizeof(INVALID_ERE) / sizeof(INVALID_ERE[0]); ++i)
        compare(INVALID_ERE[i], "aaa", REG_EXTENDED, 0);

    printf("%d cases, %d disagreements\n", s_cases, s_failures);
    return s_failures == 0 ? 0 : 1;
}
