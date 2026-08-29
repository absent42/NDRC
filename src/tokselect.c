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

#define TOKSEL_MINLEN 2
#define TOKSEL_MAXLEN 12
#define TOKSEL_MAXTOK 128
#define TOKSEL_EVAL_BUDGET 500

/* A candidate substring. ptr aliases the corpus Str it was first seen
   in; selection completes before tokens_compress replaces any Text. */
typedef struct {
    const unsigned char *ptr;
    unsigned len;
    long count;      /* overlap-inflated occurrence count */
    long idx;        /* first-occurrence order; the deterministic tie-break */
    int taken;       /* already chosen; lazily discarded on pop */
} Cand;
VEC_DECLARE(Cand, Cand *)

/* Open-addressing candidate map, FNV-1a, power-of-two capacity.
   Iteration order never reaches any decision: ordering lives in the
   Vec_Cand insert-order list and in Cand.idx. */
typedef struct {
    Cand **slots;
    size_t cap, used;
} CandMap;

static size_t cand_hash(const unsigned char *p, size_t n)
{
    size_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void candmap_grow(Arena *a, CandMap *m)
{
    size_t newcap = m->cap * 2;
    Cand **ns = arena_calloc(a, newcap * sizeof(*ns));
    size_t i;

    for (i = 0; i < m->cap; i++) {
        Cand *c = m->slots[i];
        size_t s;
        if (c == NULL) continue;
        s = cand_hash(c->ptr, c->len) & (newcap - 1);
        while (ns[s] != NULL) s = (s + 1) & (newcap - 1);
        ns[s] = c;
    }
    m->slots = ns;
    m->cap = newcap;
}

static Cand *candmap_get_or_add(Arena *a, CandMap *m, Vec_Cand *order,
                                const unsigned char *p, size_t n)
{
    size_t s;
    Cand *c;

    if ((m->used + 1) * 2 >= m->cap) candmap_grow(a, m);
    s = cand_hash(p, n) & (m->cap - 1);
    while ((c = m->slots[s]) != NULL) {
        if (c->len == n && memcmp(c->ptr, p, n) == 0) return c;
        s = (s + 1) & (m->cap - 1);
    }
    c = arena_calloc(a, sizeof(*c));
    c->ptr = p;
    c->len = (unsigned)n;
    c->idx = (long)vec_len_Cand(order);
    m->slots[s] = c;
    m->used++;
    vec_push_Cand(order, c);
    return c;
}

/* Max-heap of (bound, cand idx); ties to the lower idx. */
typedef struct { long bound; long idx; } HeapEnt;
typedef struct { HeapEnt *e; size_t n, cap; } Heap;

static int heap_before(const HeapEnt *x, const HeapEnt *y)
{
    if (x->bound != y->bound) return x->bound > y->bound;
    return x->idx < y->idx;
}

static void heap_push(Arena *a, Heap *h, long bound, long idx)
{
    size_t i;

    if (h->n == h->cap) {
        size_t nc = h->cap == 0 ? 1024 : h->cap * 2;
        HeapEnt *ne = arena_alloc(a, nc * sizeof(*ne));
        /* h->e is NULL on the first push; memcpy(dst, NULL, 0) is UB
           and trips the sanitize gate. */
        if (h->n > 0) memcpy(ne, h->e, h->n * sizeof(*ne));
        h->e = ne;
        h->cap = nc;
    }
    i = h->n++;
    h->e[i].bound = bound;
    h->e[i].idx = idx;
    while (i > 0 && heap_before(&h->e[i], &h->e[(i - 1) / 2])) {
        HeapEnt t = h->e[i];
        h->e[i] = h->e[(i - 1) / 2];
        h->e[(i - 1) / 2] = t;
        i = (i - 1) / 2;
    }
}

static HeapEnt heap_pop(Heap *h)
{
    HeapEnt top = h->e[0];
    size_t i = 0;

    h->e[0] = h->e[--h->n];
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, b = i;
        if (l < h->n && heap_before(&h->e[l], &h->e[b])) b = l;
        if (r < h->n && heap_before(&h->e[r], &h->e[b])) b = r;
        if (b == i) break;
        { HeapEnt t = h->e[i]; h->e[i] = h->e[b]; h->e[b] = t; }
        i = b;
    }
    return top;
}

