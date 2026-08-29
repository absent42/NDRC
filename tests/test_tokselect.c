/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_tokselect.c - unit tests for src/tokselect.c.
   Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "../src/tokselect.h"

static Vec_Str *strs1(Arena *a, const char *s)
{
    Vec_Str *v = vec_new_Str(a);
    vec_push_Str(v, str_from(a, s));
    return v;
}

static Vec_Str *toks(Arena *a, const char *t1, const char *t2)
{
    Vec_Str *v = vec_new_Str(a);
    if (t1 != NULL) vec_push_Str(v, str_from(a, t1));
    if (t2 != NULL) vec_push_Str(v, str_from(a, t2));
    return v;
}

TEST(parse_literal_only)
{
    Arena *a = arena_new(0);
    CHECK_INT(tokselect_parse_total(a, strs1(a, "HELLO"), NULL), 5);
    CHECK_INT(tokselect_parse_total(a, strs1(a, ""), NULL), 0);
}

TEST(parse_token_reduces)
{
    Arena *a = arena_new(0);
    CHECK_INT(tokselect_parse_total(a, strs1(a, "ABABAB"),
                                    toks(a, "AB", NULL)), 3);
}

TEST(parse_is_optimal_not_greedy)
{
    /* Leftmost-greedy would take AB and pay X,AB,C,Y = 4; the DP must
       find X,ABC,Y = 3. */
    Arena *a = arena_new(0);
    CHECK_INT(tokselect_parse_total(a, strs1(a, "XABCY"),
                                    toks(a, "AB", "ABC")), 3);
}

TEST(parse_picks_cheaper_cover)
{
    /* AA+AA = 2 beats AAA+A = 2? equal - but AA,AA is 2 and must not
       come out worse than 2. Pin the exact optimum. */
    Arena *a = arena_new(0);
    CHECK_INT(tokselect_parse_total(a, strs1(a, "AAAA"),
                                    toks(a, "AA", "AAA")), 2);
}

TEST(parse_token_longer_than_string)
{
    Arena *a = arena_new(0);
    CHECK_INT(tokselect_parse_total(a, strs1(a, "AB"),
                                    toks(a, "ABCD", NULL)), 2);
}

int main(void)
{
    RUN(parse_literal_only);
    RUN(parse_token_reduces);
    RUN(parse_is_optimal_not_greedy);
    RUN(parse_picks_cheaper_cover);
    RUN(parse_token_longer_than_string);
    return test_summary("tokselect");
}
