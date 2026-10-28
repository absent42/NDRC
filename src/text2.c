/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/text2.c - NextDAAD text format 2: Huffman-costed selection,
   one-level entry nesting, text block serialisation.
   Copyright (C) 2026 Dan Gibson. */
#include "text2.h"
#include "tokselect.h"
#include <string.h>

#define T2_MAXLEN 12      /* raw candidate cap */
#define T2_UNUSED_LIT 12  /* round 2 cost of a literal unused in round 1 */
#define T2_TAB_BITS 8
#define T2_EOM 10
#define T2_REFMAX 128     /* entry refs reach tokens 0-127 */

/* Token indices bucketed by first byte, token order kept in a bucket. */
typedef struct { size_t off[257]; size_t *idx; } Buckets;

static void buckets_build(Arena *a, Buckets *bk, const Vec_Str *t)
{
    size_t n = vec_len_Str(t), i, fill[256];

    memset(bk->off, 0, sizeof bk->off);
    for (i = 0; i < n; i++) bk->off[str_bytes(vec_at_Str(t, i))[0] + 1]++;
    for (i = 1; i <= 256; i++) bk->off[i] += bk->off[i - 1];
    memcpy(fill, bk->off, sizeof fill);
    bk->idx = arena_alloc(a, (n + 1) * sizeof(*bk->idx));
    for (i = 0; i < n; i++) bk->idx[fill[str_bytes(vec_at_Str(t, i))[0]]++] = i;
}

/* Per-string symbol sequences, EOM included. */
typedef struct { unsigned short **sym; size_t *n; size_t count; } Stream;

/* Costed optimal parse of every string; ties keep the literal, then the
   first token in bucket order. */
static Stream *encode(Arena *a, const Vec_Str *strings, const Vec_Str *tokens,
                      const int lit[256], long ref)
{
    Stream *st = arena_calloc(a, sizeof(*st));
    Buckets bk;
    size_t i, maxlen = 0;
    long *dp, *pick;

    buckets_build(a, &bk, tokens);
    st->count = vec_len_Str(strings);
    st->sym = arena_alloc(a, (st->count + 1) * sizeof(*st->sym));
    st->n = arena_alloc(a, (st->count + 1) * sizeof(*st->n));
    for (i = 0; i < st->count; i++)
        if (str_len(vec_at_Str(strings, i)) > maxlen)
            maxlen = str_len(vec_at_Str(strings, i));
    dp = arena_alloc(a, (maxlen + 1) * sizeof(*dp));
    pick = arena_alloc(a, (maxlen + 1) * sizeof(*pick));
    for (i = 0; i < st->count; i++) {
        Str *s = vec_at_Str(strings, i);
        const unsigned char *p = str_bytes(s);
        size_t n = str_len(s), pos, m = 0;
        long ii;
        dp[n] = 0;
        for (ii = (long)n - 1; ii >= 0; ii--) {
            long best = dp[ii + 1] + lit[p[ii]], bp = -1;
            size_t j;
            for (j = bk.off[p[ii]]; j < bk.off[p[ii] + 1]; j++) {
                Str *t = vec_at_Str(tokens, bk.idx[j]);
                size_t tl = str_len(t);
                if (tl <= n - (size_t)ii &&
                    memcmp(p + ii, str_bytes(t), tl) == 0 &&
                    dp[ii + (long)tl] + ref < best) {
                    best = dp[ii + (long)tl] + ref;
                    bp = (long)bk.idx[j];
                }
            }
            dp[ii] = best;
            pick[ii] = bp;
        }
        st->sym[i] = arena_alloc(a, (n + 1) * sizeof(**st->sym));
        for (pos = 0; pos < n; ) {
            if (pick[pos] < 0) {
                st->sym[i][m++] = p[pos++];
            } else {
                st->sym[i][m++] = (unsigned short)(128 + pick[pos]);
                pos += str_len(vec_at_Str(tokens, (size_t)pick[pos]));
            }
        }
        st->sym[i][m++] = T2_EOM;
        st->n[i] = m;
    }
    return st;
}

