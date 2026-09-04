/* SPDX-License-Identifier: MIT */
/*
 * shit os 2 libc -- POSIX regular expressions.
 *
 * A recursive-descent parser producing a tree, and a backtracking matcher over
 * it with an explicit continuation. Two things about that are worth stating
 * before reading any of it.
 *
 * POSIX wants the *longest* match at the leftmost position where any match
 * starts. A backtracking matcher naturally gives the first match it stumbles
 * into, which for `a|ab` against "ab" is "a" -- Perl's answer, not POSIX's. So
 * the matcher does not stop at the first success: the final continuation
 * records the end offset and reports failure, which drives the search on
 * through every remaining alternative, and the longest end wins. Captures are
 * taken from whichever run produced it.
 *
 * Where this stops short: POSIX also specifies how to disambiguate *sub*
 * expression captures when several ways of matching give the same overall
 * length, and this takes the first such run rather than working through the
 * rule. Patterns where that is observable are rare and contrived; the host
 * check in tools/check-regex.sh compares against glibc over a corpus, and
 * anything it finds is either fixed or written down.
 *
 * Backtracking is exponential on adversarial patterns -- the usual (a*)* shape.
 * A repetition whose body matched the empty string stops iterating, which
 * removes the infinite-loop case; the rest is a known property of the
 * algorithm and the reason a production libc uses a DFA.
 */

#include "internal.h"

#include <ctype.h>
#include <limits.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

/* --- the compiled form ---------------------------------------------------
 *
 * alternation := branch ('|' branch)*
 * branch      := piece*
 * piece       := atom repetition?
 * atom        := literal | any | set | group | backreference | anchor
 */

enum AtomType {
    ATOM_LITERAL,
    ATOM_ANY, /* . */
    ATOM_SET, /* [...] and the \w \s shorthands */
    ATOM_GROUP,
    ATOM_BACKREF,
    ATOM_BOL, /* ^ */
    ATOM_EOL, /* $ */
    ATOM_WORD_BOUNDARY, /* \b */
    ATOM_NOT_WORD_BOUNDARY, /* \B */
    ATOM_WORD_START, /* \< */
    ATOM_WORD_END, /* \> */
};

#define SET_WORDS (256 / 32)

typedef struct Alternation Alternation;

typedef struct {
    unsigned char type;
    union {
        unsigned char literal;
        unsigned set[SET_WORDS];
        Alternation* group;
        int backref;
    } u;
    int group_index; /* ATOM_GROUP: 1-based capture number */
} Atom;

typedef struct {
    Atom atom;
    int minimum;
    int maximum; /* -1 for unbounded */
} Piece;

typedef struct {
    Piece* pieces;
    size_t count;
    size_t capacity;
} Branch;

struct Alternation {
    Branch* branches;
    size_t count;
    size_t capacity;
};

typedef struct {
    Alternation* root;
    size_t group_count;
    int cflags;
} Program;

/* --- character sets ------------------------------------------------------ */

static void set_clear(unsigned* set)
{
    for (int i = 0; i < SET_WORDS; ++i)
        set[i] = 0;
}

static void set_add(unsigned* set, unsigned char c)
{
    set[c / 32] |= 1u << (c % 32);
}

static int set_has(const unsigned* set, unsigned char c)
{
    return (set[c / 32] & (1u << (c % 32))) != 0;
}

static void set_invert(unsigned* set)
{
    for (int i = 0; i < SET_WORDS; ++i)
        set[i] = ~set[i];
}

static int is_word_character(int c)
{
    return isalnum(c) || c == '_';
}

/* --- the parser ---------------------------------------------------------- */

typedef struct {
    const char* pattern;
    size_t position;
    size_t length;
    int cflags;
    size_t group_count;
    int depth; /* how many groups deep, so a stray ')' can be told apart */
    int error;
} Parser;

static Alternation* parse_alternation(Parser* parser);
static void free_alternation(Alternation* alternation);

static int at_end(const Parser* parser)
{
    return parser->position >= parser->length;
}

static int peek(const Parser* parser)
{
    return at_end(parser) ? -1 : (unsigned char)parser->pattern[parser->position];
}

static int peek_at(const Parser* parser, size_t ahead)
{
    size_t const index = parser->position + ahead;
    return index >= parser->length ? -1 : (unsigned char)parser->pattern[index];
}

static int extended(const Parser* parser)
{
    return (parser->cflags & REG_EXTENDED) != 0;
}

