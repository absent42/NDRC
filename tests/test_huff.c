/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_huff.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "arena.h"
#include "str.h"
#include "huff.h"

#include <string.h>

static unsigned long rng_state;

static unsigned long rng(void)
{
    rng_state = rng_state * 1103515245ul + 12345ul;
    return (rng_state >> 16) & 0x7FFFul;
}

static unsigned long kraft(const unsigned char *lengths, unsigned nsym)
{
    unsigned long sum = 0;
    for (unsigned i = 0; i < nsym; i++)
        if (lengths[i])
            sum += 1ul << (HUFF_MAXLEN - lengths[i]);
    return sum;
}

TEST(lengths_two_symbols)
{
    unsigned long counts[2] = {5, 3};
    unsigned char lengths[2];
    CHECK_INT(huff_lengths(counts, 2, lengths), 0);
    CHECK_INT(lengths[0], 1);
    CHECK_INT(lengths[1], 1);
}

TEST(lengths_single_symbol)
{
    unsigned long counts[256];
    unsigned char lengths[256];
    unsigned short codes[256];
    const unsigned char zero = 0x00, one = 0x80;
    size_t pos = 0;

    memset(counts, 0, sizeof counts);
    counts[10] = 7;
    CHECK_INT(huff_lengths(counts, 256, lengths), 0);
    CHECK_INT(lengths[10], 1);
    for (unsigned i = 0; i < 256; i++)
        if (i != 10) CHECK_INT(lengths[i], 0);
    huff_codes(lengths, 256, codes);
    CHECK_INT(codes[10], 0);
    CHECK_INT(huff_decode_one(lengths, 256, &zero, 1, &pos), 10);
    CHECK_INT((int)pos, 1);
    pos = 0;
    CHECK_INT(huff_decode_one(lengths, 256, &one, 1, &pos), -1);
}

TEST(lengths_match_entropy_order)
{
    unsigned long counts[256];
    unsigned char lengths[256];
    rng_state = 7;
    for (unsigned i = 0; i < 256; i++)
        counts[i] = (i % 5 == 0) ? 0 : rng() % 1000;
    CHECK_INT(huff_lengths(counts, 256, lengths), 0);
    for (unsigned i = 0; i < 256; i++) {
        CHECK((counts[i] == 0) == (lengths[i] == 0));
        for (unsigned j = 0; j < 256; j++)
            if (counts[i] && counts[j] && counts[i] > counts[j])
                CHECK(lengths[i] <= lengths[j]);
    }
    CHECK(kraft(lengths, 256) <= (1ul << HUFF_MAXLEN));
}

TEST(limit_15_on_skewed_counts)
{
    unsigned long counts[21];
    unsigned char lengths[21];
    for (unsigned i = 0; i < 21; i++)
        counts[i] = 1ul << i;
    CHECK_INT(huff_lengths(counts, 21, lengths), 0);
    for (unsigned i = 0; i < 21; i++) {
        CHECK(lengths[i] >= 1);
        CHECK(lengths[i] <= HUFF_MAXLEN);
    }
    CHECK(kraft(lengths, 21) <= (1ul << HUFF_MAXLEN));
}

TEST(rejects_too_many_symbols)
{
    static unsigned long counts[HUFF_MAXSYM + 1];
    static unsigned char lengths[HUFF_MAXSYM + 1];
    CHECK_INT(huff_lengths(counts, HUFF_MAXSYM + 1, lengths), -1);
}

