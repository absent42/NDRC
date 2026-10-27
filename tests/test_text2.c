/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_text2.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "arena.h"
#include "diag.h"
#include "str.h"
#include "huff.h"
#include "text2.h"

#include <string.h>

static unsigned long rng_state;

static unsigned long rng(void)
{
    rng_state = rng_state * 1103515245ul + 12345ul;
    return (rng_state >> 16) & 0x7FFFul;
}

static Vec_Str *toklist(Arena *a, const char **t, size_t n)
{
    Vec_Str *v = vec_new_Str(a);
    for (size_t i = 0; i < n; i++) vec_push_Str(v, str_from(a, t[i]));
    return v;
}

/* Index of raw token text in r->tokens, or -1. */
static long tok_index(const Vec_Str *tokens, const char *s)
{
    for (size_t i = 0; i < vec_len_Str(tokens); i++) {
        Str *t = vec_at_Str(tokens, i);
        if (str_len(t) == strlen(s) && memcmp(str_bytes(t), s, strlen(s)) == 0)
            return (long)i;
    }
    return -1;
}

static int entry_has_refs(const Str *e)
{
    for (size_t i = 1; i < str_len(e); i++)
        if (str_bytes(e)[i] >= 128) return 1;
    return 0;
}

/* Every reference targets a lower-numbered (below 128) ref-free entry. */
static int entries_one_level(const Vec_Str *entries)
{
    for (size_t i = 0; i < vec_len_Str(entries); i++) {
        Str *e = vec_at_Str(entries, i);
        if (str_len(e) < 2 || str_bytes(e)[0] != str_len(e) - 1 ||
            str_bytes(e)[0] > 32) return 0;
        for (size_t k = 1; k < str_len(e); k++) {
            unsigned b = str_bytes(e)[k];
            if (b < 128) continue;
            if (b - 128 >= vec_len_Str(entries)) return 0;
            if (entry_has_refs(vec_at_Str(entries, b - 128))) return 0;
        }
    }
    return 1;
}

/* Expands entry k (one level) to dec. */
static void expand(Str *dec, const Vec_Str *entries, size_t k)
{
    Str *e = vec_at_Str(entries, k);
    for (size_t i = 1; i < str_len(e); i++) {
        unsigned b = str_bytes(e)[i];
        if (b >= 128) expand(dec, entries, b - 128);
        else str_push_u8(dec, b);
    }
}

TEST(nests_one_level_lower_only)
{
    Arena *a = arena_new(0);
    const char *t[] = {"abcdef", "abcd", "ab", "xy"};
    long uses[] = {5, 5, 5, 5};
    size_t perm[4];
    Vec_Str *raw = toklist(a, t, 4);
    Vec_Str *ent = text2_nest(a, raw, uses, perm);
    Vec_Str *now = vec_new_Str(a);
    long iab, iabcd, iabcdef;
    Str *e;

    for (size_t i = 0; i < 4; i++) vec_push_Str(now, vec_at_Str(raw, perm[i]));
    iab = tok_index(now, "ab");
    iabcd = tok_index(now, "abcd");
    iabcdef = tok_index(now, "abcdef");
    CHECK(iab >= 0 && iab < 128);
    CHECK(iabcd >= 0 && iabcd < 128);
    /* "abcdef" nests on "abcd" (pinned); "abcd" then stays raw. */
    e = vec_at_Str(ent, (size_t)iabcdef);
    CHECK_INT(str_len(e), 4);
    CHECK_INT(str_bytes(e)[0], 3);
    CHECK_INT(str_bytes(e)[1], 128 + iabcd);
    CHECK(memcmp(str_bytes(e) + 2, "ef", 2) == 0);
    e = vec_at_Str(ent, (size_t)iabcd);
    CHECK_INT(str_len(e), 5);
    CHECK(memcmp(str_bytes(e), "\x04" "abcd", 5) == 0);
    CHECK(!entry_has_refs(vec_at_Str(ent, (size_t)iab)));
    CHECK(entries_one_level(ent));
    arena_free(a);

    /* The brief's pair: "abcd" = [ref "ab"]"cd", 3 bytes against 4. */
    a = arena_new(0);
    {
        const char *t2[] = {"abcd", "ab"};
        long u2[] = {3, 9};
        size_t p2[2];
        Vec_Str *r2 = toklist(a, t2, 2);
        Vec_Str *e2 = text2_nest(a, r2, u2, p2);
        /* Most-used first: "ab" is 0, "abcd" is 1. */
        CHECK_INT(p2[0], 1);
        CHECK_INT(p2[1], 0);
        CHECK_INT(str_len(vec_at_Str(e2, 1)), 4);
        CHECK(memcmp(str_bytes(vec_at_Str(e2, 1)), "\x03\x80" "cd", 4) == 0);
        CHECK(memcmp(str_bytes(vec_at_Str(e2, 0)), "\x02" "ab", 3) == 0);
    }
    arena_free(a);
}

