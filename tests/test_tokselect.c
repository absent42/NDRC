/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_tokselect.c - unit tests for src/tokselect.c.
   Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "../src/tokselect.h"
#include "../src/huff.h"

#include <stdlib.h>

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
    arena_free(a);
}

TEST(parse_token_reduces)
{
    Arena *a = arena_new(0);
    CHECK_INT(tokselect_parse_total(a, strs1(a, "ABABAB"),
                                    toks(a, "AB", NULL)), 3);
    arena_free(a);
}

TEST(parse_is_optimal_not_greedy)
{
    /* Leftmost-greedy would take AB and pay X,AB,C,Y = 4; the DP must
       find X,ABC,Y = 3. */
    Arena *a = arena_new(0);
    CHECK_INT(tokselect_parse_total(a, strs1(a, "XABCY"),
                                    toks(a, "AB", "ABC")), 3);
    arena_free(a);
}

TEST(parse_picks_cheaper_cover)
{
    /* AA+AA = 2 beats AAA+A = 2? equal - but AA,AA is 2 and must not
       come out worse than 2. Pin the exact optimum. */
    Arena *a = arena_new(0);
    CHECK_INT(tokselect_parse_total(a, strs1(a, "AAAA"),
                                    toks(a, "AA", "AAA")), 2);
    arena_free(a);
}

TEST(parse_token_longer_than_string)
{
    Arena *a = arena_new(0);
    CHECK_INT(tokselect_parse_total(a, strs1(a, "AB"),
                                    toks(a, "ABCD", NULL)), 2);
    arena_free(a);
}

static Adventure *adv_new(Arena *a)
{
    Adventure *adv = arena_calloc(a, sizeof *adv);
    adv->messages = vec_new_Message(a);
    adv->sysmess = vec_new_Message(a);
    adv->locations = vec_new_Message(a);
    adv->xmessages = vec_new_Message(a);
    adv->objects = vec_new_Message(a);
    return adv;
}

static void add_msg(Arena *a, Vec_Message *t, const char *text)
{
    Message *m = arena_calloc(a, sizeof *m);
    m->Text = str_from(a, text);
    vec_push_Message(t, m);
}

/* Byte-compare two TokenSets. */
static int tokensets_equal(const TokenSet *x, const TokenSet *y)
{
    size_t i;
    if (vec_len_Str(x->tokens) != vec_len_Str(y->tokens)) return 0;
    for (i = 0; i < vec_len_Str(x->tokens); i++) {
        Str *p = vec_at_Str(x->tokens, i), *q = vec_at_Str(y->tokens, i);
        if (str_len(p) != str_len(q) ||
            memcmp(str_bytes(p), str_bytes(q), str_len(p)) != 0) return 0;
    }
    return 1;
}

TEST(select_finds_the_obvious_token)
{
    /* Hand-traced: on "XYXYXYXY" the only net-positive pick is XY
       (all longer periodic candidates cost more table than they save).
       Expect exactly [0x00, "XY"]. */
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts;
    Str *t;

    add_msg(a, adv->messages, "XYXYXYXY");
    ts = tokselect_run(a, d, adv, 0, 0, 0);
    CHECK_INT(ts->has_tokens, 1);
    CHECK_INT(ts->advanced, 1);
    CHECK_STR(ts->compression, "advanced");
    CHECK_INT((long)vec_len_Str(ts->tokens), 2);
    t = vec_at_Str(ts->tokens, 0);
    CHECK_INT((long)str_len(t), 1);
    CHECK_INT(str_bytes(t)[0], 0);
    t = vec_at_Str(ts->tokens, 1);
    CHECK_INT((long)str_len(t), 2);
    CHECK(memcmp(str_bytes(t), "XY", 2) == 0);
    arena_free(a);
}

TEST(select_is_deterministic)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    int i;

    for (i = 0; i < 8; i++) {
        add_msg(a, adv->messages,
                "You can see nothing special about the old wooden door. ");
        add_msg(a, adv->locations,
                "You are standing in a narrow stone corridor. The corridor "
                "runs north and south. ");
    }
    CHECK(tokensets_equal(tokselect_run(a, d, adv, 0, 0, 0),
                          tokselect_run(a, d, adv, 0, 0, 0)));
    arena_free(a);
}

TEST(select_orders_longest_first)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts;
    size_t i;
    int i2;

    for (i2 = 0; i2 < 6; i2++) {
        add_msg(a, adv->messages, "MNMNMNMNMNMNMNMNMNMN");
        add_msg(a, adv->messages, "GHIJKLGHIJKL");
    }
    ts = tokselect_run(a, d, adv, 0, 0, 0);
    CHECK(vec_len_Str(ts->tokens) >= 3);
    /* entry 0 is the 0x00 byte; from entry 1 on lengths never grow */
    for (i = 2; i < vec_len_Str(ts->tokens); i++) {
        CHECK(str_len(vec_at_Str(ts->tokens, i)) <=
              str_len(vec_at_Str(ts->tokens, i - 1)));
    }
    arena_free(a);
}

static int any_token_contains(const TokenSet *ts, unsigned char b)
{
    size_t i, j;
    for (i = 1; i < vec_len_Str(ts->tokens); i++) {
        Str *t = vec_at_Str(ts->tokens, i);
        for (j = 0; j < str_len(t); j++)
            if (str_bytes(t)[j] == b) return 1;
    }
    return 0;
}

TEST(select_placeholder_exclusion)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv1 = adv_new(a);
    Adventure *adv2 = adv_new(a);
    int i;

    for (i = 0; i < 4; i++) {
        add_msg(a, adv1->messages, "QR_STQR_STQR_ST");
        add_msg(a, adv2->messages, "QR_STQR_STQR_ST");
    }
    CHECK(any_token_contains(tokselect_run(a, d, adv1, 0, 0, 0), '_'));
    CHECK(!any_token_contains(tokselect_run(a, d, adv2, 1, 0, 0), '_'));
    arena_free(a);
}

TEST(select_empty_and_tiny_input)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts;

    ts = tokselect_run(a, d, adv, 0, 0, 0);      /* no text at all */
    CHECK_INT((long)vec_len_Str(ts->tokens), 1);
    CHECK_INT(ts->has_tokens, 1);

    add_msg(a, adv->messages, "A");        /* below min candidate len */
    ts = tokselect_run(a, d, adv, 0, 0, 0);
    CHECK_INT((long)vec_len_Str(ts->tokens), 1);
    arena_free(a);
}

