/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_tokens.c - Copyright (C) 2026 Dan Gibson.

   Suite `tokens`: hex2str, the builtin token tables and their
   language selection, the two-pass compressor's savings arithmetic,
   and token emission. Diagnostics are
   captured the way test_model.c does: redirect Diag's stream to a
   tmpfile and compare the exact bytes. */
#include "test.h"
#include "arena.h"
#include "diag.h"
#include "model.h"
#include "str.h"
#include "tokens.h"
#include "vec.h"

#include <stdio.h>
#include <string.h>

static FILE *scratch_open(void)
{
    return tmpfile();
}

static void scratch_read(FILE *f, char *buf, size_t n)
{
    size_t got;
    rewind(f);
    got = fread(buf, 1, n - 1, f);
    buf[got] = '\0';
}

static Message *make_message(Arena *a, const char *text)
{
    Message *m = arena_calloc(a, sizeof(*m));
    m->Value = 0;
    m->Text = str_from(a, text);
    m->originalText = m->Text;
    return m;
}

/* ===================================================================
   hex2str: drb.php:300-307. */

TEST(hex2str_round_trips_the_golden_example)
{
    Arena *a = arena_new(0);
    /* EN token[1], drb.php:137. */
    Str *s = tokens_hex2str(a, "2074686520", strlen("2074686520"));
    static const unsigned char want[5] = { 0x20, 0x74, 0x68, 0x65, 0x20 };
    CHECK_INT(str_len(s), 5);
    CHECK_MEM(str_bytes(s), want, 5);
    arena_free(a);
}

TEST(hex2str_drops_a_trailing_unpaired_digit)
{
    /* PORT NOTE reproduced: PHP's loop bound is strlen($hex)-1, so an
       odd-length hex string silently drops its last digit rather than
       erroring. "abc" (3 chars) decodes only the "ab" pair. */
    Arena *a = arena_new(0);
    Str *s = tokens_hex2str(a, "abc", 3);
    static const unsigned char want[1] = { 0xAB };
    CHECK_INT(str_len(s), 1);
    CHECK_MEM(str_bytes(s), want, 1);
    arena_free(a);
}

TEST(hex2str_of_empty_string_is_empty)
{
    Arena *a = arena_new(0);
    Str *s = tokens_hex2str(a, "", 0);
    CHECK_INT(str_len(s), 0);
    arena_free(a);
}

/* ===================================================================
   Builtin table selection: drb.php:1895-1901. */

TEST(builtin_en_loads_129_advanced_tokens)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    TokenSet *ts = tokens_load_builtin(a, d, "EN");

    CHECK(ts != NULL);
    CHECK_INT(diag_error_count(d), 0);
    CHECK_INT(ts->has_tokens, 1);
    CHECK_INT(ts->advanced, 1);
    CHECK_STR(ts->compression, "advanced");
    CHECK_INT(vec_len_Str(ts->tokens), 129);

    {
        /* token[0] is hex "00" -> the single NUL byte, drb.php:137. */
        Str *t0 = vec_at_Str(ts->tokens, 0);
        static const unsigned char zero = 0x00;
        CHECK_INT(str_len(t0), 1);
        CHECK_MEM(str_bytes(t0), &zero, 1);
    }
    {
        /* token[1] is hex "2074686520" -> " the ", same as the
           hex2str test above; cross-checks the JSON array walk picks
           tokens up in order. */
        Str *t1 = vec_at_Str(ts->tokens, 1);
        static const unsigned char want[5] = { 0x20, 0x74, 0x68, 0x65, 0x20 };
        CHECK_INT(str_len(t1), 5);
        CHECK_MEM(str_bytes(t1), want, 5);
    }
    arena_free(a);
}