/* The compressable tables, in the fixed "advanced" order. Mirrors
   tokens.c's get_compressable_tables for that mode. */
static void compressable_tables(const Adventure *adv,
                                const Vec_Message *out[4])
{
    out[0] = adv->locations;
    out[1] = adv->messages;
    out[2] = adv->sysmess;
    out[3] = adv->xmessages;
}

static Vec_Str *corpus_strings(Arena *a, const Adventure *adv)
{
    const Vec_Message *tabs[4];
    Vec_Str *out = vec_new_Str(a);
    size_t t, k;

    compressable_tables(adv, tabs);
    for (t = 0; t < 4; t++)
        for (k = 0; k < vec_len_Message(tabs[t]); k++)
            vec_push_Str(out, vec_at_Message(tabs[t], k)->Text);
    return out;
}

static int is_placeholder(unsigned char b)
{
    return b == '_' || b == '@';
}

static int bytes_contain(const unsigned char *h, size_t hn,
                         const unsigned char *n, size_t nn)
{
    size_t i;

    if (nn > hn) return 0;
    for (i = 0; i + nn <= hn; i++)
        if (memcmp(h + i, n, nn) == 0) return 1;
    return 0;
}

/* Net gain of adding cand to chosen: optimal reparse of only the
   strings containing it, against cached lengths, minus table cost.
   This is the hot allocator (up to 500 evals x 128 iterations); the
   trial vec and TokIndex buckets live in a per-eval scratch arena so
   peak memory stays flat. Only pointers into the caller's arena are
   read; nothing from scratch escapes. */
static long eval_gain(const Vec_Str *strings, const long *cache,
                      const Vec_Str *chosen, const Cand *c, long *dp)
{
    Arena *scratch = arena_new(0);
    Vec_Str *trial = vec_new_Str(scratch);
    Str *cs = str_new(scratch);
    TokIndex ix;
    long gain = 0;
    size_t i;

    for (i = 0; i < vec_len_Str(chosen); i++)
        vec_push_Str(trial, vec_at_Str(chosen, i));
    str_append_n(cs, c->ptr, c->len);
    vec_push_Str(trial, cs);
    tokindex_build(scratch, &ix, trial);
    for (i = 0; i < vec_len_Str(strings); i++) {
        Str *s = vec_at_Str(strings, i);
        if (!bytes_contain(str_bytes(s), str_len(s), c->ptr, c->len))
            continue;
        gain += cache[i] - parse_one(str_bytes(s), str_len(s), &ix, dp);
    }
    arena_free(scratch);
    return gain - (long)c->len;
}

static Vec_Str *vec_without(Arena *a, const Vec_Str *v, size_t skip)
{
    Vec_Str *out = vec_new_Str(a);
    size_t i;

    for (i = 0; i < vec_len_Str(v); i++)
        if (i != skip) vec_push_Str(out, vec_at_Str(v, i));
    return out;
}

