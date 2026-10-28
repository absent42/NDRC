/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/tokselect.c - per-game compression token selection (-auto-tokens).
   Copyright (C) 2026 Dan Gibson. */
#include "tokselect.h"
#include "huff.h"
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

/* Integer cost model: per-literal cost, uniform reference cost, table
   cost per token byte. Format 1 uses all 1s (unit_costs). */
typedef struct {
    int lit[256];
    long ref, tab;
} Costs;

static const Costs *unit_costs(void)
{
    static Costs u;
    static int done;
    int i;

    if (!done) {
        for (i = 0; i < 256; i++) u.lit[i] = 1;
        u.ref = 1;
        u.tab = 1;
        done = 1;
    }
    return &u;
}

/* dp[i] = cheapest encoding of s[i..n); dp sized n+1 by the caller.
   obj_rule: classic object text - literal prefix, no '.' tokens. */
static long parse_cost(const unsigned char *s, size_t n, const TokIndex *ix,
                       long *dp, int obj_rule, const Costs *k)
{
    size_t i, prot = obj_rule ? tokselect_protected_prefix(s, n) : 0;

    dp[n] = 0;
    for (i = n; i-- > 0; ) {
        long best = dp[i + 1] + k->lit[s[i]];
        const Vec_Str *bucket = i < prot ? NULL : ix->by_first[s[i]];
        size_t j;
        for (j = 0; bucket != NULL && j < vec_len_Str(bucket); j++) {
            Str *t = vec_at_Str(bucket, j);
            size_t tl = str_len(t);
            if (obj_rule && token_has_dot(t)) continue;
            if (tl <= n - i && memcmp(s + i, str_bytes(t), tl) == 0) {
                long v = dp[i + tl] + k->ref;
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
static long parse_total_cost(Arena *a, const Vec_Str *strings,
                             const Vec_Str *tokens, size_t obj_from,
                             const Costs *k)
{
    TokIndex ix;
    long *dp = dp_scratch(a, strings);
    long total = 0;
    size_t i;

    tokindex_build(a, &ix, tokens);
    for (i = 0; i < vec_len_Str(strings); i++) {
        Str *s = vec_at_Str(strings, i);
        total += parse_cost(str_bytes(s), str_len(s), &ix, dp,
                            i >= obj_from, k);
    }
    return total;
}

static long parse_total(Arena *a, const Vec_Str *strings,
                        const Vec_Str *tokens, size_t obj_from)
{
    return parse_total_cost(a, strings, tokens, obj_from, unit_costs());
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

Vec_Str *tokselect_corpus_strings(Arena *a, const Adventure *adv,
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
                      size_t obj_from, const Costs *k)
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
        gain += cache[i] - parse_cost(str_bytes(s), str_len(s), &ix, dp,
                                      i >= obj_from, k);
    }
    arena_free(scratch);
    return gain - (long)c->len * k->tab;
}

static Vec_Str *vec_without(Arena *a, const Vec_Str *v, size_t skip)
{
    Vec_Str *out = vec_new_Str(a);
    size_t i;

    for (i = 0; i < vec_len_Str(v); i++)
        if (i != skip) vec_push_Str(out, vec_at_Str(v, i));
    return out;
}

/* The greedy under cost model k: up to maxtok tokens of 2..maxlen bytes,
   pruned, longest-first. *ncand = candidates seen. */
static Vec_Str *select_greedy(Arena *a, const Vec_Str *strings,
                              size_t obj_from, size_t maxtok, size_t maxlen,
                              int exclude_placeholders, const Costs *k,
                              size_t *ncand)
{
    Vec_Cand *order = vec_new_Cand(a);
    CandMap map;
    Heap heap = {0};
    Vec_Str *chosen = vec_new_Str(a);
    long *cache, *dp, *fresh_it, *fresh_gain;
    size_t i;
    long it = 0;

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
            if (maxl > maxlen) maxl = maxlen;
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
        long lits = 0, proxy;
        unsigned b;
        for (b = 0; b < c->len; b++) lits += k->lit[c->ptr[b]];
        proxy = c->count * (lits - k->ref) - (long)c->len * k->tab;
        if (c->count >= 2 && proxy > 0) heap_push(a, &heap, proxy, c->idx);
    }

    cache = arena_alloc(a, (vec_len_Str(strings) + 1) * sizeof(long));
    for (i = 0; i < vec_len_Str(strings); i++) {
        Str *s = vec_at_Str(strings, i);
        size_t b;
        cache[i] = 0;
        for (b = 0; b < str_len(s); b++) cache[i] += k->lit[str_bytes(s)[b]];
    }
    dp = dp_scratch(a, strings);
    fresh_it = arena_calloc(a, (vec_len_Cand(order) + 1) * sizeof(long));
    fresh_gain = arena_calloc(a, (vec_len_Cand(order) + 1) * sizeof(long));

    while (vec_len_Str(chosen) < maxtok && heap.n > 0) {
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
            net = eval_gain(strings, cache, chosen, c, dp, obj_from, k);
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
                    cache[i] = parse_cost(str_bytes(s), str_len(s), &ix, dp,
                                          i >= obj_from, k);
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
                long tl = (long)str_len(vec_at_Str(chosen, (size_t)j)) * k->tab;
                long tot = parse_total_cost(a, strings, trial, obj_from, k);
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
        Str *key = vec_at_Str(chosen, i);
        long j = (long)i - 1;
        while (j >= 0 &&
               str_len(vec_at_Str(chosen, (size_t)j)) < str_len(key)) {
            vec_set_Str(chosen, (size_t)j + 1, vec_at_Str(chosen, (size_t)j));
            j--;
        }
        vec_set_Str(chosen, (size_t)j + 1, key);
    }
    *ncand = vec_len_Cand(order);
    return chosen;
}

TokenSet *tokselect_run(Arena *a, Diag *d, const Adventure *adv,
                        int exclude_placeholders, int with_objects,
                        int classic_objects)
{
    Vec_Str *strings = tokselect_corpus_strings(a, adv, with_objects);
    size_t obj_from = objects_from(strings, adv, with_objects,
                                   classic_objects);
    size_t i, ncand;
    Vec_Str *chosen = select_greedy(a, strings, obj_from, TOKSEL_MAXTOK,
                                    TOKSEL_MAXLEN, exclude_placeholders,
                                    unit_costs(), &ncand);
    TokenSet *ts;

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
                 (unsigned long)ncand,
                 (unsigned long)vec_len_Str(chosen));
    return ts;
}

Vec_Str *tokselect_select_costed(Arena *a, Diag *d, const Vec_Str *strings,
                                 size_t obj_from, unsigned maxtok,
                                 const int lit_cost[256], int ref_cost,
                                 int tab_cost_per_byte, int maxlen)
{
    Costs *k = arena_alloc(a, sizeof(*k));
    size_t ncand;

    (void)d;
    memcpy(k->lit, lit_cost, sizeof k->lit);
    k->ref = ref_cost;
    k->tab = tab_cost_per_byte;
    return select_greedy(a, strings, obj_from, maxtok,
                         maxlen < TOKSEL_MINLEN ? TOKSEL_MINLEN : (size_t)maxlen,
                         0, k, &ncand);
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

/* Format 2 text block parsed from the emitted bytes; canonical decode
   tables rebuilt from the lengths alone. */
typedef struct {
    unsigned ntok, fp;
    unsigned short count[HUFF_MAXLEN + 1], sym[HUFF_MAXSYM];
    const unsigned char *ent[383];    /* length byte, then the body */
} Block2;

static int entry_has_ref(const unsigned char *e)
{
    unsigned i;
    for (i = 1; i <= e[0]; i++) if (e[i] >= 128) return 1;
    return 0;
}

static int block2_parse(Block2 *b, const unsigned char *ddb, size_t len,
                        long at_l)
{
    const unsigned char *p;
    unsigned char lengths[512];
    unsigned flags, nsym, lbytes, i, l, n = 0;
    unsigned short offs[HUFF_MAXLEN + 2];
    long left = 1;
    size_t at, room;

    if (at_l < 0 || (size_t)at_l >= len || len - (size_t)at_l < 7) return 0;
    at = (size_t)at_l;
    p = ddb + at;
    room = len - at;
    flags = p[2];
    if (p[0] != 'H' || p[1] != '2' || (flags & 0xFE)) return 0;
    nsym = flags ? 511 : 256;
    lbytes = flags ? 256 : 128;
    b->ntok = (unsigned)p[3] | (unsigned)p[4] << 8;
    b->fp = (unsigned)p[5] | (unsigned)p[6] << 8;
    if (b->ntok == 0 || b->ntok > (flags ? 383u : 128u) ||
        room < 7 + lbytes + 2 * (size_t)b->ntok ||
        crc16_ccitt(p + 7, lbytes) != b->fp) return 0;

    /* Unpack; no length for the pad nibble or a symbol past ntok. */
    for (i = 0; i < 2 * lbytes; i++) {
        lengths[i] = (unsigned char)(p[7 + i / 2] >> (i % 2 ? 4 : 0) & 15);
        if (lengths[i] && (i >= nsym || i >= 128 + b->ntok)) return 0;
    }
    memset(b->count, 0, sizeof b->count);
    for (i = 0; i < nsym; i++) b->count[lengths[i]]++;
    b->count[0] = 0;
    for (l = 1; l <= HUFF_MAXLEN; l++) {      /* Kraft: not over-full */
        left = 2 * left - b->count[l];
        if (left < 0) return 0;
    }
    offs[1] = 0;
    for (l = 1; l <= HUFF_MAXLEN; l++)
        offs[l + 1] = (unsigned short)(offs[l] + b->count[l]);
    for (i = 0; i < nsym; i++)
        if (lengths[i]) { b->sym[offs[lengths[i]]++] = (unsigned short)i; n++; }
    if (n == 0) return 0;

    for (i = 0; i < b->ntok; i++) {
        const unsigned char *w = p + 7 + lbytes + 2 * i;
        size_t off = (size_t)w[0] | (size_t)w[1] << 8;
        if (off >= room || p[off] < 1 || p[off] > 32 ||
            room - off < 1 + (size_t)p[off]) return 0;
        b->ent[i] = p + off;
    }
    /* References: tokens 0-127 whose entries hold none. */
    for (i = 0; i < b->ntok; i++) {
        const unsigned char *e = b->ent[i];
        for (l = 1; l <= e[0]; l++)
            if (e[l] >= 128 && ((unsigned)e[l] - 128 >= b->ntok ||
                                entry_has_ref(b->ent[e[l] - 128]))) return 0;
    }
    return 1;
}

/* One symbol at bit *pos of d[0..len), MSB first; -1 past the end or
   on an unassigned code. */
static int block2_sym(const Block2 *b, const unsigned char *d, size_t len,
                      size_t *pos)
{
    unsigned code = 0, first = 0, index = 0, l;

    for (l = 1; l <= HUFF_MAXLEN; l++) {
        if (*pos >= 8 * len) return -1;
        code |= (unsigned)(d[*pos >> 3] >> (7 - (*pos & 7))) & 1u;
        (*pos)++;
        if (code - first < b->count[l]) return b->sym[index + code - first];
        index += b->count[l];
        first = (first + b->count[l]) << 1;
        code <<= 1;
    }
    return -1;
}

/* Decodes the stream at d[at] to its end code, entries expanded one
   level, and compares with want. */
static int block2_matches(const Block2 *b, Arena *a, const unsigned char *d,
                          size_t len, long at, const Str *want)
{
    Str *dec = str_new(a);
    size_t pos = 0;

    if (at < 0 || (size_t)at >= len) return 0;
    for (;;) {
        int s = block2_sym(b, d + at, len - (size_t)at, &pos);
        const unsigned char *e;
        unsigned i, j;
        if (s < 0 || str_len(dec) > str_len(want)) return 0;
        if (s == 10) break;
        if (s < 128) { str_push_u8(dec, (unsigned)s); continue; }
        e = b->ent[s - 128];
        for (i = 1; i <= e[0]; i++) {
            const unsigned char *t;
            if (e[i] < 128) { str_push_u8(dec, e[i]); continue; }
            t = b->ent[e[i] - 128];
            for (j = 1; j <= t[0]; j++) str_push_u8(dec, t[j]);
        }
    }
    return str_len(dec) == str_len(want) &&
           memcmp(str_bytes(dec), str_bytes(want), str_len(want)) == 0;
}

static int verify_ddb2(const Vec_MsgTable *before, const unsigned char *ddb,
                       size_t ddb_len, long base_addr, long tokens_addr,
                       int big_endian, const DdbTableRef refs[4],
                       int *bad_ref, size_t *bad_index, size_t *skipped)
{
    Arena *scratch;
    Block2 b;
    size_t k;
    int r, ok = 1;

    *skipped = 0;
    for (r = 0; r < 4; r++) *skipped += refs[r].count - refs[r].checked;
    if (!block2_parse(&b, ddb, ddb_len, tokens_addr - base_addr)) {
        *bad_ref = -1;
        *bad_index = 0;
        return 0;
    }
    scratch = arena_new(0);
    for (r = 0; ok && r < 4; r++) {
        const DdbTableRef *t = &refs[r];
        const Vec_Message *was = vec_at_MsgTable(before, (size_t)t->snap_index);
        size_t n = vec_len_Message(was);
        long lk = t->lookup_addr - base_addr;
        if (t->count != n || t->checked > n) {
            *bad_ref = r;
            *bad_index = t->count < n ? t->count : n;
            ok = 0;
            break;
        }
        for (k = 0; k < t->checked; k++) {
            size_t w = (size_t)lk + 2 * k;
            long at;
            if (lk < 0 || w + 1 >= ddb_len) { ok = 0; break; }
            at = big_endian ? (ddb[w] << 8 | ddb[w + 1])
                            : (ddb[w] | ddb[w + 1] << 8);
            ok = block2_matches(&b, scratch, ddb, ddb_len, at - base_addr,
                                vec_at_Message(was, k)->Text);
            if (!ok) break;
        }
        if (!ok) { *bad_ref = r; *bad_index = k; }
    }
    arena_free(scratch);
    return ok;
}

int tokselect_verify_xmb2(const Vec_MsgTable *before, const unsigned char *ddb,
                          size_t ddb_len, long base_addr, long tokens_addr,
                          const unsigned char *xmb, size_t xmb_len,
                          const long *offsets, long *bad_index)
{
    const Vec_Message *was = vec_at_MsgTable(before, 3);
    Arena *scratch;
    Block2 b;
    size_t k;
    int ok = 1;

    *bad_index = -1;
    if (!block2_parse(&b, ddb, ddb_len, tokens_addr - base_addr) ||
        xmb_len < 4 || xmb[0] != 'X' || xmb[1] != '2' ||
        ((unsigned)xmb[2] | (unsigned)xmb[3] << 8) != b.fp) return 0;
    scratch = arena_new(0);
    for (k = 0; ok && k < vec_len_Message(was); k++) {
        ok = block2_matches(&b, scratch, xmb, xmb_len, offsets[k],
                            vec_at_Message(was, k)->Text);
        if (!ok) *bad_index = (long)k;
    }
    arena_free(scratch);
    return ok;
}

int tokselect_verify_ddb(const Vec_MsgTable *before, const unsigned char *ddb,
                         size_t ddb_len, long base_addr, long tokens_addr,
                         int big_endian, const DdbTableRef refs[4],
                         int *bad_ref, size_t *bad_index, size_t *skipped)
{
    /* Byte 0 is the header version when the text block sits past it. */
    if (tokens_addr > base_addr && ddb_len > 0 && (ddb[0] & 0x80))
        return verify_ddb2(before, ddb, ddb_len, base_addr, tokens_addr,
                           big_endian, refs, bad_ref, bad_index, skipped);
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

/* DP-encodes every compressable text through tokens. counts != NULL:
   count references per entry only, texts untouched; else rewrite them. */
static void encode_texts(Arena *a, const Adventure *adv,
                         const Vec_Str *tokens, int with_objects,
                         int classic_objects, long *counts,
                         long *original, long *encoded)
{
    TokIndex ix;
    long *dp;
    Str **choice;
    const Vec_Message *tabs[TOKSEL_TABLES];
    size_t maxlen = 0, t, k;

    tokindex_build(a, &ix, tokens);
    compressable_tables(adv, with_objects, tabs);
    for (t = 0; t < TOKSEL_TABLES; t++) {
        if (tabs[t] == NULL) continue;
        for (k = 0; k < vec_len_Message(tabs[t]); k++)
            if (str_len(vec_at_Message(tabs[t], k)->Text) > maxlen)
                maxlen = str_len(vec_at_Message(tabs[t], k)->Text);
    }
    dp = arena_alloc(a, (maxlen + 1) * sizeof(*dp));
    choice = arena_alloc(a, (maxlen + 1) * sizeof(*choice));

    for (t = 0; t < TOKSEL_TABLES; t++) {
        if (tabs[t] == NULL) continue;
        for (k = 0; k < vec_len_Message(tabs[t]); k++) {
            Message *m = vec_at_Message(tabs[t], k);
            const unsigned char *s = str_bytes(m->Text);
            size_t n = str_len(m->Text), pos;
            int obj_rule = t == 4 && classic_objects;
            long prot = obj_rule ? (long)tokselect_protected_prefix(s, n)
                                 : 0;
            Str *enc = counts != NULL ? NULL : str_new(a);
            long ii;

            if (counts == NULL) *original += (long)n;
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
                    if (enc != NULL) str_push(enc, (char)s[pos]);
                    pos++;
                } else {
                    size_t j2;
                    for (j2 = 0; j2 < vec_len_Str(tokens); j2++)
                        if (vec_at_Str(tokens, j2) == tok) break;
                    if (enc != NULL) str_push_u8(enc, (unsigned)(j2 + 127));
                    else counts[j2]++;
                    pos += str_len(tok);
                }
            }
            if (enc != NULL) {
                *encoded += (long)str_len(enc);
                m->Text = enc;
            }
        }
    }
}

Vec_Str *tokselect_compress(Arena *a, Diag *d, const Adventure *adv,
                            TokenSet *ts, int classic_mode, int with_objects,
                            int classic_objects, long *savings)
{
    Vec_Str *strings = tokselect_corpus_strings(a, adv, with_objects);
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

    /* Interpreters scan the table from entry 0 per reference: most
       referenced first, stable insertion sort (ties keep selection order). */
    {
        size_t n = vec_len_Str(final_tokens);
        long *counts = arena_calloc(a, (n + 1) * sizeof(*counts));
        encode_texts(a, adv, final_tokens, with_objects, classic_objects,
                     counts, &original, &encoded);
        for (i = 2; i < n; i++) {
            Str *kt = vec_at_Str(final_tokens, i);
            long kc = counts[i];
            size_t j = i;
            while (j > 1 && counts[j - 1] < kc) {
                vec_set_Str(final_tokens, j, vec_at_Str(final_tokens, j - 1));
                counts[j] = counts[j - 1];
                j--;
            }
            vec_set_Str(final_tokens, j, kt);
            counts[j] = kc;
        }
        diag_verbose(d, "Token table ordered by use.");
    }

    if (classic_mode) {
        Str *space = str_new(a);
        str_push(space, ' ');
        while (vec_len_Str(final_tokens) < 128)
            vec_push_Str(final_tokens, space);
        diag_verbose(d, "Filling tokens table up to 128 tokens for "
                        "classic mode compatibility.");
    }

    encode_texts(a, adv, final_tokens, with_objects, classic_objects, NULL,
                 &original, &encoded);
    *savings = original - encoded;
    return final_tokens;
}