/*
 * In BRE the metacharacters for grouping, alternation and the optional
 * repetitions are spelled with a backslash, and in ERE they are bare. Asking
 * this question in one place keeps the rest of the parser from branching on
 * the dialect at every step.
 */
static int consume_meta(Parser* parser, char c)
{
    if (extended(parser)) {
        if (peek(parser) == c) {
            ++parser->position;
            return 1;
        }
        return 0;
    }
    if (peek(parser) == '\\' && peek_at(parser, 1) == c) {
        parser->position += 2;
        return 1;
    }
    return 0;
}

static int at_meta(const Parser* parser, char c)
{
    if (extended(parser))
        return peek(parser) == c;
    return peek(parser) == '\\' && peek_at(parser, 1) == c;
}

static int branch_append(Branch* branch, const Piece* piece)
{
    if (branch->count == branch->capacity) {
        size_t const capacity = branch->capacity ? branch->capacity * 2 : 8;
        Piece* const grown = realloc(branch->pieces, capacity * sizeof(Piece));
        if (!grown)
            return 0;
        branch->pieces = grown;
        branch->capacity = capacity;
    }
    branch->pieces[branch->count++] = *piece;
    return 1;
}

static int alternation_append(Alternation* alternation, const Branch* branch)
{
    if (alternation->count == alternation->capacity) {
        size_t const capacity = alternation->capacity ? alternation->capacity * 2 : 4;
        Branch* const grown = realloc(alternation->branches, capacity * sizeof(Branch));
        if (!grown)
            return 0;
        alternation->branches = grown;
        alternation->capacity = capacity;
    }
    alternation->branches[alternation->count++] = *branch;
    return 1;
}

/* Parses the inside of a [...] expression, with `parser` just past the '['. */
static int parse_bracket(Parser* parser, unsigned* set)
{
    set_clear(set);

    int negate = 0;
    if (peek(parser) == '^') {
        negate = 1;
        ++parser->position;
    }

    /* A ']' first is a literal, which is the only way to put one in a set. */
    int first = 1;

    while (!at_end(parser) && (peek(parser) != ']' || first)) {
        first = 0;

        /* [:alpha:] and friends. [.x.] and [=x=] are collating elements and
         * equivalence classes, which need a locale; the C locale makes them
         * mean the character itself. */
        if (peek(parser) == '['
            && (peek_at(parser, 1) == ':' || peek_at(parser, 1) == '.'
                || peek_at(parser, 1) == '=')) {
            int const kind = peek_at(parser, 1);
            size_t scan = parser->position + 2;
            while (scan + 1 < parser->length
                && !(parser->pattern[scan] == kind && parser->pattern[scan + 1] == ']'))
                ++scan;
            if (scan + 1 >= parser->length) {
                parser->error = REG_EBRACK;
                return 0;
            }

            char const* const name = parser->pattern + parser->position + 2;
            size_t const name_length = scan - (parser->position + 2);
            parser->position = scan + 2;

            if (kind != ':') {
                /* A one-character collating element or equivalence class is
                 * just that character in the C locale. */
                if (name_length != 1) {
                    parser->error = REG_ECOLLATE;
                    return 0;
                }
                set_add(set, (unsigned char)name[0]);
                continue;
            }

            static const struct {
                const char* name;
                int (*test)(int);
            } CLASSES[] = {
                { "alpha", isalpha },
                { "digit", isdigit },
                { "alnum", isalnum },
                { "upper", isupper },
                { "lower", islower },
                { "space", isspace },
                { "blank", isblank },
                { "print", isprint },
                { "punct", ispunct },
                { "cntrl", iscntrl },
                { "graph", isgraph },
                { "xdigit", isxdigit },
            };

            size_t index = 0;
            for (; index < sizeof(CLASSES) / sizeof(CLASSES[0]); ++index) {
                if (strlen(CLASSES[index].name) == name_length
                    && memcmp(CLASSES[index].name, name, name_length) == 0)
                    break;
            }
            if (index == sizeof(CLASSES) / sizeof(CLASSES[0])) {
                parser->error = REG_ECTYPE;
                return 0;
            }
            for (int c = 0; c < 256; ++c) {
                if (CLASSES[index].test(c))
                    set_add(set, (unsigned char)c);
            }
            continue;
        }

        int low = peek(parser);
        ++parser->position;

        /* A range, unless the '-' is last -- [a-] is 'a' and '-'. */
        if (peek(parser) == '-' && peek_at(parser, 1) != ']' && peek_at(parser, 1) != -1) {
            ++parser->position;
            int const high = peek(parser);
            ++parser->position;
            if (high < low) {
                parser->error = REG_ERANGE;
                return 0;
            }
            for (int c = low; c <= high; ++c)
                set_add(set, (unsigned char)c);
        } else {
            set_add(set, (unsigned char)low);
        }
    }

    if (peek(parser) != ']') {
        parser->error = REG_EBRACK;
        return 0;
    }
    ++parser->position;

    if (parser->cflags & REG_ICASE) {
        /* Fold after building, so [a-z] with REG_ICASE also matches A-Z
         * without the range arithmetic having to know. */
        for (int c = 'a'; c <= 'z'; ++c) {
            if (set_has(set, (unsigned char)c))
                set_add(set, (unsigned char)(c - 32));
            if (set_has(set, (unsigned char)(c - 32)))
                set_add(set, (unsigned char)c);
        }
    }

    if (negate) {
        set_invert(set);
        /* REG_NEWLINE takes the newline back out of a negated set: POSIX says
         * [^a] must not match one. */
        if (parser->cflags & REG_NEWLINE)
            set[('\n') / 32] &= ~(1u << ('\n' % 32));
    }

    return 1;
}