TEST(select_caps_at_128)
{
    /* 150 independent winners: distinct (upper,lower) pairs, so no
       candidate spans messages. Every message contributes net-positive
       candidates (which of "Xy"/"XyXy" wins depends on the eval-budget
       fallback - the proxy tiers here exceed the 500-eval budget), so
       selection wants all 150 message families and must stop at 128. */
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts;
    char msg[9];
    int i;
    size_t k;

    for (i = 0; i < 150; i++) {
        char x = (char)('A' + i / 26), y = (char)('a' + i % 26);
        msg[0] = x; msg[1] = y; msg[2] = x; msg[3] = y;
        msg[4] = x; msg[5] = y; msg[6] = x; msg[7] = y;
        msg[8] = '\0';
        add_msg(a, adv->messages, msg);
    }
    ts = tokselect_run(a, d, adv, 0, 0, 0);
    CHECK_INT((long)vec_len_Str(ts->tokens), 129);   /* 0x00 + 128 cap */
    for (k = 1; k < vec_len_Str(ts->tokens); k++) {
        CHECK(str_len(vec_at_Str(ts->tokens, k)) >= 2);
        CHECK(str_len(vec_at_Str(ts->tokens, k)) <= 12);
    }
    arena_free(a);
}

TEST(snapshot_verify_roundtrip_and_tamper)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts;
    Vec_MsgTable *before;
    Vec_Str *final_tokens;
    long savings = 0;

    add_msg(a, adv->messages, "THE CAT SAT ON THE MAT. THE CAT SAT.");
    add_msg(a, adv->locations, "THE MAT IS ON THE FLOOR BY THE CAT.");
    add_msg(a, adv->sysmess, "OK.");

    ts = tokselect_run(a, d, adv, 0, 0, 0);
    before = tokselect_snapshot(a, adv, 0);
    final_tokens = tokens_compress(a, d, adv, ts, 0, &savings);

    CHECK_INT(tokselect_verify(before, adv, final_tokens, 0), 1);

    /* Tampered text must fail the check. */
    str_push(vec_at_Message(adv->messages, 0)->Text, 'Z');
    CHECK_INT(tokselect_verify(before, adv, final_tokens, 0), 0);
    arena_free(a);
}

TEST(verify_catches_wrong_table)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts;
    Vec_MsgTable *before;
    Vec_Str *final_tokens;
    long savings = 0;

    add_msg(a, adv->messages, "ABABABABABAB CDCDCDCDCDCD");
    ts = tokselect_run(a, d, adv, 0, 0, 0);
    before = tokselect_snapshot(a, adv, 0);
    final_tokens = tokens_compress(a, d, adv, ts, 0, &savings);
    CHECK_INT(tokselect_verify(before, adv, final_tokens, 0), 1);

    /* Two independent winners (AB, CD) must survive - assert it, so a
       selector change cannot quietly hollow this test out. */
    CHECK(vec_len_Str(final_tokens) >= 3);
    if (vec_len_Str(final_tokens) >= 3) {
        /* Swap two surviving tokens: references now expand wrongly. */
        Str *t1 = vec_at_Str(final_tokens, 1);
        Str *t2 = vec_at_Str(final_tokens, 2);
        vec_set_Str(final_tokens, 1, t2);
        vec_set_Str(final_tokens, 2, t1);
        CHECK_INT(tokselect_verify(before, adv, final_tokens, 0), 0);
    }
    arena_free(a);
}

static void scratch_path(char *buf, size_t bufsz, const char *filename)
{
    const char *dir = getenv("TMPDIR");
    if (dir == NULL) dir = getenv("TEMP");
    if (dir == NULL) dir = getenv("TMP");
    if (dir == NULL) dir = ".";
    snprintf(buf, bufsz, "%s/%s", dir, filename);
}

/* Writes content to path; test helper for hand-crafted .tok files. */
static void write_text_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f == NULL) return;
    fputs(content, f);
    fclose(f);
}

/* Loads a .tok next to a fake input path; returns the TokenSet. */
static TokenSet *load_tok_text(Arena *a, Diag *d, const char *json)
{
    char tok[512], fake[512];

    scratch_path(tok, sizeof tok, "ndrc_tsel_mk.tok");
    scratch_path(fake, sizeof fake, "ndrc_tsel_mk.json");
    write_text_file(tok, json);
    {
        TokenSet *ts = tokens_load_override(a, d, fake, NULL);
        remove(tok);
        return ts;
    }
}

TEST(loader_marker_gate)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    TokenSet *ts;

    ts = load_tok_text(a, d, "{\"compression\": \"advanced\", "
        "\"encoder\": \"optimal\", \"tokens\": [\"00\",\"4142\"]}");
    CHECK(ts != NULL && ts->optimal_encode == 1);

    ts = load_tok_text(a, d, "{\"compression\": \"advanced\", "
        "\"tokens\": [\"00\",\"4142\"]}");
    CHECK(ts != NULL && ts->optimal_encode == 0);

    /* Gate: marker without advanced compression stays sequential. */
    ts = load_tok_text(a, d, "{\"compression\": \"basic\", "
        "\"encoder\": \"optimal\", \"tokens\": [\"00\",\"4142\"]}");
    CHECK(ts != NULL && ts->optimal_encode == 0);

    ts = load_tok_text(a, d, "{\"compression\": \"none\", "
        "\"encoder\": \"optimal\", \"tokens\": [\"00\",\"4142\"]}");
    CHECK(ts != NULL && ts->optimal_encode == 0);

    /* Non-string or unknown values are ignored, not fatal. */
    ts = load_tok_text(a, d, "{\"compression\": \"advanced\", "
        "\"encoder\": \"fast\", \"tokens\": [\"00\",\"4142\"]}");
    CHECK(ts != NULL && ts->optimal_encode == 0);
    arena_free(a);
}

TEST(loader_objects_off_gate)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    TokenSet *ts;

    ts = load_tok_text(a, d, "{\"compression\":\"advanced\","
        "\"encoder\":\"optimal\",\"objects\":false,\"tokens\":[\"00\"]}");
    CHECK(ts != NULL && ts->objects_off == 1);

    ts = load_tok_text(a, d, "{\"compression\":\"advanced\","
        "\"encoder\":\"optimal\",\"objects\":true,\"tokens\":[\"00\"]}");
    CHECK(ts != NULL && ts->objects_off == 0);

    ts = load_tok_text(a, d, "{\"compression\":\"advanced\","
        "\"encoder\":\"optimal\",\"tokens\":[\"00\"]}");
    CHECK(ts != NULL && ts->objects_off == 0);

    /* Not a JSON false literal: ignored. */
    ts = load_tok_text(a, d, "{\"compression\":\"advanced\","
        "\"objects\":\"false\",\"tokens\":[\"00\"]}");
    CHECK(ts != NULL && ts->objects_off == 0);

    /* Non-advanced compression ignores the field. */
    ts = load_tok_text(a, d, "{\"compression\":\"basic\","
        "\"objects\":false,\"tokens\":[\"00\"]}");
    CHECK(ts != NULL && ts->objects_off == 0);
    arena_free(a);
}

