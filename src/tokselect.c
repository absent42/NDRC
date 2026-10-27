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

size_t tokselect_protected_prefix(const unsigned char *s, size_t n)
{
    const unsigned char *sp = n > 0 ? memchr(s, ' ', n) : NULL;
    size_t i;

    if (sp == NULL) return n;
    for (i = (size_t)(sp - s); i < n && s[i] == ' '; i++) ;
    return i;
}

/* Classic interpreters scan stored object bytes for the first space
   and reject a stored '.', so tokens must not hide either. */
static int token_has_dot(const Str *t)
{
    return memchr(str_bytes(t), '.', str_len(t)) != NULL;
}

/* dp[i] = cheapest encoding of s[i..n); dp sized n+1 by the caller.
   obj_rule: classic object text - literal prefix, no '.' tokens. */
static long parse_one(const unsigned char *s, size_t n,
                      const TokIndex *ix, long *dp, int obj_rule)
{
    size_t i, prot = obj_rule ? tokselect_protected_prefix(s, n) : 0;

    dp[n] = 0;
    for (i = n; i-- > 0; ) {
        long best = dp[i + 1] + 1;
        const Vec_Str *bucket = i < prot ? NULL : ix->by_first[s[i]];
        size_t j;
        for (j = 0; bucket != NULL && j < vec_len_Str(bucket); j++) {
            Str *t = vec_at_Str(bucket, j);
            size_t tl = str_len(t);
            if (obj_rule && token_has_dot(t)) continue;
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

/* strings[obj_from..] are classic object texts (obj_from = length
   when the rule is off). */
static long parse_total(Arena *a, const Vec_Str *strings,
                        const Vec_Str *tokens, size_t obj_from)
{
    TokIndex ix;
    long *dp = dp_scratch(a, strings);
    long total = 0;
    size_t i;

    tokindex_build(a, &ix, tokens);
    for (i = 0; i < vec_len_Str(strings); i++) {
        Str *s = vec_at_Str(strings, i);
        total += parse_one(str_bytes(s), str_len(s), &ix, dp, i >= obj_from);
    }
    return total;
}

long tokselect_parse_total(Arena *a, const Vec_Str *strings,
                           const Vec_Str *tokens)
{
    return parse_total(a, strings, tokens, vec_len_Str(strings));
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

/* The compressable tables: slots 0-3 mirror tokens.c's
   get_compressable_tables "advanced" order; slot 4 is objects when
   with_objects, else NULL (callers skip NULL slots). */
static void compressable_tables(const Adventure *adv, int with_objects,
                                const Vec_Message *out[TOKSEL_TABLES])
{
    out[0] = adv->locations;
    out[1] = adv->messages;
    out[2] = adv->sysmess;
    out[3] = adv->xmessages;
    out[4] = with_objects ? adv->objects : NULL;
}

static Vec_Str *corpus_strings(Arena *a, const Adventure *adv,
                               int with_objects)
{
    const Vec_Message *tabs[TOKSEL_TABLES];
    Vec_Str *out = vec_new_Str(a);
    size_t t, k;

    compressable_tables(adv, with_objects, tabs);
    for (t = 0; t < TOKSEL_TABLES; t++)
        for (k = 0; tabs[t] != NULL && k < vec_len_Message(tabs[t]); k++)
            vec_push_Str(out, vec_at_Message(tabs[t], k)->Text);
    return out;
}

/* Index of the first classic object text in corpus_strings' output:
   objects are the last slot, so they are its tail. */
static size_t objects_from(const Vec_Str *strings, const Adventure *adv,
                           int with_objects, int classic_objects)
{
    size_t n = vec_len_Str(strings);

    if (!with_objects || !classic_objects || adv->objects == NULL) return n;
    return n - vec_len_Message(adv->objects);
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
                      const Vec_Str *chosen, const Cand *c, long *dp,
                      size_t obj_from)
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
        gain += cache[i] - parse_one(str_bytes(s), str_len(s), &ix, dp,
                                     i >= obj_from);
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
                        int exclude_placeholders, int with_objects,
                        int classic_objects)
{
    Vec_Str *strings = corpus_strings(a, adv, with_objects);
    size_t obj_from = objects_from(strings, adv, with_objects,
                                   classic_objects);
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
        int obj_rule = i >= obj_from;
        pos = obj_rule ? tokselect_protected_prefix(p, n) : 0;
        for (; pos + TOKSEL_MINLEN <= n; pos++) {
            size_t maxl = n - pos, L;
            if (maxl > TOKSEL_MAXLEN) maxl = TOKSEL_MAXLEN;
            if (exclude_placeholders && is_placeholder(p[pos])) continue;
            if (obj_rule && p[pos] == '.') continue;
            for (L = TOKSEL_MINLEN; L <= maxl; L++) {
                if (exclude_placeholders && is_placeholder(p[pos + L - 1]))
                    break;
                if (obj_rule && p[pos + L - 1] == '.') break;
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
            net = eval_gain(strings, cache, chosen, c, dp, obj_from);
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
                    cache[i] = parse_one(str_bytes(s), str_len(s), &ix, dp,
                                         i >= obj_from);
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
                long tot = parse_total(a, strings, trial, obj_from);
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

Vec_MsgTable *tokselect_snapshot(Arena *a, const Adventure *adv,
                                 int with_objects)
{
    const Vec_Message *tabs[TOKSEL_TABLES];
    Vec_MsgTable *out = vec_new_MsgTable(a);
    size_t t, k;

    compressable_tables(adv, with_objects, tabs);
    for (t = 0; t < TOKSEL_TABLES; t++) {
        Vec_Message *copy = vec_new_Message(a);
        for (k = 0; tabs[t] != NULL && k < vec_len_Message(tabs[t]); k++) {
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
                     const Vec_Str *final_tokens, int with_objects)
{
    const Vec_Message *tabs[TOKSEL_TABLES];
    Arena *scratch = arena_new(0);
    size_t t, k, i;
    int ok = 1;

    compressable_tables(adv, with_objects, tabs);
    for (t = 0; ok && t < TOKSEL_TABLES; t++) {
        const Vec_Message *now = tabs[t];
        Vec_Message *was = vec_at_MsgTable(before, t);
        if (now == NULL) continue;
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

/* Appends the text at buffer offset `at` to dec. Reference byte b
   skips b-127 bit-7-terminated entries from tok_at, as the interpreter
   does. 0 on any read past len. */
static int decode_at(Str *dec, const unsigned char *ddb, size_t len,
                     size_t at, size_t tok_at)
{
    for (;;) {
        unsigned b;
        size_t p = tok_at, skip;
        if (at >= len) return 0;
        b = ddb[at++] ^ 0xFFu;
        if (b == 0x0A) return 1;
        if (b < 128) { str_push(dec, (char)b); continue; }
        for (skip = b - 127; skip > 0; p++) {
            if (p >= len) return 0;
            if (ddb[p] & 0x80) skip--;
        }
        do {
            if (p >= len) return 0;
            str_push(dec, (char)(ddb[p] & 0x7F));
        } while (!(ddb[p++] & 0x80));
    }
}

int tokselect_verify_ddb(const Vec_MsgTable *before, const unsigned char *ddb,
                         size_t ddb_len, long base_addr, long tokens_addr,
                         int big_endian, const DdbTableRef refs[4],
                         int *bad_ref, size_t *bad_index, size_t *skipped)
{
    Arena *scratch = arena_new(0);
    long tok_off = tokens_addr - base_addr;
    size_t tok_at = tok_off < 0 ? ddb_len : (size_t)tok_off;
    size_t k;
    int r, ok = 1;

    *skipped = 0;
    for (r = 0; r < 4; r++) *skipped += refs[r].count - refs[r].checked;
    for (r = 0; ok && r < 4; r++) {
        const DdbTableRef *t = &refs[r];
        const Vec_Message *was = vec_at_MsgTable(before, (size_t)t->snap_index);
        size_t n = vec_len_Message(was);
        long lk = t->lookup_addr - base_addr;
        if (t->count != n || t->checked > n) {   /* length differs from source */
            *bad_ref = r;
            *bad_index = t->count < n ? t->count : n;
            ok = 0;
            break;
        }
        for (k = 0; k < t->checked; k++) {
            size_t w = (size_t)lk + 2 * k;
            Str *ref = vec_at_Message(was, k)->Text, *dec = str_new(scratch);
            long at;
            if (lk < 0 || w + 1 >= ddb_len) { ok = 0; break; }
            at = big_endian ? (ddb[w] << 8 | ddb[w + 1])
                            : (ddb[w] | ddb[w + 1] << 8);
            at -= base_addr;
            ok = at >= 0 && decode_at(dec, ddb, ddb_len, (size_t)at, tok_at)
                 && str_len(dec) == str_len(ref)
                 && memcmp(str_bytes(dec), str_bytes(ref), str_len(ref)) == 0;
            if (!ok) break;
        }
        if (!ok) { *bad_ref = r; *bad_index = k; }
    }
    arena_free(scratch);
    return ok;
}

Vec_Str *tokselect_compress(Arena *a, Diag *d, const Adventure *adv,
                            TokenSet *ts, int classic_mode, int with_objects,
                            int classic_objects, long *savings)
{
    Vec_Str *strings = corpus_strings(a, adv, with_objects);
    size_t obj_from = objects_from(strings, adv, with_objects,
                                   classic_objects);
    Vec_Str *final_tokens = vec_new_Str(a);
    long cur, original = 0, encoded = 0;
    size_t i;
    int sweep;

    *savings = 0;
    for (i = 0; i < vec_len_Str(ts->tokens); i++)
        vec_push_Str(final_tokens, vec_at_Str(ts->tokens, i));

    /* Prune entries 1..n: drop when removal shrinks text+table.
       Sequential pass-1 savings are the wrong metric here. */
    cur = parse_total(a, strings, final_tokens, obj_from);
    for (sweep = 0; sweep < 2; sweep++) {
        int removed = 0;
        long j;
        for (j = (long)vec_len_Str(final_tokens) - 1; j >= 1; j--) {
            Str *tok = vec_at_Str(final_tokens, (size_t)j);
            Vec_Str *trial = vec_without(a, final_tokens, (size_t)j);
            long tot = parse_total(a, strings, trial, obj_from);
            if (tot - cur < (long)str_len(tok)) {
                diag_verbose(d, "Optimal encoder: dropped token [%s].",
                             str_cstr(tok));
                final_tokens = trial;
                cur = tot;
                removed = 1;
            }
        }
        if (!removed) break;
    }

    /* Delimiters 128..255 address entry 0 plus 128 live tokens. */
    if (vec_len_Str(final_tokens) > 129) {
        diag_fatal(d, "Tokens file has too many tokens for optimal "
                      "encoding (%lu; maximum is 128).",
                   (unsigned long)(vec_len_Str(final_tokens) - 1));
        return NULL;
    }
    diag_verbose(d, "Compression tokens used: %zu.",
                 vec_len_Str(final_tokens));

    if (classic_mode) {
        Str *space = str_new(a);
        str_push(space, ' ');
        while (vec_len_Str(final_tokens) < 128)
            vec_push_Str(final_tokens, space);
        diag_verbose(d, "Filling tokens table up to 128 tokens for "
                        "classic mode compatibility.");
    }

    {
        TokIndex ix;
        long *dp;
        Str **choice;
        const Vec_Message *tabs[TOKSEL_TABLES];
        size_t maxlen = 0, t, k;

        tokindex_build(a, &ix, final_tokens);
        for (i = 0; i < vec_len_Str(strings); i++)
            if (str_len(vec_at_Str(strings, i)) > maxlen)
                maxlen = str_len(vec_at_Str(strings, i));
        dp = arena_alloc(a, (maxlen + 1) * sizeof(*dp));
        choice = arena_alloc(a, (maxlen + 1) * sizeof(*choice));

        compressable_tables(adv, with_objects, tabs);
        for (t = 0; t < TOKSEL_TABLES; t++) {
            if (tabs[t] == NULL) continue;
            for (k = 0; k < vec_len_Message(tabs[t]); k++) {
                Message *m = vec_at_Message(tabs[t], k);
                const unsigned char *s = str_bytes(m->Text);
                size_t n = str_len(m->Text), pos;
                int obj_rule = t == 4 && classic_objects;
                long prot = obj_rule ? (long)tokselect_protected_prefix(s, n)
                                     : 0;
                Str *enc = str_new(a);
                long ii;

                original += (long)n;
                dp[n] = 0;
                choice[n] = NULL;
                for (ii = (long)n - 1; ii >= 0; ii--) {
                    long best = dp[ii + 1] + 1;
                    Str *pick = NULL;   /* ties keep the literal */
                    const Vec_Str *bucket = ii < prot ? NULL
                                                      : ix.by_first[s[ii]];
                    size_t bj;
                    for (bj = 0; bucket != NULL &&
                                 bj < vec_len_Str(bucket); bj++) {
                        Str *tok = vec_at_Str(bucket, bj);
                        size_t tl = str_len(tok);
                        if (obj_rule && token_has_dot(tok)) continue;
                        if (tl <= n - (size_t)ii &&
                            memcmp(s + ii, str_bytes(tok), tl) == 0) {
                            long v = dp[ii + (long)tl] + 1;
                            if (v < best) { best = v; pick = tok; }
                        }
                    }
                    dp[ii] = best;
                    choice[ii] = pick;
                }
                for (pos = 0; pos < n; ) {
                    Str *tok = choice[pos];
                    if (tok == NULL) {
                        str_push(enc, (char)s[pos]);
                        pos++;
                    } else {
                        size_t j2;
                        for (j2 = 0; j2 < vec_len_Str(final_tokens); j2++)
                            if (vec_at_Str(final_tokens, j2) == tok) break;
                        str_push_u8(enc, (unsigned)(j2 + 127));
                        pos += str_len(tok);
                    }
                }
                encoded += (long)str_len(enc);
                m->Text = enc;
            }
        }
    }
    *savings = original - encoded;
    return final_tokens;
}
