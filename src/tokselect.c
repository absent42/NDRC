/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/tokselect.c - per-game compression token selection (-auto-tokens).
   Copyright (C) 2026 Dan Gibson. */
#include "tokselect.h"
#include <string.h>

/* First-byte buckets over a token list; entries keep their vec order. */
typedef struct {
    Vec_Str *by_first[256];
} TokIndex;

static void tokindex_build(Arena *a, TokIndex *ix, const Vec_Str *tokens)
{
    size_t i;

    memset(ix->by_first, 0, sizeof ix->by_first);
    for (i = 0; tokens != NULL && i < vec_len_Str(tokens); i++) {
        Str *t = vec_at_Str(tokens, i);
        unsigned char f;
        if (str_len(t) == 0) continue;
        f = str_bytes(t)[0];
        if (ix->by_first[f] == NULL) ix->by_first[f] = vec_new_Str(a);
        vec_push_Str(ix->by_first[f], t);
    }
}

/* dp[i] = cheapest encoding of s[i..n); dp sized n+1 by the caller. */
static long parse_one(const unsigned char *s, size_t n,
                      const TokIndex *ix, long *dp)
{
    size_t i;

    dp[n] = 0;
    for (i = n; i-- > 0; ) {
        long best = dp[i + 1] + 1;
        const Vec_Str *bucket = ix->by_first[s[i]];
        size_t j;
        for (j = 0; bucket != NULL && j < vec_len_Str(bucket); j++) {
            Str *t = vec_at_Str(bucket, j);
            size_t tl = str_len(t);
            if (tl <= n - i && memcmp(s + i, str_bytes(t), tl) == 0) {
                long v = dp[i + tl] + 1;
                if (v < best) best = v;
            }
        }
        dp[i] = best;
    }
    return dp[0];
}

static long *dp_scratch(Arena *a, const Vec_Str *strings)
{
    size_t i, maxlen = 0;

    for (i = 0; i < vec_len_Str(strings); i++) {
        size_t l = str_len(vec_at_Str(strings, i));
        if (l > maxlen) maxlen = l;
    }
    return arena_alloc(a, (maxlen + 1) * sizeof(long));
}

long tokselect_parse_total(Arena *a, const Vec_Str *strings,
                           const Vec_Str *tokens)
{
    TokIndex ix;
    long *dp = dp_scratch(a, strings);
    long total = 0;
    size_t i;

    tokindex_build(a, &ix, tokens);
    for (i = 0; i < vec_len_Str(strings); i++) {
        Str *s = vec_at_Str(strings, i);
        total += parse_one(str_bytes(s), str_len(s), &ix, dp);
    }
    return total;
}