TEST(write_tok_objects_off_roundtrip)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts, *loaded;
    char tok[512], fake_input[512];
    int pass;

    add_msg(a, adv->messages, "WATERWATERWATERWATER EVERYWHEREEVERYWHERE");
    ts = tokselect_run(a, d, adv, 0, 0, 0);
    CHECK_INT(ts->objects_off, 0);
    scratch_path(tok, sizeof tok, "ndrc_tsel_oo.tok");
    scratch_path(fake_input, sizeof fake_input, "ndrc_tsel_oo.json");
    for (pass = 0; pass < 2; pass++) {
        ts->objects_off = pass;
        remove(tok);
        CHECK_INT(tokens_write_tok(tok, ts), 1);
        loaded = tokens_load_override(a, d, fake_input, NULL);
        CHECK(loaded != NULL);
        if (loaded != NULL) {
            CHECK_INT(loaded->optimal_encode, 1);
            CHECK_INT(loaded->objects_off, pass);
        }
    }
    remove(tok);
    arena_free(a);
}

TEST(selector_result_is_marked)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts;

    add_msg(a, adv->messages, "MNMNMNMNMNMN");
    ts = tokselect_run(a, d, adv, 0, 0, 0);
    CHECK_INT(ts->optimal_encode, 1);
    arena_free(a);
}

TEST(write_tok_roundtrip)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts, *loaded;
    char tok[512], fake_input[512];
    const char *probed;
    size_t i;

    add_msg(a, adv->messages, "WATERWATERWATERWATER EVERYWHEREEVERYWHERE");
    ts = tokselect_run(a, d, adv, 0, 0, 0);
    CHECK(vec_len_Str(ts->tokens) >= 2);

    scratch_path(tok, sizeof tok, "ndrc_tsel_rt.tok");
    scratch_path(fake_input, sizeof fake_input, "ndrc_tsel_rt.json");
    remove(tok);
    CHECK(tokens_probe_override(a, fake_input) == NULL);

    CHECK_INT(tokens_write_tok(tok, ts), 1);
    probed = tokens_probe_override(a, fake_input);
    CHECK(probed != NULL);

    loaded = tokens_load_override(a, d, fake_input, NULL);
    CHECK(loaded != NULL);
    if (loaded != NULL) {
        CHECK_INT((long)vec_len_Str(loaded->tokens),
                  (long)vec_len_Str(ts->tokens));
        for (i = 0; i < vec_len_Str(ts->tokens) &&
                    i < vec_len_Str(loaded->tokens); i++) {
            Str *p = vec_at_Str(ts->tokens, i);
            Str *q = vec_at_Str(loaded->tokens, i);
            CHECK(str_len(p) == str_len(q) &&
                  memcmp(str_bytes(p), str_bytes(q), str_len(p)) == 0);
        }
        CHECK_INT(loaded->advanced, 1);
        /* Phase 2: the tee is marked and the marker round-trips. */
        CHECK_INT(loaded->optimal_encode, 1);
    }
    remove(tok);
    arena_free(a);
}

/* Collects the compressable tables' texts, snapshot-side. */
static Vec_Str *snapshot_strings(Arena *a, const Vec_MsgTable *snap)
{
    Vec_Str *out = vec_new_Str(a);
    size_t t, k;

    for (t = 0; t < TOKSEL_TABLES; t++) {
        Vec_Message *tab = vec_at_MsgTable(snap, t);
        for (k = 0; k < vec_len_Message(tab); k++)
            vec_push_Str(out, vec_at_Message(tab, k)->Text);
    }
    return out;
}

/* Hand-builds an advanced TokenSet from latin-1 token strings. */
static TokenSet *ts_new(Arena *a, const char *t1, const char *t2)
{
    TokenSet *ts = arena_calloc(a, sizeof(*ts));
    Str *zero = str_new(a);

    ts->compression = "advanced";
    ts->advanced = 1;
    ts->has_tokens = 1;
    ts->optimal_encode = 1;
    ts->tokens = vec_new_Str(a);
    str_push_u8(zero, 0);
    vec_push_Str(ts->tokens, zero);
    if (t1 != NULL) vec_push_Str(ts->tokens, str_from(a, t1));
    if (t2 != NULL) vec_push_Str(ts->tokens, str_from(a, t2));
    return ts;
}

TEST(compress_optimal_beats_sequential_shape)
{
    /* AB before ABC: sequential provably emits X{AB}CY (4/unit);
       the DP must emit X{ABC}Y (3/unit), dropping AB as net-negative.
       Hand-traced: with both tokens, parse is 9; without AB, still 9,
       so AB (len 2) is pruned; without ABC, 12 - ABC survives. */
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts = ts_new(a, "AB", "ABC");
    Vec_MsgTable *before;
    Vec_Str *final_tokens;
    long savings = 0;
    Str *enc;
    static const unsigned char want[9] =
        { 'X', 0x80, 'Y', 'X', 0x80, 'Y', 'X', 0x80, 'Y' };

    add_msg(a, adv->messages, "XABCYXABCYXABCY");
    before = tokselect_snapshot(a, adv, 0);
    final_tokens = tokselect_compress(a, d, adv, ts, 0, 0, 0, &savings);

    CHECK(final_tokens != NULL);
    CHECK_INT((long)vec_len_Str(final_tokens), 2);   /* 0x00 + ABC */
    enc = vec_at_Message(adv->messages, 0)->Text;
    CHECK_INT((long)str_len(enc), 9);
    CHECK(memcmp(str_bytes(enc), want, 9) == 0);
    CHECK_INT(savings, 6);                            /* 15 - 9 */
    CHECK_INT(tokselect_verify(before, adv, final_tokens, 0), 1);

    /* Spec's tamper bullet, against THIS encoder's output. */
    str_push(vec_at_Message(adv->messages, 0)->Text, 'Z');
    CHECK_INT(tokselect_verify(before, adv, final_tokens, 0), 0);
    arena_free(a);
}

TEST(compress_cost_matches_parse_total)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts = ts_new(a, "TH", "THE ");
    Vec_MsgTable *before;
    Vec_Str *final_tokens, *orig;
    long savings = 0, encoded = 0, t;
    size_t k;

    add_msg(a, adv->messages, "THE CAT AND THE DOG AND THE FOX");
    add_msg(a, adv->locations, "THE THIN PATH THREADS THE HILL");
    before = tokselect_snapshot(a, adv, 0);
    final_tokens = tokselect_compress(a, d, adv, ts, 0, 0, 0, &savings);
    CHECK(final_tokens != NULL);

    orig = snapshot_strings(a, before);
    for (k = 0; k < vec_len_Message(adv->messages); k++)
        encoded += (long)str_len(vec_at_Message(adv->messages, k)->Text);
    for (k = 0; k < vec_len_Message(adv->locations); k++)
        encoded += (long)str_len(vec_at_Message(adv->locations, k)->Text);
    t = tokselect_parse_total(a, orig, final_tokens);
    CHECK_INT(encoded, t);
    CHECK_INT(tokselect_verify(before, adv, final_tokens, 0), 1);
    arena_free(a);
}