TEST(builtin_es_loads_128_advanced_tokens_with_correct_second_entry)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    TokenSet *ts = tokens_load_builtin(a, d, "ES");

    CHECK(ts != NULL);
    CHECK_INT(diag_error_count(d), 0);
    CHECK_INT(ts->has_tokens, 1);
    CHECK_INT(ts->advanced, 1);
    CHECK_STR(ts->compression, "advanced");
    CHECK_INT(vec_len_Str(ts->tokens), 128);

    {
        /* token[1] is hex "2071756520" -> " que " (drb.php:135, the
           ES table's second entry - the header is transcribed by
           script from that exact line, see tokens_es.h). */
        Str *t1 = vec_at_Str(ts->tokens, 1);
        static const unsigned char want[5] = { 0x20, 0x71, 0x75, 0x65, 0x20 };
        CHECK_INT(str_len(t1), 5);
        CHECK_MEM(str_bytes(t1), want, 5);
    }
    arena_free(a);
}

TEST(builtin_selection_defaults_unknown_language_to_es)
{
    /* PORT NOTE (tokens.h): drb.php:1895-1901's switch has no case for
       anything but EN/PT/DE/FR, so any other value - including "ES"
       itself - falls to the default arm and gets the ES table. main.c
       never actually calls tokens_load_builtin with such a value (it
       validates the language against the five supported strings
       first), but tokens_load_builtin itself reproduces the quirk
       literally rather than special-casing it away. */
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    TokenSet *ts = tokens_load_builtin(a, d, "XX");

    CHECK(ts != NULL);
    CHECK_INT(diag_error_count(d), 0);
    CHECK_INT(vec_len_Str(ts->tokens), 128);
    {
        Str *t1 = vec_at_Str(ts->tokens, 1);
        static const unsigned char want[5] = { 0x20, 0x71, 0x75, 0x65, 0x20 };
        CHECK_INT(str_len(t1), 5);
        CHECK_MEM(str_bytes(t1), want, 5);
    }
    arena_free(a);
}

/* ===================================================================
   Synthetic two-pass compress, drb.php:144-241. Four hand-built
   tokens over a two-message fixture: tokens [0]="Z" [1]="zzz"
   [2]="quick" [3]="Q"; locations loc0.Text = "XquickQ", loc1.Text =
   "Yquick". The three rules that produce the expected values: first
   occurrence scores -1 (drb.php:174); token 0 is always kept
   (drb.php:182); pass two rewrites bytes using the FINAL post-drop
   index (drb.php:210-221). Hand-derived: savings = 3, final marker
   byte 0x80. */
TEST(compress_reproduces_progressive_savings_and_final_index_replacement)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure adv;
    TokenSet ts;
    Vec_Str *final_tokens;
    long savings = -12345;
    FILE *scratch = scratch_open();
    char captured[2048];

    diag_set_stream(d, scratch);
    diag_set_verbose(d, 1);

    memset(&adv, 0, sizeof(adv));
    adv.locations = vec_new_Message(a);
    vec_push_Message(adv.locations, make_message(a, "XquickQ"));
    vec_push_Message(adv.locations, make_message(a, "Yquick"));
    adv.messages = vec_new_Message(a);
    adv.sysmess = vec_new_Message(a);
    adv.xmessages = vec_new_Message(a);

    ts.has_tokens = 1;
    ts.advanced = 1;
    ts.compression = "advanced";
    ts.tokens = vec_new_Str(a);
    vec_push_Str(ts.tokens, str_from(a, "Z"));
    vec_push_Str(ts.tokens, str_from(a, "zzz"));
    vec_push_Str(ts.tokens, str_from(a, "quick"));
    vec_push_Str(ts.tokens, str_from(a, "Q"));

    final_tokens = tokens_compress(a, d, &adv, &ts, 0, &savings);

    CHECK_INT(savings, 3);
    CHECK_INT(vec_len_Str(final_tokens), 2);
    {
        Str *f0 = vec_at_Str(final_tokens, 0);
        Str *f1 = vec_at_Str(final_tokens, 1);
        CHECK_STR(str_cstr(f0), "Z");
        CHECK_STR(str_cstr(f1), "quick");
    }

    /* Real tables mutated in place, using FINAL index 1 (marker 0x80)
       for "quick", not original index 2 (marker 0x81). */
    {
        Message *m0 = vec_at_Message(adv.locations, 0);
        Message *m1 = vec_at_Message(adv.locations, 1);
        static const unsigned char want0[3] = { 'X', 0x80, 'Q' };
        static const unsigned char want1[2] = { 'Y', 0x80 };
        CHECK_INT(str_len(m0->Text), 3);
        CHECK_MEM(str_bytes(m0->Text), want0, 3);
        CHECK_INT(str_len(m1->Text), 2);
        CHECK_MEM(str_bytes(m1->Text), want1, 2);
    }

    scratch_read(scratch, captured, sizeof(captured));
    CHECK(strstr(captured,
        "Warning: token [zzz] won't be used cause it was not used by any text.\n") != NULL);
    CHECK(strstr(captured,
        "Warning: token [Q] won't be used cause using it wont save any bytes, but waste 1 byte.\n") != NULL);
    CHECK(strstr(captured, "Compression tokens used: 2.\n") != NULL);
    /* token "quick" was kept, so no "won't be used" warning names it. */
    CHECK(strstr(captured, "[quick]") == NULL);

    fclose(scratch);
    arena_free(a);
}