/* The \w \W \s \S shorthands, which are GNU rather than POSIX but which every
 * pattern anyone writes uses. */
static int shorthand_set(int c, unsigned* set)
{
    int invert = 0;
    int (*test)(int) = NULL;

    switch (c) {
    case 'W': invert = 1; /* fall through */
    case 'w': test = is_word_character; break;
    case 'S': invert = 1; /* fall through */
    case 's': test = isspace; break;
    default: return 0;
    }

    set_clear(set);
    for (int value = 0; value < 256; ++value) {
        if (test(value))
            set_add(set, (unsigned char)value);
    }
    if (invert)
        set_invert(set);
    return 1;
}

static int parse_atom(Parser* parser, Atom* atom)
{
    int const c = peek(parser);
    if (c < 0) {
        parser->error = REG_BADPAT;
        return 0;
    }

    /* Grouping, in whichever spelling this dialect uses. */
    if (at_meta(parser, '(')) {
        consume_meta(parser, '(');
        atom->type = ATOM_GROUP;
        atom->group_index = (int)++parser->group_count;
        ++parser->depth;
        atom->u.group = parse_alternation(parser);
        --parser->depth;
        if (!atom->u.group)
            return 0;
        if (!consume_meta(parser, ')')) {
            free_alternation(atom->u.group);
            parser->error = REG_EPAREN;
            return 0;
        }
        return 1;
    }

    if (c == '[') {
        ++parser->position;
        atom->type = ATOM_SET;
        return parse_bracket(parser, atom->u.set);
    }

    if (c == '.') {
        ++parser->position;
        if (parser->cflags & REG_NEWLINE) {
            /* . must not match a newline, which is easier as a set than as a
             * special case in the matcher. */
            atom->type = ATOM_SET;
            set_clear(atom->u.set);
            set_invert(atom->u.set);
            atom->u.set[('\n') / 32] &= ~(1u << ('\n' % 32));
            return 1;
        }
        atom->type = ATOM_ANY;
        return 1;
    }

    if (c == '\\') {
        int const escaped = peek_at(parser, 1);
        if (escaped < 0) {
            parser->error = REG_EESCAPE;
            return 0;
        }
        parser->position += 2;

        if (escaped >= '1' && escaped <= '9') {
            /* Backreferences are BRE only in POSIX, but glibc takes them in
             * both and so does everything that matters. */
            atom->type = ATOM_BACKREF;
            atom->u.backref = escaped - '0';
            if ((size_t)atom->u.backref > parser->group_count) {
                parser->error = REG_ESUBREG;
                return 0;
            }
            return 1;
        }

        switch (escaped) {
        case 'b': atom->type = ATOM_WORD_BOUNDARY; return 1;
        case 'B': atom->type = ATOM_NOT_WORD_BOUNDARY; return 1;
        case '<': atom->type = ATOM_WORD_START; return 1;
        case '>': atom->type = ATOM_WORD_END; return 1;
        case 'w':
        case 'W':
        case 's':
        case 'S':
            atom->type = ATOM_SET;
            shorthand_set(escaped, atom->u.set);
            return 1;
        case 'n':
            atom->type = ATOM_LITERAL;
            atom->u.literal = '\n';
            return 1;
        case 't':
            atom->type = ATOM_LITERAL;
            atom->u.literal = '\t';
            return 1;
        case 'r':
            atom->type = ATOM_LITERAL;
            atom->u.literal = '\r';
            return 1;
        default:
            /* Anything else backslashed is itself. */
            atom->type = ATOM_LITERAL;
            atom->u.literal = (unsigned char)escaped;
            return 1;
        }
    }

    ++parser->position;
    atom->type = ATOM_LITERAL;
    atom->u.literal = (unsigned char)c;
    return 1;
}