TokenSet *tokselect_run(Arena *a, Diag *d, const Adventure *adv,
                        int exclude_placeholders)
{
    Vec_Str *strings = corpus_strings(a, adv);
    Vec_Cand *order = vec_new_Cand(a);
    CandMap map;
    Heap heap = {0};
    Vec_Str *chosen = vec_new_Str(a);
    long *cache, *dp, *fresh_it, *fresh_gain;
    size_t i;
    long it = 0;
    TokenSet *ts;

    map.cap = 1 << 12;
    map.used = 0;
    map.slots = arena_calloc(a, map.cap * sizeof(*map.slots));

    /* Candidate extraction: strings in table order, positions
       ascending, lengths ascending - Cand.idx IS the determinism. */
    for (i = 0; i < vec_len_Str(strings); i++) {
        Str *s = vec_at_Str(strings, i);
        const unsigned char *p = str_bytes(s);
        size_t n = str_len(s), pos;
        for (pos = 0; pos + TOKSEL_MINLEN <= n; pos++) {
            size_t maxl = n - pos, L;
            if (maxl > TOKSEL_MAXLEN) maxl = TOKSEL_MAXLEN;
            if (exclude_placeholders && is_placeholder(p[pos])) continue;
            for (L = TOKSEL_MINLEN; L <= maxl; L++) {
                if (exclude_placeholders && is_placeholder(p[pos + L - 1]))
                    break;
                candmap_get_or_add(a, &map, order, p + pos, L)->count++;
            }
        }
    }

    for (i = 0; i < vec_len_Cand(order); i++) {
        Cand *c = vec_at_Cand(order, i);
        /* count*(len-1) fits long everywhere: overflow would need a
           ~190MB text corpus; DDB text is bounded far below 64K. */
        long proxy = c->count * ((long)c->len - 1) - (long)c->len;
        if (c->count >= 2 && proxy > 0) heap_push(a, &heap, proxy, c->idx);
    }

    cache = arena_alloc(a, (vec_len_Str(strings) + 1) * sizeof(long));
    for (i = 0; i < vec_len_Str(strings); i++)
        cache[i] = (long)str_len(vec_at_Str(strings, i));
    dp = dp_scratch(a, strings);
    fresh_it = arena_calloc(a, (vec_len_Cand(order) + 1) * sizeof(long));
    fresh_gain = arena_calloc(a, (vec_len_Cand(order) + 1) * sizeof(long));

    while (vec_len_Str(chosen) < TOKSEL_MAXTOK && heap.n > 0) {
        long evals = 0;
        long best_eval_gain = 0, best_eval_idx = -1;
        Cand *pick = NULL;
        long pick_gain = 0;

        it++;
        while (heap.n > 0) {
            HeapEnt e = heap_pop(&heap);
            Cand *c = vec_at_Cand(order, (size_t)e.idx);
            long net;
            if (c->taken) continue;
            if (fresh_it[e.idx] == it) {
                pick = c;
                pick_gain = e.bound;
                break;
            }
            evals++;
            net = eval_gain(strings, cache, chosen, c, dp);
            if (net > 0) {
                fresh_it[e.idx] = it;
                fresh_gain[e.idx] = net;
                heap_push(a, &heap, net, e.idx);
                if (best_eval_idx < 0 || net > best_eval_gain) {
                    best_eval_gain = net;
                    best_eval_idx = e.idx;
                }
            }
            if (evals >= TOKSEL_EVAL_BUDGET) break;
        }
        /* Budget exhausted before a fresh top: take the best seen. */
        if (pick == NULL && best_eval_idx >= 0) {
            pick = vec_at_Cand(order, (size_t)best_eval_idx);
            pick_gain = fresh_gain[best_eval_idx];
        }
        if (pick == NULL || pick_gain <= 0) break;

        {
            Str *tok = str_new(a);
            TokIndex ix;
            str_append_n(tok, pick->ptr, pick->len);
            vec_push_Str(chosen, tok);
            pick->taken = 1;
            tokindex_build(a, &ix, chosen);
            for (i = 0; i < vec_len_Str(strings); i++) {
                Str *s = vec_at_Str(strings, i);
                if (bytes_contain(str_bytes(s), str_len(s),
                                  pick->ptr, pick->len))
                    cache[i] = parse_one(str_bytes(s), str_len(s), &ix, dp);
            }
        }
    }

    /* Prune: 2 sweeps, last to first; drop tokens whose removal nets
       positive (table bytes saved beat text growth). */
    {
        long cur = 0;
        int sweep;
        for (i = 0; i < vec_len_Str(strings); i++) cur += cache[i];
        for (sweep = 0; sweep < 2; sweep++) {
            int removed = 0;
            long j;
            for (j = (long)vec_len_Str(chosen) - 1; j >= 0; j--) {
                Vec_Str *trial = vec_without(a, chosen, (size_t)j);
                long tl = (long)str_len(vec_at_Str(chosen, (size_t)j));
                long tot = tokselect_parse_total(a, strings, trial);
                if (tot - cur < tl) {
                    chosen = trial;
                    cur = tot;
                    removed = 1;
                }
            }
            if (!removed) break;
        }
    }

    /* Longest-first, stable (selection order kept within a length):
       insertion sort - qsort is unstable and platform-varying. */
    for (i = 1; i < vec_len_Str(chosen); i++) {
        Str *k = vec_at_Str(chosen, i);
        long j = (long)i - 1;
        while (j >= 0 &&
               str_len(vec_at_Str(chosen, (size_t)j)) < str_len(k)) {
            vec_set_Str(chosen, (size_t)j + 1, vec_at_Str(chosen, (size_t)j));
            j--;
        }
        vec_set_Str(chosen, (size_t)j + 1, k);
    }

    ts = arena_calloc(a, sizeof(*ts));
    ts->compression = "advanced";
    ts->advanced = 1;
    ts->has_tokens = 1;
    ts->optimal_encode = 1;
    ts->tokens = vec_new_Str(a);
    {
        Str *zero = str_new(a);
        str_push_u8(zero, 0);
        vec_push_Str(ts->tokens, zero);
    }
    for (i = 0; i < vec_len_Str(chosen); i++)
        vec_push_Str(ts->tokens, vec_at_Str(chosen, i));

    diag_verbose(d, "Auto-tokens: %lu candidates, %lu selected.",
                 (unsigned long)vec_len_Cand(order),
                 (unsigned long)vec_len_Str(chosen));
    return ts;
}