TEST(compress_prunes_unused_token)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts = ts_new(a, "ZZZZZZ", NULL);
    Vec_Str *final_tokens;
    long savings = 99;

    add_msg(a, adv->messages, "AAAA");
    final_tokens = tokselect_compress(a, d, adv, ts, 0, 0, 0, &savings);
    CHECK(final_tokens != NULL);
    CHECK_INT((long)vec_len_Str(final_tokens), 1);   /* entry 0 only */
    CHECK_INT(savings, 0);
    arena_free(a);
}

TEST(compress_classic_padding)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts = ts_new(a, "MN", NULL);
    Vec_Str *final_tokens;
    long savings = 0;
    size_t k;

    add_msg(a, adv->messages, "MNMNMNMNMNMN");
    final_tokens = tokselect_compress(a, d, adv, ts, 1, 0, 0, &savings);
    CHECK(final_tokens != NULL);
    CHECK_INT((long)vec_len_Str(final_tokens), 128);
    /* Fillers are literal single spaces and are never referenced:
       the encoded text contains only 'MN' references. */
    for (k = 2; k < 128; k++) {
        Str *t = vec_at_Str(final_tokens, k);
        CHECK(str_len(t) == 1 && str_bytes(t)[0] == ' ');
    }
    arena_free(a);
}

TEST(compress_orders_table_by_use)
{
    /* QQQ selected first but used twice; ZZZ used five times must
       become entry 1 and encode as 0x80. */
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts = ts_new(a, "QQQ", "ZZZ");
    Vec_MsgTable *before;
    Vec_Str *final_tokens;
    long savings = 0;
    Str *enc;
    static const unsigned char want_z[9] =
        { 0x80, 'x', 0x80, 'x', 0x80, 'x', 0x80, 'x', 0x80 };
    static const unsigned char want_q[3] = { 0x81, 'x', 0x81 };

    add_msg(a, adv->messages, "ZZZxZZZxZZZxZZZxZZZ");
    add_msg(a, adv->messages, "QQQxQQQ");
    before = tokselect_snapshot(a, adv, 0);
    final_tokens = tokselect_compress(a, d, adv, ts, 0, 0, 0, &savings);

    CHECK(final_tokens != NULL);
    CHECK_INT((long)vec_len_Str(final_tokens), 3);
    CHECK(strcmp(str_cstr(vec_at_Str(final_tokens, 1)), "ZZZ") == 0);
    CHECK(strcmp(str_cstr(vec_at_Str(final_tokens, 2)), "QQQ") == 0);
    enc = vec_at_Message(adv->messages, 0)->Text;
    CHECK_INT((long)str_len(enc), 9);
    CHECK(memcmp(str_bytes(enc), want_z, 9) == 0);
    enc = vec_at_Message(adv->messages, 1)->Text;
    CHECK_INT((long)str_len(enc), 3);
    CHECK(memcmp(str_bytes(enc), want_q, 3) == 0);
    CHECK_INT(tokselect_verify(before, adv, final_tokens, 0), 1);
    arena_free(a);
}

TEST(compress_guard_trips_past_128_tokens)
{
    /* 135 independent net-positive digraphs (the cap-test corpus
       shape): prune keeps them all, 136 entries > 129 - the pinned
       fatal fires and NULL comes back, not a wrapped delimiter. */
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts = arena_calloc(a, sizeof(*ts));
    Vec_Str *final_tokens;
    long savings = 0;
    char msg[9], tokbuf[3], sinkpath[512];
    int i;
    FILE *sink;

    /* tmpfile() hits the drive root on Windows and often fails
       unelevated; a scratch-path file is deterministic. */
    scratch_path(sinkpath, sizeof sinkpath, "ndrc_tsel_diag.txt");
    sink = fopen(sinkpath, "wb");
    CHECK(sink != NULL);
    if (sink != NULL) diag_set_stream(d, sink);   /* keep output pristine */
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
    for (i = 0; i < 135; i++) {
        char x = (char)('A' + i / 26), y = (char)('a' + i % 26);
        msg[0] = x; msg[1] = y; msg[2] = x; msg[3] = y;
        msg[4] = x; msg[5] = y; msg[6] = x; msg[7] = y;
        msg[8] = '\0';
        add_msg(a, adv->messages, msg);
        tokbuf[0] = x; tokbuf[1] = y; tokbuf[2] = '\0';
        vec_push_Str(ts->tokens, str_from(a, tokbuf));
    }
    final_tokens = tokselect_compress(a, d, adv, ts, 0, 0, 0, &savings);
    CHECK(final_tokens == NULL);
    CHECK(diag_error_count(d) > 0);
    if (sink != NULL) fclose(sink);
    remove(sinkpath);
    arena_free(a);
}

TEST(select_picks_token_from_object_texts)
{
    /* Only object texts repeat; with_objects=1 must select at least
       one token, with_objects=0 none. */
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *on, *off;
    int i;

    add_msg(a, adv->messages, "zq");
    for (i = 0; i < 4; i++) add_msg(a, adv->objects, "lantern of brass");
    on = tokselect_run(a, d, adv, 0, 1, 0);
    off = tokselect_run(a, d, adv, 0, 0, 0);
    CHECK(vec_len_Str(on->tokens) >= 2);
    CHECK_INT((long)vec_len_Str(off->tokens), 1);
    arena_free(a);
}

TEST(select_objects_empty_is_noop)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *x, *y;

    add_msg(a, adv->messages, "XYXYXYXY");
    x = tokselect_run(a, d, adv, 0, 0, 0);
    y = tokselect_run(a, d, adv, 0, 1, 0);
    CHECK_INT((long)vec_len_Str(x->tokens), 2);
    CHECK(tokensets_equal(x, y));
    arena_free(a);
}

TEST(snapshot_verify_covers_objects)
{
    /* The object text holds a token use; verify must cover slot 4. */
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts = ts_new(a, "AB", NULL);
    Vec_MsgTable *before;
    Vec_Str *final_tokens;
    long savings = 0;

    add_msg(a, adv->messages, "ABABAB");
    add_msg(a, adv->objects, "AB");
    before = tokselect_snapshot(a, adv, 1);
    CHECK_INT((long)vec_len_MsgTable(before), TOKSEL_TABLES);
    final_tokens = tokselect_compress(a, d, adv, ts, 0, 1, 0, &savings);
    CHECK(final_tokens != NULL);
    CHECK_INT((long)str_len(vec_at_Message(adv->objects, 0)->Text), 1);
    CHECK_INT(tokselect_verify(before, adv, final_tokens, 1), 1);

    str_push(vec_at_Message(adv->objects, 0)->Text, 'Z');
    CHECK_INT(tokselect_verify(before, adv, final_tokens, 1), 0);
    arena_free(a);
}