static void stream_lengths(const Stream *st, unsigned nsym,
                           unsigned char *lengths)
{
    unsigned long counts[HUFF_MAXSYM];
    size_t i, k;

    memset(counts, 0, sizeof counts);
    for (i = 0; i < st->count; i++)
        for (k = 0; k < st->n[i]; k++) counts[st->sym[i][k]]++;
    huff_lengths(counts, nsym, lengths);
}

Vec_Str *text2_nest(Arena *a, const Vec_Str *tokens, const long *uses,
                    size_t *perm)
{
    size_t n = vec_len_Str(tokens), i, q, npinned = 0, nlow, taken = 0;
    size_t *order = arena_alloc(a, (n + 1) * sizeof(*order));
    size_t *inv = arena_alloc(a, (n + 1) * sizeof(*inv));
    size_t *low = arena_alloc(a, (n + 1) * sizeof(*low)), nl = 0, nh = 0;
    size_t *high = arena_alloc(a, (n + 1) * sizeof(*high));
    char *pinned = arena_calloc(a, n + 1), *nested = arena_calloc(a, n + 1);
    /* body[i]: < 128 literal, else 256 + old target index */
    unsigned short **body = arena_calloc(a, (n + 1) * sizeof(*body));
    size_t *blen = arena_calloc(a, (n + 1) * sizeof(*blen));
    Vec_Str *out = vec_new_Str(a);

    /* Longest first, stable: a nested entry's targets are all shorter,
       hence still raw, and get pinned raw for good. */
    for (i = 0; i < n; i++) {
        size_t j = i, key = i;
        while (j > 0 && str_len(vec_at_Str(tokens, order[j - 1])) <
                            str_len(vec_at_Str(tokens, key))) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
    for (q = 0; q < n; q++) {
        size_t ti = order[q], L = str_len(vec_at_Str(tokens, ti)), p, j;
        const unsigned char *s = str_bytes(vec_at_Str(tokens, ti));
        int allow_new = npinned + L <= T2_REFMAX;
        long dp[257], ch[257];
        if (pinned[ti] || L > 256) continue;
        dp[L] = 0;
        for (p = L; p-- > 0; ) {
            dp[p] = dp[p + 1] + 1;
            ch[p] = -1;
            for (j = 0; j < n; j++) {
                Str *t = vec_at_Str(tokens, j);
                size_t tl = str_len(t);
                if (tl >= L || nested[j] || (!pinned[j] && !allow_new) ||
                    tl > L - p || memcmp(s + p, str_bytes(t), tl) != 0)
                    continue;
                if (dp[p + tl] + 1 < dp[p]) { dp[p] = dp[p + tl] + 1; ch[p] = (long)j; }
            }
        }
        if (dp[0] >= (long)L) continue;
        body[ti] = arena_alloc(a, L * sizeof(**body));
        for (p = 0; p < L; ) {
            if (ch[p] < 0) {
                body[ti][blen[ti]++] = s[p++];
            } else {
                size_t tj = (size_t)ch[p];
                body[ti][blen[ti]++] = (unsigned short)(256 + tj);
                if (!pinned[tj]) { pinned[tj] = 1; npinned++; }
                p += str_len(vec_at_Str(tokens, tj));
            }
        }
        nested[ti] = 1;
    }

    /* Most used first, stable; slots 0-127 hold every pinned token plus
       the most used of the rest. */
    for (i = 0; i < n; i++) {
        size_t j = i, key = i;
        while (j > 0 && uses[order[j - 1]] < uses[key]) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
    nlow = n < T2_REFMAX ? n : T2_REFMAX;
    for (i = 0; i < n; i++) {
        size_t t = order[i];
        if (pinned[t] || taken < nlow - npinned) {
            if (!pinned[t]) taken++;
            low[nl++] = t;
        } else {
            high[nh++] = t;
        }
    }
    for (i = 0; i < nl; i++) perm[i] = low[i];
    for (i = 0; i < nh; i++) perm[nl + i] = high[i];
    for (i = 0; i < n; i++) inv[perm[i]] = i;

    for (i = 0; i < n; i++) {
        size_t t = perm[i], k;
        Str *e = str_new(a), *raw = vec_at_Str(tokens, t);
        if (!nested[t]) {
            str_push_u8(e, (unsigned)str_len(raw));
            str_append_n(e, str_bytes(raw), str_len(raw));
        } else {
            str_push_u8(e, (unsigned)blen[t]);
            for (k = 0; k < blen[t]; k++) {
                unsigned v = body[t][k];
                str_push_u8(e, v < 256 ? v : (unsigned)(128 + inv[v - 256]));
            }
        }
        vec_push_Str(out, e);
    }
    return out;
}

static Text2Result *build_one(Arena *a, Diag *d, const Vec_Str *strings,
                              unsigned maxtok)
{
    Text2Result *r = arena_calloc(a, sizeof(*r));
    unsigned nsym = maxtok > 128 ? 511 : 256;
    int unit[256], lit2[256];
    unsigned char len1[HUFF_MAXSYM];
    unsigned short codes[HUFF_MAXSYM];
    long ref2 = 0, cnt = 0, *uses;
    size_t i, k, ntok, live = 0, *perm, *keep, *inv;
    Vec_Str *t1, *t2, *t3;
    Stream *st;

    for (i = 0; i < 256; i++) unit[i] = 1;
    /* Round 1: unit costs, lengths over its stream. */
    t1 = tokselect_select_costed(a, d, strings, vec_len_Str(strings), maxtok,
                                 unit, 1, 1, T2_MAXLEN);
    st = encode(a, strings, t1, unit, 1);
    memset(len1, 0, sizeof len1);
    stream_lengths(st, nsym, len1);

    /* Round 2: costs in bits from round 1. */
    for (i = 0; i < 256; i++)
        lit2[i] = i < 128 && len1[i] ? len1[i] : T2_UNUSED_LIT;
    for (k = 0; k < vec_len_Str(t1); k++)
        if (len1[128 + k]) { ref2 += len1[128 + k]; cnt++; }
    ref2 = cnt ? (ref2 + cnt / 2) / cnt : T2_UNUSED_LIT;
    t2 = tokselect_select_costed(a, d, strings, vec_len_Str(strings), maxtok,
                                 lit2, (int)ref2, T2_TAB_BITS, T2_MAXLEN);
    st = encode(a, strings, t2, lit2, ref2);

    /* Drop tokens the round 2 stream never uses. */
    ntok = vec_len_Str(t2);
    uses = arena_calloc(a, (ntok + 1) * sizeof(*uses));
    for (i = 0; i < st->count; i++)
        for (k = 0; k < st->n[i]; k++)
            if (st->sym[i][k] >= 128) uses[st->sym[i][k] - 128]++;
    keep = arena_alloc(a, (ntok + 1) * sizeof(*keep));
    t3 = vec_new_Str(a);
    for (k = 0; k < ntok; k++) {
        keep[k] = live;
        if (uses[k] > 0) {
            uses[live++] = uses[k];
            vec_push_Str(t3, vec_at_Str(t2, k));
        }
    }

    perm = arena_alloc(a, (live + 1) * sizeof(*perm));
    inv = arena_alloc(a, (live + 1) * sizeof(*inv));
    r->entries = text2_nest(a, t3, uses, perm);
    r->tokens = vec_new_Str(a);
    for (k = 0; k < live; k++) {
        inv[perm[k]] = k;
        vec_push_Str(r->tokens, vec_at_Str(t3, perm[k]));
    }
    for (i = 0; i < st->count; i++)
        for (k = 0; k < st->n[i]; k++)
            if (st->sym[i][k] >= 128)
                st->sym[i][k] = (unsigned short)(128 +
                                    inv[keep[st->sym[i][k] - 128]]);

    /* Final code over the renumbered round 2 stream. */
    r->alphabet383 = maxtok > 128;
    r->nsym = nsym;
    stream_lengths(st, nsym, r->lengths);
    huff_codes(r->lengths, nsym, codes);
    r->coded = vec_new_Str(a);
    for (i = 0; i < st->count; i++) {
        Str *c = str_new(a);
        BitWriter w;
        bw_init(&w, c);
        for (k = 0; k < st->n[i]; k++)
            bw_put(&w, codes[st->sym[i][k]], r->lengths[st->sym[i][k]]);
        bw_flush(&w);
        r->text_bytes += (long)str_len(c);
        vec_push_Str(r->coded, c);
    }
    r->block_bytes = 7 + (nsym == 256 ? 128 : 256) + 2 * (long)live;
    for (k = 0; k < live; k++)
        r->block_bytes += (long)str_len(vec_at_Str(r->entries, k));
    return r;
}

/* Coded bytes with each equal stream of a group counted once. */
static long unique_bytes(const Vec_Str *coded, const int *group)
{
    long n = 0;
    size_t i, j;

    for (i = 0; i < vec_len_Str(coded); i++) {
        const Str *c = vec_at_Str(coded, i);
        for (j = 0; group != NULL && group[i] >= 0 && j < i; j++) {
            const Str *p = vec_at_Str(coded, j);
            if (group[j] == group[i] && str_len(p) == str_len(c) &&
                memcmp(str_bytes(p), str_bytes(c), str_len(c)) == 0)
                break;
        }
        if (group == NULL || group[i] < 0 || j == i) n += (long)str_len(c);
    }
    return n;
}

Text2Result *text2_build(Arena *a, Diag *d, const Vec_Str *strings,
                         const int *group)
{
    Text2Result *r128, *r383;
    size_t i, k;

    for (i = 0; i < vec_len_Str(strings); i++) {
        Str *s = vec_at_Str(strings, i);
        for (k = 0; k < str_len(s); k++)
            if (str_bytes(s)[k] >= 128) {
                diag_fatal(d, "Text format 2: text byte %u above 127.",
                           (unsigned)str_bytes(s)[k]);
                return NULL;
            }
    }
    r128 = build_one(a, d, strings, 128);
    r383 = build_one(a, d, strings, 383);
    r128->unique_bytes = unique_bytes(r128->coded, group);
    r383->unique_bytes = unique_bytes(r383->coded, group);
    return r383->unique_bytes + r383->block_bytes <
           r128->unique_bytes + r128->block_bytes ? r383 : r128;
}

Str *text2_block(Arena *a, const Text2Result *r)
{
    Str *b = str_new(a);
    size_t ntok = vec_len_Str(r->entries), lbytes = r->nsym > 256 ? 256 : 128;
    size_t i, off;
    unsigned char packed[256];

    memset(packed, 0, sizeof packed);
    for (i = 0; i < r->nsym; i++)
        packed[i / 2] |= (unsigned char)((r->lengths[i] & 15) << (i % 2 ? 4 : 0));
    str_append_n(b, "H2", 2);
    str_push_u8(b, r->alphabet383 ? 1 : 0);
    str_push_u16le(b, (unsigned)ntok);
    str_push_u16le(b, crc16_ccitt(packed, lbytes));
    str_append_n(b, packed, lbytes);
    off = 7 + lbytes + 2 * ntok;
    for (i = 0; i < ntok; i++) {
        str_push_u16le(b, (unsigned)off);
        off += str_len(vec_at_Str(r->entries, i));
    }
    for (i = 0; i < ntok; i++) {
        Str *e = vec_at_Str(r->entries, i);
        str_append_n(b, str_bytes(e), str_len(e));
    }
    return b;
}