/* Reads {n}, {n,} or {n,m} -- with the braces already consumed by the caller,
 * which knows whether this dialect spells them \{ \} or { }. */
static int parse_interval(Parser* parser, int* minimum, int* maximum)
{
    if (!isdigit(peek(parser))) {
        parser->error = REG_BADBR;
        return 0;
    }

    long low = 0;
    while (isdigit(peek(parser))) {
        low = low * 10 + (peek(parser) - '0');
        if (low > 32767) {
            parser->error = REG_BADBR;
            return 0;
        }
        ++parser->position;
    }

    long high = low;
    if (peek(parser) == ',') {
        ++parser->position;
        if (isdigit(peek(parser))) {
            high = 0;
            while (isdigit(peek(parser))) {
                high = high * 10 + (peek(parser) - '0');
                if (high > 32767) {
                    parser->error = REG_BADBR;
                    return 0;
                }
                ++parser->position;
            }
        } else {
            high = -1; /* {n,} */
        }
    }

    if (high >= 0 && high < low) {
        parser->error = REG_BADBR;
        return 0;
    }

    *minimum = (int)low;
    *maximum = (int)high;
    return 1;
}

static Branch parse_branch(Parser* parser)
{
    Branch branch = { NULL, 0, 0 };

    while (!at_end(parser)) {
        if (at_meta(parser, '|'))
            break;
        /* A ')' only closes something if there is something open. Outside a
         * group it is an ordinary character, which is what glibc does and what
         * POSIX leaves undefined. */
        if (at_meta(parser, ')') && parser->depth > 0)
            break;

        /* A repetition with nothing before it. BRE has its own rule for a
         * leading '*' below; in ERE there is nothing to repeat and glibc says
         * so. */
        if (extended(parser) && branch.count == 0
            && (peek(parser) == '*' || peek(parser) == '+' || peek(parser) == '?')) {
            parser->error = REG_BADRPT;
            break;
        }

        /* Anchors are atoms, but only where they are allowed to be. In BRE a
         * '^' is an anchor only at the start of the pattern or of a group, and
         * a '$' only at the end; anywhere else they are literals. ERE treats
         * them as anchors everywhere. */
        if (peek(parser) == '^' && (extended(parser) || branch.count == 0)) {
            ++parser->position;
            Piece piece = { { ATOM_BOL, { 0 }, 0 }, 1, 1 };
            piece.atom.type = ATOM_BOL;
            if (!branch_append(&branch, &piece))
                parser->error = REG_ESPACE;
            continue;
        }
        if (peek(parser) == '$') {
            size_t const next = parser->position + 1;
            int const at_pattern_end = next >= parser->length;
            int const before_alternation_or_close = !extended(parser)
                ? (next + 1 < parser->length && parser->pattern[next] == '\\'
                      && (parser->pattern[next + 1] == ')' || parser->pattern[next + 1] == '|'))
                : (next < parser->length
                      && (parser->pattern[next] == ')' || parser->pattern[next] == '|'));

            if (extended(parser) || at_pattern_end || before_alternation_or_close) {
                ++parser->position;
                Piece piece = { { ATOM_EOL, { 0 }, 0 }, 1, 1 };
                piece.atom.type = ATOM_EOL;
                if (!branch_append(&branch, &piece))
                    parser->error = REG_ESPACE;
                continue;
            }
        }

        Piece piece;
        memset(&piece, 0, sizeof(piece));
        piece.minimum = 1;
        piece.maximum = 1;

        /* A '*' with nothing to repeat is a literal in BRE and an error in
         * ERE, which is the one place the dialects disagree about a character
         * that is not spelled differently. */
        if (peek(parser) == '*' && branch.count == 0 && !extended(parser)) {
            ++parser->position;
            piece.atom.type = ATOM_LITERAL;
            piece.atom.u.literal = '*';
            if (!branch_append(&branch, &piece))
                parser->error = REG_ESPACE;
            continue;
        }

        if (!parse_atom(parser, &piece.atom))
            break;

        /* Repetition. A run of them collapses -- a** is a*, and glibc agrees. */
        for (;;) {
            if (peek(parser) == '*') {
                ++parser->position;
                piece.minimum = 0;
                piece.maximum = -1;
            } else if (at_meta(parser, '+')) {
                consume_meta(parser, '+');
                piece.minimum = piece.minimum ? 1 : 0;
                piece.maximum = -1;
            } else if (at_meta(parser, '?')) {
                consume_meta(parser, '?');
                piece.minimum = 0;
                if (piece.maximum != -1)
                    piece.maximum = 1;
            } else if (at_meta(parser, '{')
                && isdigit(extended(parser) ? peek_at(parser, 1) : peek_at(parser, 2))) {
                consume_meta(parser, '{');
                if (!parse_interval(parser, &piece.minimum, &piece.maximum))
                    break;
                if (!consume_meta(parser, '}')) {
                    parser->error = REG_EBRACE;
                    break;
                }
            } else {
                break;
            }
        }

        if (parser->error)
            break;
        if (!branch_append(&branch, &piece)) {
            parser->error = REG_ESPACE;
            break;
        }
    }

    return branch;
}