TEST(compress_savings_include_objects)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts = ts_new(a, "AB", NULL);
    long savings = 0;

    add_msg(a, adv->messages, "ABABAB");
    add_msg(a, adv->objects, "AB");
    CHECK(tokselect_compress(a, d, adv, ts, 0, 1, 0, &savings) != NULL);
    CHECK_INT(savings, 4);                            /* (6-3) + (2-1) */
    arena_free(a);
}

TEST(select_is_deterministic_with_objects)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *x;
    int i;

    for (i = 0; i < 8; i++) {
        add_msg(a, adv->messages, "The door is shut. ");
        add_msg(a, adv->objects, "an old wooden door");
        add_msg(a, adv->objects, "a rusty iron key");
    }
    x = tokselect_run(a, d, adv, 0, 1, 0);
    CHECK(vec_len_Str(x->tokens) >= 2);
    CHECK(tokensets_equal(x, tokselect_run(a, d, adv, 0, 1, 0)));
    arena_free(a);
}

static size_t pp(const char *s)
{
    return tokselect_protected_prefix((const unsigned char *)s, strlen(s));
}

TEST(protected_prefix_cases)
{
    CHECK_INT((long)pp("a lamp"), 2);
    CHECK_INT((long)pp("the  key"), 5);
    CHECK_INT((long)pp("lamp"), 4);
    CHECK_INT((long)pp(""), 0);
    CHECK_INT((long)pp(" lamp"), 1);
}

/* Object "a lamp" encoded under tokens "a lamp" (0x80) and "lamp"
   (0x81); messages keep both tokens alive through pruning. */
static Str *encode_a_lamp(Arena *a, int classic_objects)
{
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts = ts_new(a, "a lamp", "lamp");
    Vec_MsgTable *before;
    Vec_Str *final_tokens;
    long savings = 0;
    int i;

    for (i = 0; i < 4; i++) add_msg(a, adv->messages, "a lamp");
    add_msg(a, adv->messages, "lamplamplamp");
    add_msg(a, adv->objects, "a lamp");
    before = tokselect_snapshot(a, adv, 1);
    final_tokens = tokselect_compress(a, d, adv, ts, 0, 1, classic_objects,
                                      &savings);
    CHECK(final_tokens != NULL);
    if (final_tokens == NULL) return str_new(a);
    CHECK_INT((long)vec_len_Str(final_tokens), 3);
    CHECK_INT(tokselect_verify(before, adv, final_tokens, 1), 1);
    CHECK_INT((long)str_len(vec_at_Message(adv->messages, 0)->Text), 1);
    return vec_at_Message(adv->objects, 0)->Text;
}

TEST(classic_rule_keeps_prefix_literal)
{
    Arena *a = arena_new(0);
    Str *enc = encode_a_lamp(a, 1);
    static const unsigned char want[3] = { 'a', ' ', 0x81 };

    CHECK_INT((long)str_len(enc), 3);
    CHECK(str_len(enc) == 3 && memcmp(str_bytes(enc), want, 3) == 0);
    arena_free(a);
}

/* Object "a lamp." under token "mp." (0x80), kept alive by messages. */
static Str *encode_dot(Arena *a, int classic_objects)
{
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts = ts_new(a, "mp.", NULL);
    long savings = 0;

    add_msg(a, adv->messages, "mp.mp.mp.mp.");
    add_msg(a, adv->objects, "a lamp.");
    CHECK(tokselect_compress(a, d, adv, ts, 0, 1, classic_objects,
                             &savings) != NULL);
    return vec_at_Message(adv->objects, 0)->Text;
}

TEST(classic_rule_no_dot_token)
{
    Arena *a = arena_new(0);
    Str *enc = encode_dot(a, 1);

    CHECK_INT((long)str_len(enc), 7);
    CHECK(str_len(enc) == 7 && memcmp(str_bytes(enc), "a lamp.", 7) == 0);
    arena_free(a);
}

TEST(classic_rule_off_unchanged)
{
    Arena *a = arena_new(0);
    Str *enc = encode_a_lamp(a, 0);
    static const unsigned char want_dot[5] = { 'a', ' ', 'l', 'a', 0x80 };

    CHECK_INT((long)str_len(enc), 1);
    CHECK(str_len(enc) == 1 && str_bytes(enc)[0] == 0x80);
    enc = encode_dot(a, 0);
    CHECK_INT((long)str_len(enc), 5);
    CHECK(str_len(enc) == 5 && memcmp(str_bytes(enc), want_dot, 5) == 0);
    arena_free(a);
}

TEST(classic_rule_no_space_stays_literal)
{
    /* No space: the whole object text is literal under the rule. */
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts = ts_new(a, "lamp", NULL);
    long savings = 0;

    add_msg(a, adv->messages, "lamplamplamp");
    add_msg(a, adv->objects, "lamp");
    CHECK(tokselect_compress(a, d, adv, ts, 0, 1, 1, &savings) != NULL);
    CHECK(memcmp(str_bytes(vec_at_Message(adv->objects, 0)->Text),
                 "lamp", 4) == 0);
    CHECK_INT((long)str_len(vec_at_Message(adv->objects, 0)->Text), 4);
    arena_free(a);
}

TEST(classic_rule_select_skips_prefix_and_dot)
{
    /* Only object texts repeat: no selected token may come from the
       protected first word or contain '.'. */
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts;
    size_t i;
    int k;

    add_msg(a, adv->messages, "x");
    for (k = 0; k < 6; k++) add_msg(a, adv->objects, "zq brass lamp.");
    ts = tokselect_run(a, d, adv, 1, 1, 1);
    CHECK(vec_len_Str(ts->tokens) >= 2);
    for (i = 1; i < vec_len_Str(ts->tokens); i++) {
        Str *t = vec_at_Str(ts->tokens, i);
        CHECK(memchr(str_bytes(t), '.', str_len(t)) == NULL);
        CHECK(memchr(str_bytes(t), 'z', str_len(t)) == NULL);
        CHECK(memchr(str_bytes(t), 'q', str_len(t)) == NULL);
    }
    ts = tokselect_run(a, d, adv, 1, 1, 0);
    CHECK(any_token_contains(ts, '.') || any_token_contains(ts, 'z'));
    arena_free(a);
}

/* Snapshot with up to two object texts (slot 4); other slots empty. */
static Vec_MsgTable *obj_snap(Arena *a, const char *t0, const char *t1)
{
    Adventure *adv = adv_new(a);
    if (t0 != NULL) add_msg(a, adv->objects, t0);
    if (t1 != NULL) add_msg(a, adv->objects, t1);
    return tokselect_snapshot(a, adv, 1);
}