TEST(renumbers_referenced_below_128)
{
    Arena *a = arena_new(0);
    Vec_Str *raw = vec_new_Str(a);
    long uses[200];
    size_t perm[200];
    Vec_Str *ent;
    long where = -1, host = -1;

    /* 200 distinct 3-byte tokens over a-z; token 150 = "QZ" (2 bytes,
       least used), token 0 = "QZx" which nests on it. */
    for (size_t i = 0; i < 200; i++) {
        Str *s = str_new(a);
        if (i == 150) str_append(s, "QZ");
        else if (i == 0) str_append(s, "QZx");
        else {
            str_push(s, (char)('a' + i % 26));
            str_push(s, (char)('a' + (i / 26) % 26));
            str_push(s, 'k');
        }
        vec_push_Str(raw, s);
        uses[i] = 1000 - (long)i;
    }
    uses[150] = 1;
    ent = text2_nest(a, raw, uses, perm);
    for (size_t i = 0; i < 200; i++) {
        if (perm[i] == 150) where = (long)i;
        if (perm[i] == 0) host = (long)i;
    }
    CHECK(where >= 0 && where < 128);
    CHECK(host >= 0);
    CHECK_INT(str_bytes(vec_at_Str(ent, (size_t)host))[1], 128 + where);
    CHECK(entries_one_level(ent));
    /* Unreferenced tokens keep most-used-first order: token 1 first. */
    CHECK_INT(perm[0], 0);
    CHECK_INT(perm[1], 1);
    arena_free(a);
}

static Vec_Str *small_corpus(Arena *a)
{
    Vec_Str *v = vec_new_Str(a);
    vec_push_Str(v, str_from(a, "You are in a dark forest. The path leads north."));
    vec_push_Str(v, str_from(a, "You are in a clearing. The path leads south."));
    vec_push_Str(v, str_from(a, "You are by a river. The water is cold."));
    vec_push_Str(v, str_from(a, "I can't do that."));
    vec_push_Str(v, str_from(a, "You are carrying nothing."));
    return v;
}

/* 300 distinct random 9-letter phrases, each used 8 times, shuffled
   into lines of 6 phrases. */
static Vec_Str *phrase_corpus(Arena *a)
{
    enum { NP = 300, REP = 8, PER = 6 };
    static char ph[NP][10];
    static unsigned short order[NP * REP];
    Vec_Str *v = vec_new_Str(a);
    size_t i;

    rng_state = 12345;
    for (i = 0; i < NP; i++) {
        for (int k = 0; k < 9; k++) ph[i][k] = (char)('a' + rng() % 26);
        ph[i][9] = 0;
    }
    for (i = 0; i < NP * REP; i++) order[i] = (unsigned short)(i % NP);
    for (i = NP * REP - 1; i > 0; i--) {
        size_t j = rng() % (i + 1);
        unsigned short t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (i = 0; i < NP * REP; i += PER) {
        Str *s = str_new(a);
        for (size_t k = i; k < i + PER && k < NP * REP; k++) {
            str_append(s, ph[order[k]]);
            str_push(s, k % 3 == 2 ? '.' : ' ');
        }
        vec_push_Str(v, s);
    }
    return v;
}

TEST(picks_128_when_383_loses)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Text2Result *r = text2_build(a, d, small_corpus(a));
    CHECK(r != NULL);
    CHECK_INT(r->alphabet383, 0);
    CHECK_INT(r->nsym, 256);
    CHECK(vec_len_Str(r->tokens) <= 128);
    arena_free(a);
}