static void free_branch(Branch* branch);

static Alternation* parse_alternation(Parser* parser)
{
    Alternation* const alternation = calloc(1, sizeof(Alternation));
    if (!alternation) {
        parser->error = REG_ESPACE;
        return NULL;
    }

    for (;;) {
        Branch branch = parse_branch(parser);
        if (parser->error) {
            free_branch(&branch);
            free_alternation(alternation);
            return NULL;
        }
        if (!alternation_append(alternation, &branch)) {
            free_branch(&branch);
            free_alternation(alternation);
            parser->error = REG_ESPACE;
            return NULL;
        }
        if (!consume_meta(parser, '|'))
            break;
    }

    return alternation;
}

static void free_atom(Atom* atom)
{
    if (atom->type == ATOM_GROUP)
        free_alternation(atom->u.group);
}

static void free_branch(Branch* branch)
{
    for (size_t i = 0; i < branch->count; ++i)
        free_atom(&branch->pieces[i].atom);
    free(branch->pieces);
    branch->pieces = NULL;
    branch->count = branch->capacity = 0;
}

static void free_alternation(Alternation* alternation)
{
    if (!alternation)
        return;
    for (size_t i = 0; i < alternation->count; ++i)
        free_branch(&alternation->branches[i]);
    free(alternation->branches);
    free(alternation);
}

/* --- the matcher ---------------------------------------------------------
 *
 * A continuation is "what to do once the current thing has matched". There are
 * only two kinds: the empty one, which means the whole pattern is satisfied,
 * and a group-loop, which closes a capture and decides whether to go round
 * again. Everything else recurses directly, because a branch always knows what
 * comes next within itself.
 *
 * No continuation ever reports success. The empty one records how far it got
 * and returns failure, which drives the search on through every remaining
 * alternative until they are exhausted -- that is what turns a first-match
 * backtracker into the leftmost-longest matcher POSIX asks for.
 */

typedef struct Continuation Continuation;
typedef struct Matcher Matcher;

struct Continuation {
    int (*run)(Matcher*, const Continuation*, size_t position);
};

struct Matcher {
    const char* string;
    size_t length;
    const Program* program;
    int eflags;
    regmatch_t* captures; /* the path being explored */
    regmatch_t* best; /* the longest complete match so far */
    size_t group_count;
    int found;
    regoff_t best_end;
    long budget;
};

/*
 * Backtracking is exponential on patterns like (a*)* against a long run of a's.
 * Refusing to run forever is better than hanging a shell; a pattern that
 * exhausts this reports no match, which is wrong but bounded, and is far more
 * generous than anything a command line will produce.
 */
#define MATCH_BUDGET 2000000L

static int match_branch(Matcher* matcher, const Branch* branch, size_t index, size_t position,
    const Continuation* next);

static int run_continuation(Matcher* matcher, const Continuation* continuation, size_t position)
{
    if (--matcher->budget < 0)
        return 1; /* stop unwinding; the answer is whatever was found so far */

    if (!continuation) {
        if (!matcher->found || (regoff_t)position > matcher->best_end) {
            matcher->found = 1;
            matcher->best_end = (regoff_t)position;
            memcpy(
                matcher->best, matcher->captures, (matcher->group_count + 1) * sizeof(regmatch_t));
            matcher->best[0].rm_eo = (regoff_t)position;
        }
        return 0;
    }

    return continuation->run(matcher, continuation, position);
}