/* refs[0] = objects at lookup; the other three are empty tables. */
static void obj_refs(DdbTableRef refs[4], long lookup, size_t count,
                     size_t checked)
{
    static const int slots[4] = { 4, 2, 1, 0 };
    static const char *names[4] = { "objects", "sysmess", "messages",
                                    "locations" };
    int i;
    for (i = 0; i < 4; i++) {
        refs[i].snap_index = slots[i];
        refs[i].lookup_addr = 0;
        refs[i].count = refs[i].checked = 0;
        refs[i].name = names[i];
    }
    refs[0].lookup_addr = lookup;
    refs[0].count = count;
    refs[0].checked = checked;
}

/* Token table [80][41 C2] at 0 (entry 1 = "AB"), text 0x80 0x0A at 10
   (stored XOR 0xFF), little-endian lookup words from 20. Returns len. */
static size_t ddb_build(unsigned char *b, const unsigned *words, size_t n)
{
    size_t i;
    memset(b, 0, 64);
    b[0] = 0x80; b[1] = 0x41; b[2] = 0xC2;
    b[10] = 0x80 ^ 0xFF; b[11] = 0x0A ^ 0xFF;
    for (i = 0; i < n; i++) {
        b[20 + 2 * i] = (unsigned char)(words[i] & 0xFF);
        b[21 + 2 * i] = (unsigned char)(words[i] >> 8);
    }
    return 20 + 2 * n;
}

TEST(verify_ddb_decodes_through_lookup)
{
    Arena *a = arena_new(0);
    unsigned char b[64];
    unsigned w[1] = { 10 };
    size_t len = ddb_build(b, w, 1), bad_index = 99, skipped = 99;
    DdbTableRef refs[4];
    int bad_ref = 99;

    obj_refs(refs, 20, 1, 1);
    CHECK_INT(tokselect_verify_ddb(obj_snap(a, "AB", NULL), b, len, 0, 0, 0,
                                   refs, &bad_ref, &bad_index, &skipped), 1);
    CHECK_INT((long)skipped, 0);
    arena_free(a);
}

TEST(verify_ddb_detects_mismatch)
{
    Arena *a = arena_new(0);
    unsigned char b[64];
    unsigned w[1] = { 10 };
    size_t len = ddb_build(b, w, 1), bad_index = 99, skipped = 99;
    DdbTableRef refs[4];
    int bad_ref = 99;

    obj_refs(refs, 20, 1, 1);
    CHECK_INT(tokselect_verify_ddb(obj_snap(a, "AC", NULL), b, len, 0, 0, 0,
                                   refs, &bad_ref, &bad_index, &skipped), 0);
    CHECK_INT(bad_ref, 0);
    CHECK_INT((long)bad_index, 0);
    arena_free(a);
}

TEST(verify_ddb_shared_words)
{
    Arena *a = arena_new(0);
    unsigned char b[64];
    unsigned w[2] = { 10, 10 };
    size_t len = ddb_build(b, w, 2), bad_index = 99, skipped = 99;
    DdbTableRef refs[4];
    int bad_ref = 99;

    obj_refs(refs, 20, 2, 2);
    CHECK_INT(tokselect_verify_ddb(obj_snap(a, "AB", "AB"), b, len, 0, 0, 0,
                                   refs, &bad_ref, &bad_index, &skipped), 1);
    arena_free(a);
}

TEST(verify_ddb_empty_table)
{
    Arena *a = arena_new(0);
    unsigned char b[64];
    size_t len = ddb_build(b, NULL, 0), bad_index = 99, skipped = 99;
    DdbTableRef refs[4];
    int bad_ref = 99;

    obj_refs(refs, 20, 0, 0);
    CHECK_INT(tokselect_verify_ddb(obj_snap(a, NULL, NULL), b, len, 0, 0, 0,
                                   refs, &bad_ref, &bad_index, &skipped), 1);
    CHECK_INT((long)skipped, 0);
    arena_free(a);
}

TEST(verify_ddb_skips_dumped)
{
    Arena *a = arena_new(0);
    unsigned char b[64];
    unsigned w[2] = { 10, 0x7000 };   /* second word: an XMB offset */
    size_t len = ddb_build(b, w, 2), bad_index = 99, skipped = 99;
    DdbTableRef refs[4];
    int bad_ref = 99;

    obj_refs(refs, 20, 2, 1);
    CHECK_INT(tokselect_verify_ddb(obj_snap(a, "AB", "XYZ"), b, len, 0, 0, 0,
                                   refs, &bad_ref, &bad_index, &skipped), 1);
    CHECK_INT((long)skipped, 1);
    arena_free(a);
}

TEST(verify_ddb_classic_padding)
{
    Arena *a = arena_new(0);
    unsigned char b[64];
    unsigned w[1] = { 10 };
    size_t len = ddb_build(b, w, 1), bad_index = 99, skipped = 99;
    DdbTableRef refs[4];
    int bad_ref = 99;

    b[3] = b[4] = b[5] = 0xA0;   /* padding entries: literal spaces */
    obj_refs(refs, 20, 1, 1);
    CHECK_INT(tokselect_verify_ddb(obj_snap(a, "AB", NULL), b, len, 0, 0, 0,
                                   refs, &bad_ref, &bad_index, &skipped), 1);
    arena_free(a);
}

TEST(verify_ddb_base_address)
{
    Arena *a = arena_new(0);
    unsigned char b[64];
    unsigned w[1] = { 0x80B };
    size_t len = ddb_build(b, w, 1), bad_index = 99, skipped = 99;
    DdbTableRef refs[4];
    int bad_ref = 99;

    obj_refs(refs, 0x801 + 20, 1, 1);
    CHECK_INT(tokselect_verify_ddb(obj_snap(a, "AB", NULL), b, len, 0x801,
                                   0x801, 0, refs, &bad_ref, &bad_index,
                                   &skipped), 1);
    arena_free(a);
}

TEST(verify_ddb_big_endian)
{
    Arena *a = arena_new(0);
    unsigned char b[64];
    unsigned w[1] = { 0x0A00 };   /* bytes 0A 00: big-endian 10 */
    size_t len = ddb_build(b, w, 1), bad_index = 99, skipped = 99;
    DdbTableRef refs[4];
    int bad_ref = 99;

    obj_refs(refs, 20, 1, 1);
    CHECK_INT(tokselect_verify_ddb(obj_snap(a, "AB", NULL), b, len, 0, 0, 1,
                                   refs, &bad_ref, &bad_index, &skipped), 1);
    arena_free(a);
}