TEST(picks_383_when_it_wins)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Text2Result *r = text2_build(a, d, phrase_corpus(a));
    CHECK(r != NULL);
    CHECK_INT(r->alphabet383, 1);
    CHECK_INT(r->nsym, 511);
    CHECK(vec_len_Str(r->tokens) > 128);
    CHECK(vec_len_Str(r->tokens) <= 383);
    CHECK(entries_one_level(r->entries));
    arena_free(a);
}

TEST(block_layout)
{
    Arena *a = arena_new(0);
    Text2Result r;
    Str *e0 = str_new(a), *e1 = str_new(a), *blk;
    const unsigned char *b;
    unsigned char packed[256];
    unsigned short crc;

    memset(&r, 0, sizeof r);
    r.alphabet383 = 0;
    r.nsym = 256;
    r.tokens = vec_new_Str(a);
    vec_push_Str(r.tokens, str_from(a, "ab"));
    vec_push_Str(r.tokens, str_from(a, "abc"));
    str_append(e0, "\x02" "ab");
    str_append(e1, "\x02\x80" "c");
    r.entries = vec_new_Str(a);
    vec_push_Str(r.entries, e0);
    vec_push_Str(r.entries, e1);
    r.lengths[10] = 1; r.lengths['a'] = 2; r.lengths['b'] = 3;
    r.lengths['c'] = 4; r.lengths[128] = 5; r.lengths[129] = 6;
    blk = text2_block(a, &r);
    b = str_bytes(blk);
    CHECK_INT(str_len(blk), 7 + 128 + 4 + 3 + 3);
    CHECK(b[0] == 'H' && b[1] == '2');
    CHECK_INT(b[2], 0);
    CHECK_INT(b[3] | b[4] << 8, 2);
    memset(packed, 0, sizeof packed);
    for (unsigned i = 0; i < 256; i++)
        packed[i / 2] |= (unsigned char)(r.lengths[i] << (i % 2 ? 4 : 0));
    CHECK_MEM(b + 7, packed, 128);
    CHECK_INT(b[7 + 5], 1);                    /* symbol 10, low nibble */
    CHECK_INT(b[7 + 48], 2 << 4);              /* 'a' = 97, high nibble */
    CHECK_INT(b[7 + 64], 5 | 6 << 4);          /* 128, 129 */
    crc = crc16_ccitt(packed, 128);
    CHECK_INT(b[5] | b[6] << 8, crc);
    CHECK_INT(b[135] | b[136] << 8, 139);
    CHECK_INT(b[137] | b[138] << 8, 142);
    CHECK_MEM(b + 139, "\x02" "ab" "\x02\x80" "c", 6);

    /* 383 alphabet: flag bit 0, 256-byte lengths field. */
    r.alphabet383 = 1;
    r.nsym = 511;
    r.lengths[510] = 7;
    blk = text2_block(a, &r);
    b = str_bytes(blk);
    CHECK_INT(str_len(blk), 7 + 256 + 4 + 6);
    CHECK_INT(b[2], 1);
    CHECK_INT(b[7 + 255], 7);
    CHECK_INT(b[263] | b[264] << 8, 267);
    arena_free(a);
}

