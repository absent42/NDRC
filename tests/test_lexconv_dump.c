/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_lexconv_dump.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "../src/front/lex_tables.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Compiled checker that closes a gap lexconv.py's own --check cannot:
   --check parses the committed header's TEXT with regexes, which is
   nearly circular since both the header and the canon come from the
   same Python parse of lexer.pas. This test instead walks the
   EMITTED C ARRAYS themselves, so a mis-packed cc[8] bit fails here
   even when lexconv.py --check passes. Two representations (FAITHFUL,
   decoded from yyt's packed cc[8]; FLATTENED, yynext) are emitted and
   cross-checked against each other and against the committed
   tests/oracle/lex_tables.canon.

   CANON_PATH below is resolved relative to the current working
   directory, not this binary's own location, since the test suite is
   always built and run from the repository root. */

#define CANON_PATH "tests/oracle/lex_tables.canon"
#define BUF_CAP (1 << 20)

static char g_dump[BUF_CAP];
static size_t g_len;

/* The faithful next-state matrix, decoded straight from yyt's packed
   cc[8] bits - see build_faithful_next. Kept alongside yynext (the
   flattened form lex_tables.h already provides) so the two can be
   cross-checked against each other, not just each against the canon. */
static int16_t g_faithful_next[YYNSTATES][256];

static void emit(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(g_dump + g_len, BUF_CAP - g_len, fmt, ap);
    va_end(ap);

    CHECK(n >= 0);
    CHECK(g_len + (size_t)n < BUF_CAP);
    g_len += (size_t)n;
}

/* True iff character `ch` (0..255) is a member of `rec`'s packed cc
   set - bit n of cc[n/32] is character n, exactly as lexconv.py's
   format_yyt packs it (word = ordv // 32, bit = ordv % 32). This is
   the ONLY place in this file that reads cc[8] - every other check
   here goes through g_faithful_next, built below. */
static int cc_test(const YYTRec *rec, int ch)
{
    int word = ch >> 5;
    int bit = ch & 31;
    return (int)((rec->cc[word] >> (unsigned)bit) & 1u);
}

/* Flags (via CHECK_INT, not a hard abort) any character claimed by
   more than one transition in the same state - the disjointness
   invariant lexconv.py's own build_flattened() asserts on the Python
   side while building yynext; a violation here would mean the emitted
   cc[8] bytes broke it even though the Python IR did not. */
static void build_faithful_next(void)
{
    int state;

    for (state = 0; state < YYNSTATES; state++) {
        int ch;

        for (ch = 0; ch < 256; ch++)
            g_faithful_next[state][ch] = -1;

        if (yytl[state] > yyth[state])
            continue;  /* dead state - empty slice */

        {
            int idx;
            for (idx = yytl[state]; idx <= yyth[state]; idx++) {
                const YYTRec *rec = &yyt[idx];
                for (ch = 0; ch < 256; ch++) {
                    if (!cc_test(rec, ch))
                        continue;
                    CHECK_INT(g_faithful_next[state][ch], -1);
                    g_faithful_next[state][ch] = rec->s;
                }
            }
        }
    }
}

/* Rebuilds the canonical dump format, mirroring lexconv.py's
   render_canon(). */
static void build_dump(void)
{
    int state;

    build_faithful_next();
    g_len = 0;
    for (state = 0; state < YYNSTATES; state++) {
        int col;

        emit("%d:", state);
        for (col = 0; col < 256; col++)
            emit(" %d", (int)g_faithful_next[state][col]);
        emit(" |");
        if (yykl[state] <= yykh[state]) {
            int idx;
            for (idx = yykl[state]; idx <= yykh[state]; idx++)
                emit(" %d", (int)yyk[idx]);
        }
        emit("\n");
    }
}

static char *slurp_canon(size_t *out_len)
{
    static char buf[BUF_CAP];
    FILE *f = fopen(CANON_PATH, "rb");
    size_t n;

    CHECK(f != NULL);
    if (f == NULL) {
        *out_len = 0;
        return buf;
    }
    n = fread(buf, 1, sizeof buf, f);
    CHECK(!ferror(f));
    fclose(f);
    *out_len = n;
    return buf;
}

TEST(dump_matches_committed_canon)
{
    size_t canon_len, minlen, i;
    char *canon;

    build_dump();
    canon = slurp_canon(&canon_len);

    CHECK_INT(g_len, canon_len);

    minlen = g_len < canon_len ? g_len : canon_len;
    for (i = 0; i < minlen; i++) {
        if (g_dump[i] != canon[i]) {
            printf("  first difference at offset %lu: "
                   "regenerated=0x%02X committed=0x%02X\n",
                   (unsigned long)i, (unsigned char)g_dump[i],
                   (unsigned char)canon[i]);
            break;
        }
    }
    if (g_len == canon_len)
        CHECK_MEM(g_dump, canon, g_len);
}

/* Cross-checks FAITHFUL (g_faithful_next, from yyt's cc[8]) against
   FLATTENED (yynext) for every (state, character) pair: independent
   code paths from independent arrays, so disagreement means mis-emit. */
TEST(faithful_matches_flattened_exhaustively)
{
    int state, col;

    build_faithful_next();
    for (state = 0; state < YYNSTATES; state++) {
        for (col = 0; col < 256; col++) {
            CHECK_INT(g_faithful_next[state][col], yynext[state][col]);
        }
    }
}

/* A handful of state-shape sanity checks, independent of the full-dump
   comparison above - a guard against both sides being wrong (or empty)
   in the same way and the comparison above passing by accident. */
TEST(basic_shape_sanity)
{
    int state, col;

    CHECK_INT(YYNSTATES, 123);
    CHECK_INT(YYNMARKS, 93);
    CHECK_INT(YYNMATCHES, 93);
    CHECK_INT(YYNTRANS, 186);

    /* column 0 (the EOF sentinel, #0) is unset in every state - no cc
       set in lexer.pas ever contains it. */
    for (state = 0; state < YYNSTATES; state++)
        CHECK_INT(yynext[state][0], -1);

    /* BOL row 0 and mid-line row 1 are byte-identical. */
    for (col = 0; col < 256; col++)
        CHECK_INT(yynext[0][col], yynext[1][col]);
    CHECK_INT(yykl[0], yykl[1]);
    CHECK_INT(yykh[0], yykh[1]);
}

int main(void)
{
    RUN(dump_matches_committed_canon);
    RUN(faithful_matches_flattened_exhaustively);
    RUN(basic_shape_sanity);
    return test_summary("lexconv_dump");
}