TEST(compress_with_no_tokens_leaves_tables_untouched)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure adv;
    TokenSet ts;
    Vec_Str *final_tokens;
    long savings = -1;

    memset(&adv, 0, sizeof(adv));
    adv.locations = vec_new_Message(a);
    vec_push_Message(adv.locations, make_message(a, "unchanged"));
    adv.messages = vec_new_Message(a);
    adv.sysmess = vec_new_Message(a);
    adv.xmessages = vec_new_Message(a);

    ts.has_tokens = 0;   /* compression == "none" */
    ts.advanced = 0;
    ts.compression = "none";
    ts.tokens = vec_new_Str(a);

    final_tokens = tokens_compress(a, d, &adv, &ts, 0, &savings);

    CHECK_INT(vec_len_Str(final_tokens), 0);
    CHECK_INT(savings, 0);
    {
        Message *m0 = vec_at_Message(adv.locations, 0);
        CHECK_STR(str_cstr(m0->Text), "unchanged");
    }
    arena_free(a);
}

/* ===================================================================
   Classic-mode pad, drb.php:202-206. Single token "AB" (token 0,
   never dropped, drb.php:182) over location "AB AB"; pad grows
   finalTokens from 1 to 128 single-space entries before pass two
   (drb.php:210-221) runs. The literal space is claimed by the FIRST
   filler (final index 1, marker 0x80): final text 0x7F 0x80 0x7F,
   no 0x20 left. */