TEST(verify_ddb_overrun_is_failure)
{
    Arena *a = arena_new(0);
    unsigned char b[64];
    unsigned w[1] = { 10 };
    size_t bad_index = 99, skipped = 99;
    DdbTableRef refs[4];
    int bad_ref = 99;

    /* Lookup word at 4, unterminated "AB" at 10-11, buffer ends at 12. */
    ddb_build(b, NULL, 0);
    b[4] = (unsigned char)w[0]; b[5] = 0;
    b[10] = 0x41 ^ 0xFF; b[11] = 0x42 ^ 0xFF;
    obj_refs(refs, 4, 1, 1);
    CHECK_INT(tokselect_verify_ddb(obj_snap(a, "AB", NULL), b, 12, 0, 0, 0,
                                   refs, &bad_ref, &bad_index, &skipped), 0);
    CHECK_INT(bad_ref, 0);
    CHECK_INT((long)bad_index, 0);
    arena_free(a);
}

/* Format 2: part A b (byte 0 = 0x82, object lookup words at 240) and
   region rg (text block at 0, alphabet 256, coded streams from 200).
   sym[t] is text t's symbol list ending in 10; lookup words are region
   offsets. Returns part A's length; fills *fp and *rlen. */
#define F2_TEXT 200
#define F2_LOOKUP 240
static size_t f2_build(unsigned char *b, unsigned char *rg,
                       const char **entries, unsigned ntok,
                       const unsigned char *lengths,
                       const unsigned short *const *sym, size_t ntext,
                       unsigned *fp, size_t *rlen)
{
    Arena *a = arena_new(0);
    unsigned short codes[256];
    size_t i, at = F2_TEXT, off = 7 + 128 + 2 * (size_t)ntok, p;
    unsigned char *k = rg;

    memset(b, 0, 512);
    memset(rg, 0, 512);
    b[0] = 0x82;
    k[0] = 'H'; k[1] = '2'; k[2] = 0; k[3] = (unsigned char)ntok; k[4] = 0;
    for (i = 0; i < 256; i++)
        k[7 + i / 2] |= (unsigned char)(lengths[i] << (i % 2 ? 4 : 0));
    *fp = crc16_ccitt(k + 7, 128);
    k[5] = (unsigned char)(*fp & 0xFF); k[6] = (unsigned char)(*fp >> 8);
    for (i = 0, p = off; i < ntok; i++) {
        size_t n = strlen(entries[i]);
        k[7 + 128 + 2 * i] = (unsigned char)p;
        k[8 + 128 + 2 * i] = (unsigned char)(p >> 8);
        k[p] = (unsigned char)n;
        memcpy(k + p + 1, entries[i], n);
        p += n + 1;
    }
    huff_codes(lengths, 256, codes);
    for (i = 0; i < ntext; i++) {
        Str *s = str_new(a);
        BitWriter w;
        size_t j;
        bw_init(&w, s);
        for (j = 0; ; j++) {
            bw_put(&w, codes[sym[i][j]], lengths[sym[i][j]]);
            if (sym[i][j] == 10) break;
        }
        bw_flush(&w);
        b[F2_LOOKUP + 2 * i] = (unsigned char)at;
        b[F2_LOOKUP + 2 * i + 1] = (unsigned char)(at >> 8);
        memcpy(rg + at, str_bytes(s), str_len(s));
        at += str_len(s);
    }
    arena_free(a);
    *rlen = at;
    return F2_LOOKUP + 2 * ntext;
}

TEST(verify_region_detects_mismatch)
{
    Arena *a = arena_new(0);
    static const char *ent[1] = { "AB" };
    static const unsigned short s0[] = { 128, 'C', 10 };
    const unsigned short *syms[1] = { s0 };
    unsigned char b[512], rg[512], lengths[256];
    unsigned fp;
    size_t len, rlen, bad_index = 99, skipped = 99;
    DdbTableRef refs[4];
    int bad_ref = 99;

    memset(lengths, 0, sizeof lengths);
    lengths[128] = 1; lengths['C'] = 2; lengths[10] = 2;
    len = f2_build(b, rg, ent, 1, lengths, syms, 1, &fp, &rlen);
    obj_refs(refs, F2_LOOKUP, 1, 1);
    CHECK_INT(tokselect_verify_region(obj_snap(a, "ABC", NULL), b, len, 0,
                                      rg, rlen, 0, refs, &bad_ref,
                                      &bad_index, &skipped), 1);
    CHECK_INT((long)skipped, 0);

    /* One flipped stream bit. */
    rg[F2_TEXT] ^= 0x40;
    CHECK_INT(tokselect_verify_region(obj_snap(a, "ABC", NULL), b, len, 0,
                                      rg, rlen, 0, refs, &bad_ref,
                                      &bad_index, &skipped), 0);
    CHECK_INT(bad_ref, 0);
    CHECK_INT((long)bad_index, 0);
    rg[F2_TEXT] ^= 0x40;

    /* A lookup word past the region. */
    b[F2_LOOKUP] = (unsigned char)rlen;
    b[F2_LOOKUP + 1] = (unsigned char)(rlen >> 8);
    bad_ref = 99;
    CHECK_INT(tokselect_verify_region(obj_snap(a, "ABC", NULL), b, len, 0,
                                      rg, rlen, 0, refs, &bad_ref,
                                      &bad_index, &skipped), 0);
    CHECK_INT(bad_ref, 0);

    /* Lookup table under a base address; words stay region offsets. */
    b[F2_LOOKUP] = (unsigned char)F2_TEXT;
    b[F2_LOOKUP + 1] = 0;
    obj_refs(refs, F2_LOOKUP + 0x801, 1, 1);
    CHECK_INT(tokselect_verify_region(obj_snap(a, "ABC", NULL), b, len, 0x801,
                                      rg, rlen, 0, refs, &bad_ref,
                                      &bad_index, &skipped), 1);
    arena_free(a);
}