static int character_matches(const Matcher* matcher, const Atom* atom, unsigned char c)
{
    switch (atom->type) {
    case ATOM_LITERAL:
        if ((matcher->program->cflags & REG_ICASE) == 0)
            return c == atom->u.literal;
        return tolower(c) == tolower(atom->u.literal);
    case ATOM_ANY: return 1;
    case ATOM_SET: return set_has(atom->u.set, c);
    default: return 0;
    }
}

static int at_line_start(const Matcher* matcher, size_t position)
{
    if (position == 0)
        return (matcher->eflags & REG_NOTBOL) == 0;
    if (matcher->program->cflags & REG_NEWLINE)
        return matcher->string[position - 1] == '\n';
    return 0;
}

static int at_line_end(const Matcher* matcher, size_t position)
{
    if (position == matcher->length)
        return (matcher->eflags & REG_NOTEOL) == 0;
    if (matcher->program->cflags & REG_NEWLINE)
        return matcher->string[position] == '\n';
    return 0;
}

static int word_at(const Matcher* matcher, size_t position)
{
    return position < matcher->length
        && is_word_character((unsigned char)matcher->string[position]);
}

static int word_before(const Matcher* matcher, size_t position)
{
    return position > 0 && is_word_character((unsigned char)matcher->string[position - 1]);
}

static int match_alternation(
    Matcher* matcher, const Alternation* alternation, size_t position, const Continuation* next)
{
    for (size_t i = 0; i < alternation->count; ++i) {
        if (match_branch(matcher, &alternation->branches[i], 0, position, next))
            return 1;
    }
    return 0;
}

/* A group piece, `taken` iterations in. Declared here because the loop
 * continuation below calls back into it. */
static int match_group(Matcher* matcher, const Branch* branch, size_t index, size_t position,
    const Continuation* next, int taken);

typedef struct {
    Continuation base;
    const Branch* branch;
    size_t index;
    const Continuation* after;
    size_t start; /* where this iteration of the body began */
    int taken; /* iterations completed before this one */
} GroupLoop;

static int group_loop_run(Matcher* matcher, const Continuation* continuation, size_t position)
{
    const GroupLoop* const loop = (const GroupLoop*)continuation;
    const Atom* const atom = &loop->branch->pieces[loop->index].atom;

    regmatch_t const saved = matcher->captures[atom->group_index];

    /* An iteration that consumed nothing does not count as the group's match.
     * `(a*)*` against "aaa" takes "aaa" and then a final empty pass; recording
     * that pass would report the group as the empty string at the end instead
     * of the "aaa" it actually matched. Only the first iteration may be empty,
     * because then the group really did match nothing. */
    int const empty = position == loop->start;
    if (!empty || loop->taken == 0) {
        matcher->captures[atom->group_index].rm_so = (regoff_t)loop->start;
        matcher->captures[atom->group_index].rm_eo = (regoff_t)position;
    }

    int result;
    if (empty) {
        /* Going round again would match nothing again, forever -- and since an
         * empty iteration can be repeated any number of times, one of them
         * satisfies any minimum. */
        result = match_branch(matcher, loop->branch, loop->index + 1, position, loop->after);
    } else {
        result = match_group(
            matcher, loop->branch, loop->index, position, loop->after, loop->taken + 1);
    }

    if (!result)
        matcher->captures[atom->group_index] = saved;
    return result;
}

static int match_group(Matcher* matcher, const Branch* branch, size_t index, size_t position,
    const Continuation* next, int taken)
{
    if (--matcher->budget < 0)
        return 1;

    const Piece* const piece = &branch->pieces[index];
    const Atom* const atom = &piece->atom;

    /* Greedy: another iteration before giving up on one. */
    if (piece->maximum < 0 || taken < piece->maximum) {
        GroupLoop loop;
        loop.base.run = group_loop_run;
        loop.branch = branch;
        loop.index = index;
        loop.after = next;
        loop.start = position;
        loop.taken = taken;

        regmatch_t const saved = matcher->captures[atom->group_index];
        if (match_alternation(matcher, atom->u.group, position, &loop.base))
            return 1;
        matcher->captures[atom->group_index] = saved;
    }

    if (taken >= piece->minimum)
        return match_branch(matcher, branch, index + 1, position, next);

    return 0;
}

