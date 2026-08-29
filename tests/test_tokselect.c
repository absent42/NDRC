/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_tokselect.c - unit tests for src/tokselect.c.
   Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "../src/tokselect.h"

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

static Adventure *adv_new(Arena *a)
{
    Adventure *adv = arena_calloc(a, sizeof *adv);
    adv->messages = vec_new_Message(a);
    adv->sysmess = vec_new_Message(a);
    adv->locations = vec_new_Message(a);
    adv->xmessages = vec_new_Message(a);
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
    ts = tokselect_run(a, d, adv, 0);
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
    CHECK(tokensets_equal(tokselect_run(a, d, adv, 0),
                          tokselect_run(a, d, adv, 0)));
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
    ts = tokselect_run(a, d, adv, 0);
    CHECK(vec_len_Str(ts->tokens) >= 3);
    /* entry 0 is the 0x00 byte; from entry 1 on lengths never grow */
    for (i = 2; i < vec_len_Str(ts->tokens); i++) {
        CHECK(str_len(vec_at_Str(ts->tokens, i)) <=
              str_len(vec_at_Str(ts->tokens, i - 1)));
    }
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
    CHECK(any_token_contains(tokselect_run(a, d, adv1, 0), '_'));
    CHECK(!any_token_contains(tokselect_run(a, d, adv2, 1), '_'));
}

TEST(select_empty_and_tiny_input)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts;

    ts = tokselect_run(a, d, adv, 0);      /* no text at all */
    CHECK_INT((long)vec_len_Str(ts->tokens), 1);
    CHECK_INT(ts->has_tokens, 1);

    add_msg(a, adv->messages, "A");        /* below min candidate len */
    ts = tokselect_run(a, d, adv, 0);
    CHECK_INT((long)vec_len_Str(ts->tokens), 1);
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
    ts = tokselect_run(a, d, adv, 0);
    CHECK_INT((long)vec_len_Str(ts->tokens), 129);   /* 0x00 + 128 cap */
    for (k = 1; k < vec_len_Str(ts->tokens); k++) {
        CHECK(str_len(vec_at_Str(ts->tokens, k)) >= 2);
        CHECK(str_len(vec_at_Str(ts->tokens, k)) <= 12);
    }
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

    ts = tokselect_run(a, d, adv, 0);
    before = tokselect_snapshot(a, adv);
    final_tokens = tokens_compress(a, d, adv, ts, 0, &savings);

    CHECK_INT(tokselect_verify(before, adv, final_tokens), 1);

    /* Tampered text must fail the check. */
    str_push(vec_at_Message(adv->messages, 0)->Text, 'Z');
    CHECK_INT(tokselect_verify(before, adv, final_tokens), 0);
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
    ts = tokselect_run(a, d, adv, 0);
    before = tokselect_snapshot(a, adv);
    final_tokens = tokens_compress(a, d, adv, ts, 0, &savings);
    CHECK_INT(tokselect_verify(before, adv, final_tokens), 1);

    /* Two independent winners (AB, CD) must survive - assert it, so a
       selector change cannot quietly hollow this test out. */
    CHECK(vec_len_Str(final_tokens) >= 3);
    if (vec_len_Str(final_tokens) >= 3) {
        /* Swap two surviving tokens: references now expand wrongly. */
        Str *t1 = vec_at_Str(final_tokens, 1);
        Str *t2 = vec_at_Str(final_tokens, 2);
        vec_set_Str(final_tokens, 1, t2);
        vec_set_Str(final_tokens, 2, t1);
        CHECK_INT(tokselect_verify(before, adv, final_tokens), 0);
    }
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
}

TEST(selector_result_is_marked)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure *adv = adv_new(a);
    TokenSet *ts;

    add_msg(a, adv->messages, "MNMNMNMNMNMN");
    ts = tokselect_run(a, d, adv, 0);
    CHECK_INT(ts->optimal_encode, 1);
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
    ts = tokselect_run(a, d, adv, 0);
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
    RUN(selector_result_is_marked);
    RUN(write_tok_roundtrip);
    return test_summary("tokselect");
}