TEST(verify_region_roundtrip)
{
    Arena *a = arena_new(0);
    static const char *ent[1] = { "AB" };
    static const unsigned short s0[] = { 128, 'C', 10 }, s1[] = { 'C', 128, 10 };
    const unsigned short *syms[2] = { s0, s1 };
    unsigned char b[512], rg[512], lengths[256], x[16];
    unsigned fp;
    size_t len, rlen, bad_index = 99, skipped = 99;
    long xbad = 99;
    long xoff[1] = { 4 };
    DdbTableRef refs[4];
    int bad_ref = 99;
    Adventure *adv = adv_new(a);
    Vec_MsgTable *snap;

    memset(lengths, 0, sizeof lengths);
    lengths[128] = 1; lengths['C'] = 2; lengths[10] = 2;
    len = f2_build(b, rg, ent, 1, lengths, syms, 2, &fp, &rlen);
    obj_refs(refs, F2_LOOKUP, 2, 2);
    add_msg(a, adv->objects, "ABC");
    add_msg(a, adv->objects, "CAB");
    add_msg(a, adv->xmessages, "ABC");
    snap = tokselect_snapshot(a, adv, 1);
    CHECK_INT(tokselect_verify_region(snap, b, len, 0, rg, rlen, 0, refs,
                                      &bad_ref, &bad_index, &skipped), 1);

    /* XMB: "X2" + fingerprint, then text 0's stream at 4. */
    x[0] = 'X'; x[1] = '2';
    x[2] = (unsigned char)(fp & 0xFF); x[3] = (unsigned char)(fp >> 8);
    memcpy(x + 4, rg + F2_TEXT, 1);
    CHECK_INT(tokselect_verify_xmb2(snap, rg, rlen, 0, 0, x, 5, xoff,
                                    &xbad), 1);
    x[4] ^= 0x80;
    CHECK_INT(tokselect_verify_xmb2(snap, rg, rlen, 0, 0, x, 5, xoff,
                                    &xbad), 0);
    CHECK_INT(xbad, 0);
    x[4] ^= 0x80; x[2] ^= 1;
    CHECK_INT(tokselect_verify_xmb2(snap, rg, rlen, 0, 0, x, 5, xoff,
                                    &xbad), 0);
    CHECK_INT(xbad, -1);

    /* One flipped bit in text 1: objects, index 1. */
    rg[F2_TEXT + 1] ^= 0x80;
    CHECK_INT(tokselect_verify_region(snap, b, len, 0, rg, rlen, 0, refs,
                                      &bad_ref, &bad_index, &skipped), 0);
    CHECK_INT(bad_ref, 0);
    CHECK_STR(refs[bad_ref < 0 ? 0 : bad_ref].name, "objects");
    CHECK_INT((long)bad_index, 1);
    rg[F2_TEXT + 1] ^= 0x80;

    /* Fingerprint no longer matches the lengths: block fault. */
    rg[5] ^= 1;
    bad_ref = 99;
    CHECK_INT(tokselect_verify_region(snap, b, len, 0, rg, rlen, 0, refs,
                                      &bad_ref, &bad_index, &skipped), 0);
    CHECK_INT(bad_ref, -1);
    arena_free(a);
}

TEST(verify_region_nested_entry)
{
    Arena *a = arena_new(0);
    /* Token 1 = token 0 ("AB") + "CD"; token 0 need not precede it. */
    const char *ent[2] = { "AB", "\x80" "CD" };
    static const unsigned short s0[] = { 129, 'X', 10 };
    const unsigned short *syms[1] = { s0 };
    unsigned char b[512], rg[512], lengths[256];
    unsigned fp;
    size_t len, rlen, bad_index = 99, skipped = 99;
    DdbTableRef refs[4];
    int bad_ref = 99;

    memset(lengths, 0, sizeof lengths);
    lengths[128] = 2; lengths[129] = 2; lengths['X'] = 2; lengths[10] = 2;
    len = f2_build(b, rg, ent, 2, lengths, syms, 1, &fp, &rlen);
    obj_refs(refs, F2_LOOKUP, 1, 1);
    CHECK_INT(tokselect_verify_region(obj_snap(a, "ABCDX", NULL), b, len,
                                      0, rg, rlen, 0, refs, &bad_ref,
                                      &bad_index, &skipped), 1);

    /* A set pad bit after the end code. */
    rg[F2_TEXT] |= 1;
    CHECK_INT(tokselect_verify_region(obj_snap(a, "ABCDX", NULL), b, len,
                                      0, rg, rlen, 0, refs, &bad_ref,
                                      &bad_index, &skipped), 0);

    /* Token 0 referencing token 1: the target holds a reference. */
    ent[0] = "\x81";
    len = f2_build(b, rg, ent, 2, lengths, syms, 1, &fp, &rlen);
    CHECK_INT(tokselect_verify_region(obj_snap(a, "ABCDX", NULL), b, len,
                                      0, rg, rlen, 0, refs, &bad_ref,
                                      &bad_index, &skipped), 0);
    CHECK_INT(bad_ref, -1);

    /* A reference past ntok. */
    ent[0] = "AB";
    ent[1] = "\x82" "CD";
    bad_ref = 99;
    len = f2_build(b, rg, ent, 2, lengths, syms, 1, &fp, &rlen);
    CHECK_INT(tokselect_verify_region(obj_snap(a, "ABCDX", NULL), b, len,
                                      0, rg, rlen, 0, refs, &bad_ref,
                                      &bad_index, &skipped), 0);
    CHECK_INT(bad_ref, -1);
    arena_free(a);
}

int main(void)
{
    RUN(parse_literal_only);
    RUN(parse_token_reduces);
    RUN(parse_is_optimal_not_greedy);
    RUN(parse_picks_cheaper_cover);
    RUN(parse_token_longer_than_string);
    RUN(select_finds_the_obvious_token);
    RUN(select_is_deterministic);
    RUN(select_orders_longest_first);
    RUN(select_placeholder_exclusion);
    RUN(select_empty_and_tiny_input);
    RUN(select_caps_at_128);
    RUN(snapshot_verify_roundtrip_and_tamper);
    RUN(verify_catches_wrong_table);
    RUN(loader_marker_gate);
    RUN(loader_objects_off_gate);
    RUN(write_tok_objects_off_roundtrip);
    RUN(selector_result_is_marked);
    RUN(write_tok_roundtrip);
    RUN(compress_optimal_beats_sequential_shape);
    RUN(compress_cost_matches_parse_total);
    RUN(compress_prunes_unused_token);
    RUN(compress_classic_padding);
    RUN(compress_orders_table_by_use);
    RUN(compress_guard_trips_past_128_tokens);
    RUN(select_picks_token_from_object_texts);
    RUN(select_objects_empty_is_noop);
    RUN(snapshot_verify_covers_objects);
    RUN(compress_savings_include_objects);
    RUN(select_is_deterministic_with_objects);
    RUN(protected_prefix_cases);
    RUN(classic_rule_keeps_prefix_literal);
    RUN(classic_rule_no_dot_token);
    RUN(classic_rule_off_unchanged);
    RUN(classic_rule_no_space_stays_literal);
    RUN(classic_rule_select_skips_prefix_and_dot);
    RUN(verify_ddb_decodes_through_lookup);
    RUN(verify_ddb_detects_mismatch);
    RUN(verify_ddb_shared_words);
    RUN(verify_ddb_empty_table);
    RUN(verify_ddb_skips_dumped);
    RUN(verify_ddb_classic_padding);
    RUN(verify_ddb_base_address);
    RUN(verify_ddb_big_endian);
    RUN(verify_ddb_overrun_is_failure);
    RUN(verify_region_roundtrip);
    RUN(verify_region_nested_entry);
    RUN(verify_region_detects_mismatch);
    return test_summary("tokselect");
}