static int match_branch(
    Matcher* matcher, const Branch* branch, size_t index, size_t position, const Continuation* next)
{
    if (--matcher->budget < 0)
        return 1;

    if (index >= branch->count)
        return run_continuation(matcher, next, position);

    const Piece* const piece = &branch->pieces[index];
    const Atom* const atom = &piece->atom;

    /* Assertions consume nothing, so repeating one is meaningless. */
    switch (atom->type) {
    case ATOM_BOL:
        return at_line_start(matcher, position)
            ? match_branch(matcher, branch, index + 1, position, next)
            : 0;
    case ATOM_EOL:
        return at_line_end(matcher, position)
            ? match_branch(matcher, branch, index + 1, position, next)
            : 0;
    case ATOM_WORD_BOUNDARY:
        return word_before(matcher, position) != word_at(matcher, position)
            ? match_branch(matcher, branch, index + 1, position, next)
            : 0;
    case ATOM_NOT_WORD_BOUNDARY:
        return word_before(matcher, position) == word_at(matcher, position)
            ? match_branch(matcher, branch, index + 1, position, next)
            : 0;
    case ATOM_WORD_START:
        return (!word_before(matcher, position) && word_at(matcher, position))
            ? match_branch(matcher, branch, index + 1, position, next)
            : 0;
    case ATOM_WORD_END:
        return (word_before(matcher, position) && !word_at(matcher, position))
            ? match_branch(matcher, branch, index + 1, position, next)
            : 0;
    default: break;
    }

    int const minimum = piece->minimum;
    int const maximum = piece->maximum;

    /*
     * A single-character atom cannot capture, so the whole greedy run can be
     * measured at once and then given back one character at a time. That
     * avoids a recursion per repetition, which for `.*` on a long line is the
     * difference between working and not.
     */
    if (atom->type == ATOM_LITERAL || atom->type == ATOM_ANY || atom->type == ATOM_SET) {
        int taken = 0;
        while ((maximum < 0 || taken < maximum) && position + (size_t)taken < matcher->length
            && character_matches(
                matcher, atom, (unsigned char)matcher->string[position + (size_t)taken]))
            ++taken;

        if (taken < minimum)
            return 0;

        for (int count = taken; count >= minimum; --count) {
            if (match_branch(matcher, branch, index + 1, position + (size_t)count, next))
                return 1;
        }
        return 0;
    }

    if (atom->type == ATOM_BACKREF) {
        regmatch_t const captured = matcher->captures[atom->u.backref];
        if (captured.rm_so < 0 || captured.rm_eo < 0) {
            /* A group that never participated matches nothing at all, so the
             * backreference can only be satisfied by taking it zero times. */
            return minimum == 0 ? match_branch(matcher, branch, index + 1, position, next) : 0;
        }

        size_t const width = (size_t)(captured.rm_eo - captured.rm_so);
        if (width == 0) {
            /* An empty capture matches the empty string as often as asked. */
            return match_branch(matcher, branch, index + 1, position, next);
        }

        int taken = 0;
        while (maximum < 0 || taken < maximum) {
            size_t const at = position + (size_t)taken * width;
            if (at + width > matcher->length)
                break;
            int same = 1;
            for (size_t i = 0; i < width && same; ++i) {
                unsigned char const a = (unsigned char)matcher->string[at + i];
                unsigned char const b
                    = (unsigned char)matcher->string[captured.rm_so + (regoff_t)i];
                same = (matcher->program->cflags & REG_ICASE) ? tolower(a) == tolower(b) : a == b;
            }
            if (!same)
                break;
            ++taken;
        }

        if (taken < minimum)
            return 0;

        for (int count = taken; count >= minimum; --count) {
            if (match_branch(matcher, branch, index + 1, position + (size_t)count * width, next))
                return 1;
        }
        return 0;
    }

    if (atom->type == ATOM_GROUP)
        return match_group(matcher, branch, index, position, next, 0);

    return 0;
}

/* --- the public interface ------------------------------------------------ */