TEST(codes_are_canonical_prefix_free)
{
    unsigned long counts[50];
    unsigned char lengths[50];
    unsigned short codes[50];
    rng_state = 12345;
    for (unsigned i = 0; i < 50; i++)
        counts[i] = 1 + rng() % 5000;
    CHECK_INT(huff_lengths(counts, 50, lengths), 0);
    huff_codes(lengths, 50, codes);
    for (unsigned i = 0; i < 50; i++) {
        CHECK(lengths[i] >= 1 && lengths[i] <= HUFF_MAXLEN);
        CHECK(codes[i] < (1u << lengths[i]));
        for (unsigned j = 0; j < 50; j++) {
            if (i == j)
                continue;
            if (lengths[i] <= lengths[j]) {
                /* i must not be a prefix of j */
                unsigned top = codes[j] >> (lengths[j] - lengths[i]);
                CHECK(top != codes[i]);
            }
            /* left-aligned: shorter codes first, ties by symbol */
            unsigned long li = (unsigned long)codes[i] << (HUFF_MAXLEN - lengths[i]);
            unsigned long lj = (unsigned long)codes[j] << (HUFF_MAXLEN - lengths[j]);
            if (lengths[i] < lengths[j] || (lengths[i] == lengths[j] && i < j))
                CHECK(li < lj);
        }
    }
}

TEST(bitwriter_roundtrip)
{
    enum { NSYM = 511, N = 1000 };
    static unsigned long counts[NSYM];
    static unsigned char lengths[NSYM];
    static unsigned short codes[NSYM];
    static int syms[N];
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    BitWriter w;
    unsigned long total = 0;

    rng_state = 99;
    for (unsigned i = 0; i < NSYM; i++)
        counts[i] = (i % 3 == 0) ? 0 : 1 + rng() % (i < 32 ? 20000 : 50);
    CHECK_INT(huff_lengths(counts, NSYM, lengths), 0);
    huff_codes(lengths, NSYM, codes);

    bw_init(&w, out);
    for (unsigned k = 0; k < N; k++) {
        unsigned s;
        do
            s = (unsigned)(rng() % NSYM);
        while (!lengths[s]);
        syms[k] = (int)s;
        bw_put(&w, codes[s], lengths[s]);
        total += lengths[s];
    }
    bw_flush(&w);
    CHECK_INT(str_len(out), (total + 7) / 8);

    const unsigned char *data = str_bytes(out);
    size_t len = str_len(out);
    size_t pos = 0;
    for (unsigned k = 0; k < N; k++)
        CHECK_INT(huff_decode_one(lengths, NSYM, data, len, &pos), syms[k]);
    CHECK_INT(pos, total);

    /* final symbol cut short: overrun returns -1 */
    size_t cut = 0;
    for (unsigned k = 0; k + 1 < N; k++)
        cut += lengths[syms[k]];
    size_t tlen = cut / 8;
    pos = 0;
    int got = 0;
    for (unsigned k = 0; k < N && got >= 0; k++)
        got = huff_decode_one(lengths, NSYM, data, tlen, &pos);
    CHECK_INT(got, -1);
    arena_free(a);
}

TEST(bitwriter_flush_pads_zero)
{
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    BitWriter w;
    bw_init(&w, out);
    bw_put(&w, 0x5, 3);     /* 101 */
    bw_put(&w, 0x1FF, 9);   /* 111111111 */
    bw_flush(&w);
    bw_flush(&w);
    CHECK_INT(str_len(out), 2);
    CHECK_INT(str_bytes(out)[0], 0xBF);
    CHECK_INT(str_bytes(out)[1], 0xF0);
    arena_free(a);
}

TEST(crc16_known_vector)
{
    CHECK_INT(crc16_ccitt((const unsigned char *)"123456789", 9), 0x29B1);
    CHECK_INT(crc16_ccitt((const unsigned char *)"", 0), 0xFFFF);
}

int main(void)
{
    RUN(lengths_two_symbols);
    RUN(lengths_single_symbol);
    RUN(lengths_match_entropy_order);
    RUN(limit_15_on_skewed_counts);
    RUN(rejects_too_many_symbols);
    RUN(codes_are_canonical_prefix_free);
    RUN(bitwriter_roundtrip);
    RUN(bitwriter_flush_pads_zero);
    RUN(crc16_known_vector);
    return test_summary("huff");
}