/* Decodes every coded stream through the entries; 1 = all match. */
static int decodes_all(const Text2Result *r, const Vec_Str *strings)
{
    Arena *s = arena_new(0);
    int ok = vec_len_Str(r->coded) == vec_len_Str(strings);
    for (size_t i = 0; ok && i < vec_len_Str(strings); i++) {
        Str *c = vec_at_Str(r->coded, i), *dec = str_new(s);
        Str *want = vec_at_Str(strings, i);
        size_t pos = 0;
        for (;;) {
            int sym = huff_decode_one(r->lengths, r->nsym, str_bytes(c),
                                      str_len(c), &pos);
            if (sym < 0) { ok = 0; break; }
            if (sym == 10) break;
            if (sym < 128) str_push_u8(dec, (unsigned)sym);
            else if ((size_t)sym - 128 >= vec_len_Str(r->entries)) { ok = 0; break; }
            else expand(dec, r->entries, (size_t)sym - 128);
        }
        ok = ok && (pos + 7) / 8 == str_len(c) && str_len(dec) == str_len(want)
             && memcmp(str_bytes(dec), str_bytes(want), str_len(want)) == 0;
    }
    arena_free(s);
    return ok;
}

static long coded_total(const Text2Result *r)
{
    long t = 0;
    for (size_t i = 0; i < vec_len_Str(r->coded); i++)
        t += (long)str_len(vec_at_Str(r->coded, i));
    return t;
}

TEST(coded_streams_decode)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Vec_Str *s1 = small_corpus(a), *s2 = phrase_corpus(a);
    Text2Result *r1 = text2_build(a, d, s1), *r2 = text2_build(a, d, s2);

    CHECK(decodes_all(r1, s1));
    CHECK(decodes_all(r2, s2));
    CHECK(entries_one_level(r1->entries));
    CHECK_INT(r1->text_bytes, coded_total(r1));
    CHECK_INT(r2->text_bytes, coded_total(r2));
    CHECK_INT(r1->block_bytes, (long)str_len(text2_block(a, r1)));
    CHECK_INT(r2->block_bytes, (long)str_len(text2_block(a, r2)));
    CHECK_INT(vec_len_Str(r2->tokens), vec_len_Str(r2->entries));
    arena_free(a);
}

static int vecs_equal(const Vec_Str *x, const Vec_Str *y)
{
    if (vec_len_Str(x) != vec_len_Str(y)) return 0;
    for (size_t i = 0; i < vec_len_Str(x); i++) {
        Str *p = vec_at_Str(x, i), *q = vec_at_Str(y, i);
        if (str_len(p) != str_len(q) ||
            memcmp(str_bytes(p), str_bytes(q), str_len(p)) != 0) return 0;
    }
    return 1;
}

TEST(deterministic_double_run)
{
    Arena *a = arena_new(0), *b = arena_new(0);
    Text2Result *x = text2_build(a, diag_new(a), phrase_corpus(a));
    Text2Result *y = text2_build(b, diag_new(b), phrase_corpus(b));
    Str *bx = text2_block(a, x), *by = text2_block(b, y);

    CHECK_INT(x->alphabet383, y->alphabet383);
    CHECK_INT(x->nsym, y->nsym);
    CHECK(memcmp(x->lengths, y->lengths, sizeof x->lengths) == 0);
    CHECK(vecs_equal(x->tokens, y->tokens));
    CHECK(vecs_equal(x->entries, y->entries));
    CHECK(vecs_equal(x->coded, y->coded));
    CHECK_INT(x->text_bytes, y->text_bytes);
    CHECK_INT(x->block_bytes, y->block_bytes);
    CHECK(str_len(bx) == str_len(by) &&
          memcmp(str_bytes(bx), str_bytes(by), str_len(bx)) == 0);
    arena_free(a);
    arena_free(b);
}

int main(void)
{
    RUN(nests_one_level_lower_only);
    RUN(renumbers_referenced_below_128);
    RUN(picks_128_when_383_loses);
    RUN(picks_383_when_it_wins);
    RUN(block_layout);
    RUN(coded_streams_decode);
    RUN(deterministic_double_run);
    return test_summary("test_text2");
}