int regcomp(regex_t* compiled, const char* pattern, int cflags)
{
    if (!compiled || !pattern)
        return REG_BADPAT;

    compiled->re_program = NULL;
    compiled->re_nsub = 0;
    compiled->re_cflags = cflags;

    Parser parser;
    parser.pattern = pattern;
    parser.position = 0;
    parser.length = strlen(pattern);
    parser.cflags = cflags;
    parser.group_count = 0;
    parser.depth = 0;
    parser.error = 0;

    Alternation* const root = parse_alternation(&parser);
    if (!root)
        return parser.error ? parser.error : REG_BADPAT;

    /* Anything left over is an unbalanced close paren -- parse_alternation
     * stops at one and expects its caller to consume it. */
    if (!at_end(&parser)) {
        free_alternation(root);
        return REG_EPAREN;
    }

    Program* const program = calloc(1, sizeof(Program));
    if (!program) {
        free_alternation(root);
        return REG_ESPACE;
    }

    program->root = root;
    program->group_count = parser.group_count;
    program->cflags = cflags;

    compiled->re_program = program;
    compiled->re_nsub = parser.group_count;
    return 0;
}

void regfree(regex_t* compiled)
{
    if (!compiled || !compiled->re_program)
        return;
    Program* const program = compiled->re_program;
    free_alternation(program->root);
    free(program);
    compiled->re_program = NULL;
    compiled->re_nsub = 0;
}

int regexec(const regex_t* compiled, const char* string, size_t match_count, regmatch_t matches[],
    int eflags)
{
    if (!compiled || !compiled->re_program || !string)
        return REG_BADPAT;

    const Program* const program = compiled->re_program;
    size_t const groups = program->group_count;

    regmatch_t* const live = calloc(groups + 1, sizeof(regmatch_t));
    regmatch_t* const best = calloc(groups + 1, sizeof(regmatch_t));
    if (!live || !best) {
        free(live);
        free(best);
        return REG_ESPACE;
    }

    Matcher matcher;
    matcher.string = string;
    matcher.length = strlen(string);
    matcher.program = program;
    matcher.eflags = eflags;
    matcher.captures = live;
    matcher.best = best;
    matcher.group_count = groups;

    /* Leftmost: the first start offset that matches at all wins, however long
     * a later one might be. */
    for (size_t start = 0; start <= matcher.length; ++start) {
        for (size_t i = 0; i <= groups; ++i) {
            live[i].rm_so = -1;
            live[i].rm_eo = -1;
        }
        live[0].rm_so = (regoff_t)start;

        matcher.found = 0;
        matcher.best_end = -1;
        matcher.budget = MATCH_BUDGET;

        (void)match_alternation(&matcher, program->root, start, NULL);

        if (matcher.found) {
            best[0].rm_so = (regoff_t)start;
            if (matches && match_count > 0 && (program->cflags & REG_NOSUB) == 0) {
                for (size_t i = 0; i < match_count; ++i) {
                    if (i <= groups) {
                        matches[i] = best[i];
                    } else {
                        matches[i].rm_so = -1;
                        matches[i].rm_eo = -1;
                    }
                }
            } else if (matches && match_count > 0) {
                matches[0] = best[0];
                for (size_t i = 1; i < match_count; ++i) {
                    matches[i].rm_so = -1;
                    matches[i].rm_eo = -1;
                }
            }
            free(live);
            free(best);
            return 0;
        }
    }

    free(live);
    free(best);
    return REG_NOMATCH;
}

size_t regerror(int code, const regex_t* compiled, char* buffer, size_t capacity)
{
    (void)compiled;

    const char* message;
    switch (code) {
    case 0: message = "success"; break;
    case REG_NOMATCH: message = "no match"; break;
    case REG_BADPAT: message = "invalid regular expression"; break;
    case REG_ECOLLATE: message = "invalid collating element"; break;
    case REG_ECTYPE: message = "invalid character class"; break;
    case REG_EESCAPE: message = "trailing backslash"; break;
    case REG_ESUBREG: message = "invalid back reference"; break;
    case REG_EBRACK: message = "unmatched ["; break;
    case REG_EPAREN: message = "unmatched ("; break;
    case REG_EBRACE: message = "unmatched {"; break;
    case REG_BADBR: message = "invalid contents of {}"; break;
    case REG_ERANGE: message = "invalid range end"; break;
    case REG_ESPACE: message = "out of memory"; break;
    case REG_BADRPT: message = "nothing to repeat"; break;
    default: message = "unknown error"; break;
    }

    size_t const length = strlen(message);
    if (buffer && capacity > 0) {
        size_t const copied = length < capacity - 1 ? length : capacity - 1;
        memcpy(buffer, message, copied);
        buffer[copied] = '\0';
    }
    return length + 1;
}