TEST(compress_classic_mode_pads_to_128_and_replaces_before_padding_marker)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure adv;
    TokenSet ts;
    Vec_Str *final_tokens;
    long savings = -999;
    FILE *scratch = scratch_open();
    char captured[4096];

    diag_set_stream(d, scratch);
    diag_set_verbose(d, 1);

    memset(&adv, 0, sizeof(adv));
    adv.locations = vec_new_Message(a);
    vec_push_Message(adv.locations, make_message(a, "AB AB"));
    adv.messages = vec_new_Message(a);
    adv.sysmess = vec_new_Message(a);
    adv.xmessages = vec_new_Message(a);

    ts.has_tokens = 1;
    ts.advanced = 1;
    ts.compression = "advanced";
    ts.tokens = vec_new_Str(a);
    vec_push_Str(ts.tokens, str_from(a, "AB"));

    final_tokens = tokens_compress(a, d, &adv, &ts, 1 /* classic_mode */, &savings);

    /* (a) exactly 128 entries. */
    CHECK_INT(vec_len_Str(final_tokens), 128);

    /* (b) every entry beyond the single survivor is the single byte
       0x20. */
    {
        size_t k;
        for (k = 1; k < vec_len_Str(final_tokens); k++) {
            Str *filler = vec_at_Str(final_tokens, k);
            static const unsigned char space = 0x20;
            CHECK_INT(str_len(filler), 1);
            CHECK_MEM(str_bytes(filler), &space, 1);
        }
    }

    /* (c) location text after replacement has no 0x20 byte left - the
       literal space was claimed by the FIRST filler (final index 1,
       marker chr(1+127)=0x80), not any later one. */
    {
        Message *m0 = vec_at_Message(adv.locations, 0);
        static const unsigned char want[3] = { 0x7F, 0x80, 0x7F };
        size_t k;
        CHECK_INT(str_len(m0->Text), 3);
        CHECK_MEM(str_bytes(m0->Text), want, 3);
        for (k = 0; k < str_len(m0->Text); k++) {
            CHECK(str_bytes(m0->Text)[k] != 0x20);
        }
    }

    /* Verbose lines, in order: "Compression tokens used: 1." (survivor
       count, BEFORE the pad) then the classic-mode fill line. */
    scratch_read(scratch, captured, sizeof(captured));
    {
        const char *used = strstr(captured, "Compression tokens used: 1.\n");
        const char *fill = strstr(captured,
            "Filling tokens table up to 128 tokens for classic mode compatibility.\n");
        CHECK(used != NULL);
        CHECK(fill != NULL);
        CHECK(used < fill);
    }

    /* (d) tokens_emit: each filler emits as the single byte 0xA0. */
    {
        Str *out = str_new(a);
        long addr = 0;
        tokens_emit(out, final_tokens, &addr);
        /* token 0 "AB" emits as 0x41 0xC2 ('A', 'B'+128); each of the
           127 fillers then emits as the single byte 0xA0. */
        CHECK_INT(str_len(out), 2 + 127);
        CHECK_INT((unsigned char)str_bytes(out)[0], 0x41);
        CHECK_INT((unsigned char)str_bytes(out)[1], 0xC2);
        {
            size_t k;
            for (k = 2; k < str_len(out); k++) {
                CHECK_INT((unsigned char)str_bytes(out)[k], 0xA0);
            }
        }
    }

    fclose(scratch);
    arena_free(a);
}

TEST(compress_non_classic_mode_leaves_final_tokens_unpadded)
{
    /* (e) classic_mode=0, same shape of input: no pad, vec length ==
       survivor count (1, token 0 is never dropped). */
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure adv;
    TokenSet ts;
    Vec_Str *final_tokens;
    long savings = -999;

    memset(&adv, 0, sizeof(adv));
    adv.locations = vec_new_Message(a);
    vec_push_Message(adv.locations, make_message(a, "AB AB"));
    adv.messages = vec_new_Message(a);
    adv.sysmess = vec_new_Message(a);
    adv.xmessages = vec_new_Message(a);

    ts.has_tokens = 1;
    ts.advanced = 1;
    ts.compression = "advanced";
    ts.tokens = vec_new_Str(a);
    vec_push_Str(ts.tokens, str_from(a, "AB"));

    final_tokens = tokens_compress(a, d, &adv, &ts, 0 /* classic_mode */, &savings);

    CHECK_INT(vec_len_Str(final_tokens), 1);
    arena_free(a);
}

/* ===================================================================
   Basic compression arm: getCompressableTables, drb.php:310-319.
   "basic" pushes only adv->locations (drb.php:315); "advanced" pushes
   all four tables - locations, messages, sysmess, xmessages
   (drb.php:311-314). Proven by giving all four tables the identical
   literal "quick": under basic only locations is touched (replaced by
   the token-0 marker 0x7F), the other three come back byte-for-byte
   as authored. */