Vec_MsgTable *tokselect_snapshot(Arena *a, const Adventure *adv)
{
    const Vec_Message *tabs[4];
    Vec_MsgTable *out = vec_new_MsgTable(a);
    size_t t, k;

    compressable_tables(adv, tabs);
    for (t = 0; t < 4; t++) {
        Vec_Message *copy = vec_new_Message(a);
        for (k = 0; k < vec_len_Message(tabs[t]); k++) {
            Message *src = vec_at_Message(tabs[t], k);
            Message *m = arena_calloc(a, sizeof(*m));
            m->Text = str_new(a);
            str_append_n(m->Text, str_bytes(src->Text), str_len(src->Text));
            vec_push_Message(copy, m);
        }
        vec_push_MsgTable(out, copy);
    }
    return out;
}

int tokselect_verify(const Vec_MsgTable *before, const Adventure *adv,
                     const Vec_Str *final_tokens)
{
    const Vec_Message *tabs[4];
    Arena *scratch = arena_new(0);
    size_t t, k, i;
    int ok = 1;

    compressable_tables(adv, tabs);
    for (t = 0; ok && t < 4; t++) {
        const Vec_Message *now = tabs[t];
        Vec_Message *was = vec_at_MsgTable(before, t);
        if (vec_len_Message(now) != vec_len_Message(was)) { ok = 0; break; }
        for (k = 0; ok && k < vec_len_Message(now); k++) {
            Str *enc = vec_at_Message(now, k)->Text;
            Str *ref = vec_at_Message(was, k)->Text;
            Str *dec = str_new(scratch);
            const unsigned char *b = str_bytes(enc);
            /* Byte 127 (token 0's delimiter) falls through as a
               literal: token 0 is 0x00 and never matches valid DAAD
               text, so 127 is unreachable in compressed output. */
            for (i = 0; i < str_len(enc); i++) {
                if (b[i] >= 128) {
                    size_t j = (size_t)b[i] - 127;   /* pass-2 delim j+127 */
                    Str *tok;
                    if (j >= vec_len_Str(final_tokens)) { ok = 0; break; }
                    tok = vec_at_Str(final_tokens, j);
                    str_append_n(dec, str_bytes(tok), str_len(tok));
                } else {
                    str_push(dec, (char)b[i]);
                }
            }
            if (ok && (str_len(dec) != str_len(ref) ||
                       memcmp(str_bytes(dec), str_bytes(ref),
                              str_len(ref)) != 0)) ok = 0;
        }
    }
    arena_free(scratch);
    return ok;
}