TEST(compress_basic_arm_compresses_locations_only)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Adventure adv;
    TokenSet ts;
    Vec_Str *final_tokens;
    long savings = -1;

    memset(&adv, 0, sizeof(adv));
    adv.locations = vec_new_Message(a);
    vec_push_Message(adv.locations, make_message(a, "XquickQ"));
    adv.messages = vec_new_Message(a);
    vec_push_Message(adv.messages, make_message(a, "a quick fox"));
    adv.sysmess = vec_new_Message(a);
    vec_push_Message(adv.sysmess, make_message(a, "quick"));
    adv.xmessages = vec_new_Message(a);
    vec_push_Message(adv.xmessages, make_message(a, "quick brown"));

    ts.has_tokens = 1;
    ts.advanced = 0;
    ts.compression = "basic";
    ts.tokens = vec_new_Str(a);
    vec_push_Str(ts.tokens, str_from(a, "quick"));

    final_tokens = tokens_compress(a, d, &adv, &ts, 0, &savings);

    /* token 0 is always kept (drb.php:182); it is the only token here,
       so no j=1.. loop ever runs and total_saving stays 0. */
    CHECK_INT(vec_len_Str(final_tokens), 1);
    CHECK_INT(savings, 0);

    /* Locations: substituted - "quick" -> chr(127) = 0x7F. */
    {
        Message *loc0 = vec_at_Message(adv.locations, 0);
        static const unsigned char want[3] = { 'X', 0x7F, 'Q' };
        CHECK_INT(str_len(loc0->Text), 3);
        CHECK_MEM(str_bytes(loc0->Text), want, 3);
    }

    /* Messages/sysmess/xmessages: NOT in the basic compressable set -
       untouched, "quick" still present literally. */
    {
        Message *msg0 = vec_at_Message(adv.messages, 0);
        Message *sys0 = vec_at_Message(adv.sysmess, 0);
        Message *xms0 = vec_at_Message(adv.xmessages, 0);
        CHECK_STR(str_cstr(msg0->Text), "a quick fox");
        CHECK_STR(str_cstr(sys0->Text), "quick");
        CHECK_STR(str_cstr(xms0->Text), "quick brown");
    }

    arena_free(a);
}

/* ===================================================================
   Emission: drb.php:224-236. */

TEST(emit_adds_128_to_each_tokens_last_byte)
{
    Arena *a = arena_new(0);
    Vec_Str *tokens = vec_new_Str(a);
    Str *out = str_new(a);
    long addr = 0x1000;
    /* token[0] = single 0x00 byte; token[1] = " the " (5 bytes), same
       hex2str decode as drb.php:137's EN token[1]. */
    Str *t0 = str_new(a);
    str_push(t0, (char)0x00);
    vec_push_Str(tokens, t0);
    vec_push_Str(tokens, tokens_hex2str(a, "2074686520", strlen("2074686520")));

    tokens_emit(out, tokens, &addr);

    {
        static const unsigned char want[6] = { 0x80, 0x20, 0x74, 0x68, 0x65, 0xA0 };
        CHECK_INT(str_len(out), 6);
        CHECK_MEM(str_bytes(out), want, 6);
    }
    CHECK_INT(addr, 0x1000 + 6);
    arena_free(a);
}

TEST(emit_with_no_tokens_writes_a_single_zero_byte)
{
    Arena *a = arena_new(0);
    Vec_Str *tokens = vec_new_Str(a);
    Str *out = str_new(a);
    long addr = 5;

    tokens_emit(out, tokens, &addr);

    CHECK_INT(str_len(out), 1);
    CHECK_INT(str_bytes(out)[0], 0);
    CHECK_INT(addr, 6);
    arena_free(a);
}

int main(void)
{
    RUN(hex2str_round_trips_the_golden_example);
    RUN(hex2str_drops_a_trailing_unpaired_digit);
    RUN(hex2str_of_empty_string_is_empty);
    RUN(builtin_en_loads_129_advanced_tokens);
    RUN(builtin_es_loads_128_advanced_tokens_with_correct_second_entry);
    RUN(builtin_selection_defaults_unknown_language_to_es);
    RUN(compress_reproduces_progressive_savings_and_final_index_replacement);
    RUN(compress_with_no_tokens_leaves_tables_untouched);
    RUN(compress_classic_mode_pads_to_128_and_replaces_before_padding_marker);
    RUN(compress_non_classic_mode_leaves_final_tokens_unpadded);
    RUN(compress_basic_arm_compresses_locations_only);
    RUN(emit_adds_128_to_each_tokens_last_byte);
    RUN(emit_with_no_tokens_writes_a_single_zero_byte);
    return test_summary("tokens");
}
