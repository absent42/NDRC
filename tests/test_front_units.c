/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_front_units.c - Copyright (C) 2026 Dan Gibson.

   Shared suite for Tasks 4-5. Task 4's share: symbols.c, labels.c,
   constants.h, and str_upper_latin1 (str.h/str.c). Task 5's share:
   condacts.c, voctree.c, messagelist.c, objects.c, connections.c,
   process.c. */
#include "test.h"
#include "arena.h"
#include "diag.h"
#include "str.h"
#include "../src/front/symbols.h"
#include "../src/front/labels.h"
#include "../src/front/constants.h"
#include "../src/front/condacts.h"
#include "../src/front/voctree.h"
#include "../src/front/messagelist.h"
#include "../src/front/objects.h"
#include "../src/front/connections.h"
#include "../src/front/process.h"

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

/* ---- str_upper_latin1 ---- */

TEST(upper_latin1_folds_ascii_letters)
{
    Arena *a = arena_new(0);
    CHECK_STR(str_upper_latin1(a, "verb"), "VERB");
    CHECK_STR(str_upper_latin1(a, "MiXeD123_"), "MIXED123_");
    arena_free(a);
}

/* The eight pairs live-probed against drf.exe (report has the probe
   table): e1/c1 e9/c9 ed/cd f3/d3 fa/da fc/dc f1/d1 e7/c7. */
TEST(upper_latin1_folds_all_eight_accent_pairs)
{
    Arena *a = arena_new(0);
    struct { unsigned char lo, hi; } pairs[] = {
        {0xE1, 0xC1}, {0xE9, 0xC9}, {0xED, 0xCD}, {0xF3, 0xD3},
        {0xFA, 0xDA}, {0xFC, 0xDC}, {0xF1, 0xD1}, {0xE7, 0xC7},
    };
    size_t i;
    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        char in[2], want[2];
        char *got;
        in[0] = (char)pairs[i].lo; in[1] = '\0';
        want[0] = (char)pairs[i].hi; want[1] = '\0';
        got = str_upper_latin1(a, in);
        CHECK_INT((unsigned char)got[0], (unsigned char)want[0]);
    }
    arena_free(a);
}

/* A live re-probe via the CLI AdditionalSymbols path (drf.pas:292-300,
   which bypasses the lexer entirely) showed AnsiUpperCase folds the
   WHOLE 0xE0-0xFF lowercase-accented block under cp1252 semantics, not
   just eight bytes - see str.h's str_upper_latin1 doc comment for the
   full measured table and probe method. This test re-derives the same
   table (flat -0x20 shift for 0xE0-0xFE except 0xF7, and the
   0xFF->0x9F irregular case) and checks every one of the 32 bytes
   individually, not just the original 8. */
TEST(upper_latin1_folds_the_full_measured_range)
{
    Arena *a = arena_new(0);
    unsigned char b;
    for (b = 0xE0; ; b++) {
        char in[2];
        char *got;
        unsigned char want;

        in[0] = (char)b; in[1] = '\0';
        got = str_upper_latin1(a, in);

        if (b == 0xF7) want = 0xF7;      /* division sign: no case */
        else if (b == 0xFF) want = 0x9F; /* y-diaeresis: cp1252 irregular target */
        else want = (unsigned char)(b - 0x20);

        CHECK_INT((unsigned char)got[0], want);
        if (b == 0xFF) break; /* avoid wrapping an unsigned char loop counter */
    }
    arena_free(a);
}

/* Explicit pin for the single most surprising measured pair: 0xFF
   (y-diaeresis) does NOT land in the expected 0xC0-0xDE row a flat
   -0x20 shift would produce (that would be 0xDF); cp1252 has no
   single-byte uppercase Y-diaeresis there, so AnsiUpperCase folds it
   to 0x9F instead, cp1252's sole slot for that letter. */
TEST(upper_latin1_ff_folds_to_cp1252_9f_not_df)
{
    Arena *a = arena_new(0);
    char in[2] = { (char)0xFF, '\0' };
    char *got = str_upper_latin1(a, in);
    CHECK_INT((unsigned char)got[0], 0x9F);
    CHECK((unsigned char)got[0] != 0xDF);
    arena_free(a);
}

/* A mid-block pair well outside the original 8: 0xE0 (a-grave), which
   the pre-fix-round table left as identity, folds to 0xC0 (A-grave). */
TEST(upper_latin1_e0_folds_to_c0)
{
    Arena *a = arena_new(0);
    char in[2] = { (char)0xE0, '\0' };
    char *got = str_upper_latin1(a, in);
    CHECK_INT((unsigned char)got[0], 0xC0);
    arena_free(a);
}

/* The one byte in 0xE0-0xFF the probe shows genuinely UNFOLDED: 0xF7,
   the division sign, has no letter case at all. This pins the boundary
   that remains after the fix-round widening - not every byte in the
   block folds. */
TEST(upper_latin1_f7_division_sign_has_no_case)
{
    Arena *a = arena_new(0);
    char in[2] = { (char)0xF7, '\0' };
    char *got = str_upper_latin1(a, in);
    CHECK_INT((unsigned char)got[0], 0xF7);
    arena_free(a);
}

/* Bytes outside 0xE0-0xFF (both the already-uppercase 0xC0-0xDE row
   and anything below 0xE0/above 0xFF) remain identity - the fold's
   domain is exactly the measured lowercase block, nothing wider. */
TEST(upper_latin1_bytes_outside_the_block_are_identity)
{
    Arena *a = arena_new(0);
    char in_df[2] = { (char)0xDF, '\0' }; /* sharp s: just below the block */
    char *got = str_upper_latin1(a, in_df);
    CHECK_INT((unsigned char)got[0], 0xDF);
    arena_free(a);
}

TEST(upper_latin1_leaves_already_uppercase_accents_unchanged)
{
    Arena *a = arena_new(0);
    char in[2] = { (char)0xC9, '\0' }; /* E-acute, already upper */
    char *got = str_upper_latin1(a, in);
    CHECK_INT((unsigned char)got[0], 0xC9);
    arena_free(a);
}

/* ---- symbols.c ---- */

TEST(symbols_add_then_lookup_roundtrips)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    long v = -1;
    CHECK_INT(symbols_add(sl, a, d, "fDark", 0), 1);
    CHECK_INT(symbols_lookup(sl, a, "fDark", &v), 1);
    CHECK_INT(v, 0);
    arena_free(a);
}

TEST(symbols_lookup_miss_returns_zero)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    long v = 12345;
    (void)d;
    CHECK_INT(symbols_lookup(sl, a, "NOPE", &v), 0);
    CHECK_INT(v, 12345); /* untouched on a miss */
    arena_free(a);
}

TEST(symbols_add_folds_ascii_case_and_lookup_is_case_insensitive)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    long v = 0;
    CHECK_INT(symbols_add(sl, a, d, "score", 30), 1);
    CHECK_INT(symbols_lookup(sl, a, "SCORE", &v), 1);
    CHECK_INT(v, 30);
    CHECK_INT(symbols_lookup(sl, a, "ScOrE", &v), 1);
    CHECK_INT(v, 30);
    arena_free(a);
}

/* The Latin-1 fold requirement (brief step 2): "ca e-acute" and
   "CA E-acute" (byte 0xE9 vs 0xC9) must collide as the same symbol,
   exactly the way AddSymbol rejects a duplicate. */
TEST(symbols_fold_collides_lowercase_and_uppercase_accented_symbol)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    char lower[4]; /* "CA" + e-acute */
    char upper[4]; /* "CA" + E-acute */
    long v = -1;

    lower[0] = 'c'; lower[1] = 'a'; lower[2] = (char)0xE9; lower[3] = '\0';
    upper[0] = 'C'; upper[1] = 'A'; upper[2] = (char)0xC9; upper[3] = '\0';

    CHECK_INT(symbols_add(sl, a, d, lower, 7), 1);
    /* Same folded key, so this is a duplicate: rejected. */
    CHECK_INT(symbols_add(sl, a, d, upper, 999), 0);
    /* And a lookup under either spelling finds the FIRST value. */
    CHECK_INT(symbols_lookup(sl, a, upper, &v), 1);
    CHECK_INT(v, 7);
    CHECK_INT(symbols_lookup(sl, a, lower, &v), 1);
    CHECK_INT(v, 7);
    arena_free(a);
}

TEST(symbols_add_duplicate_keeps_original_value)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    long v = -1;
    CHECK_INT(symbols_add(sl, a, d, "fDark", 0), 1);
    CHECK_INT(symbols_add(sl, a, d, "FDARK", 999), 0);
    CHECK_INT(symbols_lookup(sl, a, "fDark", &v), 1);
    CHECK_INT(v, 0); /* unchanged - AddSymbol never overwrites */
    arena_free(a);
}

TEST(symbols_add_verbose_prints_folded_name)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    FILE *f = scratch_open();
    char buf[256];
    diag_set_stream(d, f);
    diag_set_verbose(d, 1);
    CHECK_INT(symbols_add(sl, a, d, "fDark", 0), 1);
    scratch_read(f, buf, sizeof(buf));
    CHECK_STR(buf, "Added Symbol: FDARK=0\n");
    fclose(f);
    arena_free(a);
}

TEST(symbols_add_verbose_silent_unless_enabled)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    FILE *f = scratch_open();
    char buf[256];
    diag_set_stream(d, f);
    /* verbose left off (default) */
    CHECK_INT(symbols_add(sl, a, d, "fDark", 0), 1);
    scratch_read(f, buf, sizeof(buf));
    CHECK_STR(buf, "");
    fclose(f);
    arena_free(a);
}

TEST(symbols_add_verbose_suppressed_for_voc_prefix)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    FILE *f = scratch_open();
    char buf[256];
    diag_set_stream(d, f);
    diag_set_verbose(d, 1);
    CHECK_INT(symbols_add(sl, a, d, "_VOC_TORCH", 100), 1);
    scratch_read(f, buf, sizeof(buf));
    CHECK_STR(buf, "");
    fclose(f);
    arena_free(a);
}

TEST(symbols_iteration_is_insertion_order)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    const char *name;
    long value;
    CHECK_INT(symbols_add(sl, a, d, "first", 1), 1);
    CHECK_INT(symbols_add(sl, a, d, "second", 2), 1);
    CHECK_INT(symbols_count(sl), 2);
    CHECK_INT(symbols_at(sl, 0, &name, &value), 1);
    CHECK_STR(name, "FIRST");
    CHECK_INT(value, 1);
    CHECK_INT(symbols_at(sl, 1, &name, &value), 1);
    CHECK_STR(name, "SECOND");
    CHECK_INT(value, 2);
    CHECK_INT(symbols_at(sl, 2, &name, &value), 0);
    arena_free(a);
}

/* ---- labels.c ---- */

TEST(labels_add_new_then_find)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    LabelTable *t = labels_new(a);
    LabelData out;
    long idx = labels_add(t, a, d, "$pictureOK", 3, 5, 0, -1);
    CHECK(idx >= 0);
    CHECK_INT(labels_find(t, "$pictureOK", &out), 1);
    CHECK_STR(out.skip_label, "$pictureOK");
    CHECK_INT(out.process, 3);
    CHECK_INT(out.entry, 5);
    CHECK_INT(out.is_forward, 0);
    CHECK_INT(out.condact, -1);
    arena_free(a);
}

/* Labels do NOT fold - case-sensitive, unlike symbols. */
TEST(labels_are_case_sensitive)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    LabelTable *t = labels_new(a);
    LabelData out;
    long idx_lo = labels_add(t, a, d, "$foo", 0, 1, 0, -1);
    long idx_hi = labels_add(t, a, d, "$FOO", 0, 2, 0, -1);
    CHECK(idx_lo >= 0);
    CHECK(idx_hi >= 0);
    CHECK(idx_lo != idx_hi); /* two distinct labels */
    CHECK_INT(labels_find(t, "$foo", &out), 1);
    CHECK_INT(out.entry, 1);
    CHECK_INT(labels_find(t, "$FOO", &out), 1);
    CHECK_INT(out.entry, 2);
    arena_free(a);
}

/* The '$' is part of the key: "$foo" and "foo" are unrelated. */
TEST(labels_key_includes_dollar_sign)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    LabelTable *t = labels_new(a);
    LabelData out;
    CHECK(labels_add(t, a, d, "$foo", 0, 1, 0, -1) >= 0);
    CHECK_INT(labels_find(t, "foo", &out), 0);
    arena_free(a);
}

TEST(labels_forward_reference_then_resolves)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    LabelTable *t = labels_new(a);
    LabelData out;
    long fwd_idx, real_idx;

    /* SKIP to a not-yet-defined label: AddLabel(text, -1, -1, true, -1). */
    fwd_idx = labels_add(t, a, d, "$initLoop", -1, -1, 1, -1);
    CHECK(fwd_idx >= 0);
    /* Still forward: GetLabelData does not find it. */
    CHECK_INT(labels_find(t, "$initLoop", &out), 0);

    /* A second SKIP to the same still-forward label: returns the SAME
       slot, unchanged. */
    CHECK_INT(labels_add(t, a, d, "$initLoop", -1, -1, 1, -1), fwd_idx);

    /* The real definition resolves it in place, same slot. */
    real_idx = labels_add(t, a, d, "$initLoop", 6, 4, 0, -1);
    CHECK_INT(real_idx, fwd_idx);
    CHECK_INT(labels_find(t, "$initLoop", &out), 1);
    CHECK_INT(out.process, 6);
    CHECK_INT(out.entry, 4);
    CHECK_INT(out.is_forward, 0);
    arena_free(a);
}

TEST(labels_duplicate_real_definition_rejected)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    LabelTable *t = labels_new(a);
    CHECK(labels_add(t, a, d, "$x", 0, 1, 0, -1) >= 0);
    CHECK_INT(labels_add(t, a, d, "$x", 0, 2, 0, -1), -1);
    arena_free(a);
}

TEST(labels_iteration_matches_insertion_order)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    LabelTable *t = labels_new(a);
    const LabelData *e;
    CHECK(labels_add(t, a, d, "$a", 0, 1, 0, -1) >= 0);
    CHECK(labels_add(t, a, d, "$b", 0, 2, 0, -1) >= 0);
    CHECK_INT(labels_count(t), 2);
    e = labels_at(t, 0);
    CHECK(e != NULL);
    CHECK_STR(e->skip_label, "$a");
    e = labels_at(t, 1);
    CHECK(e != NULL);
    CHECK_STR(e->skip_label, "$b");
    CHECK(labels_at(t, 2) == NULL);
    arena_free(a);
}

/* THE GUARD (ruled): table-full at MAX_LABELS FATALs with a clear
   diagnostic instead of reproducing 19.23's corruption. */
TEST(labels_add_fatals_when_table_is_full)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    LabelTable *t = labels_new(a);
    FILE *f = scratch_open();
    char name[32];
    int i;
    long idx;

    diag_set_stream(d, f);
    for (i = 0; i < MAX_LABELS; i++) {
        snprintf(name, sizeof(name), "$L%d", i);
        idx = labels_add(t, a, d, name, 0, i, 0, -1);
        CHECK(idx >= 0);
    }
    CHECK_INT(labels_count(t), MAX_LABELS);
    CHECK_INT(diag_error_count(d), 0);

    idx = labels_add(t, a, d, "$overflow", 0, 0, 0, -1);
    CHECK_INT(idx, -2);
    CHECK_INT(labels_count(t), MAX_LABELS); /* table unchanged */
    CHECK_INT(diag_error_count(d), 1);
    CHECK_INT(diag_exit_code(d), 2);
    fclose(f);
    arena_free(a);
}

/* ---- constants.h ---- */

TEST(constants_match_uconstants_pas)
{
    CHECK_INT(VERSION_HI, 0);
    CHECK_INT(VERSION_LO, 40);
    CHECK_INT(LOC_CARRIED, 254);
    CHECK_INT(LOC_WORN, 253);
    CHECK_INT(LOC_NOT_CREATED, 252);
    CHECK_INT(LOC_HERE, 255);
    CHECK_INT(NO_WORD, 255);
    CHECK_INT(MAX_FLAG_VALUE, 255);
    CHECK_INT(VOCABULARY_LENGTH, 5);
    CHECK_INT(MAX_DIRECTION_VOCABULARY, 13);
    CHECK_INT(MAX_CONVERTIBLE_NAME, 39);
    CHECK_INT(MAX_PROCESSES, 255);
    CHECK_INT(MAX_CONDACT_PARAMS, 3);
    CHECK_INT(MAX_V3_DIRECTION, 127);
    CHECK_INT(MAX_BLOCKABLE_CONNECTIONS, 128);
    CHECK_INT(MAX_OBJECTS, 256);
    CHECK_INT(MAX_MESSAGES_PER_TABLE, 255);
    CHECK_INT(MAX_WEIGHT, 63);
    CHECK_INT(MAX_PARAMETER_RANGE, 255);
    CHECK_INT(MAX_LABELS, 1024);
    CHECK_INT(NUM_CONDACTS, 128);
    CHECK_INT(NUM_FAKE_CONDACTS, 16);
    CHECK_INT(NUM_PREFIX_CONDACTS, 10);
    CHECK_INT(MESSAGE_OPCODE, 38);
    CHECK_INT(MES_OPCODE, 77);
    CHECK_INT(SYSMESS_OPCODE, 54);
    CHECK_INT(XMES_OPCODE, 128);
    CHECK_INT(XMESSAGE_OPCODE, 129);
    CHECK_INT(XPICTURE_OPCODE, 130);
    CHECK_INT(PICTURE_OPCODE, 84);
    CHECK_INT(XSAVE_OPCODE, 131);
    CHECK_INT(SAVE_OPCODE, 25);
    CHECK_INT(XLOAD_OPCODE, 132);
    CHECK_INT(LOAD_OPCODE, 26);
    CHECK_INT(XPLAY_OPCODE, 134);
    CHECK_INT(XBEEP_OPCODE, 135);
    CHECK_INT(XSPLITSCR_OPCODE, 136);
    CHECK_INT(XUNDONE_OPCODE, 137);
    CHECK_INT(XNEXTCLS_OPCODE, 138);
    CHECK_INT(XNEXTRST_OPCODE, 139);
    CHECK_INT(XSPEED_OPCODE, 140);
    CHECK_INT(XDATA_OPCODE, 142);
    CHECK_INT(BEEP_OPCODE, 64);
    CHECK_INT(DESC_OPCODE, 19);
    CHECK_INT(SKIP_OPCODE, 116);
    CHECK_INT(PENDINGSKIP_OPCODE, 141);
    CHECK_INT(SYNONYM_OPCODE, 36);
    CHECK_INT(PREP_OPCODE, 68);
    CHECK_INT(NOUN2_OPCODE, 69);
    CHECK_INT(ADJECT1_OPCODE, 16);
    CHECK_INT(ADVERB_OPCODE, 17);
    CHECK_INT(ADJECT2_OPCODE, 70);
    CHECK_INT(MES2_OPCODE, 521);
    CHECK_INT(FAKE_DEBUG_CONDACT_CODE, 220);
    CHECK_STR(FAKE_DEBUG_CONDACT_TEXT, "DEBUG");
    CHECK_INT(FAKE_USERPTR_CONDACT_CODE, 256);
    CHECK_INT(TOGGLECON_OPCODE, 520);
}

/* ---- condacts.c ---- */

TEST(condact_table_len_is_144)
{
    CHECK_INT((int)condact_table_len(), 144);
    CHECK(condact_table() != NULL);
}

TEST(condact_lookup_is_case_insensitive_ascii)
{
    const CondactDef *d1 = condact_lookup("at", 0);
    const CondactDef *d2 = condact_lookup("At", 0);
    const CondactDef *d3 = condact_lookup("AT", 0);
    CHECK(d1 != NULL);
    CHECK_INT(d1->opcode, 0);
    CHECK_STR(d1->name, "AT");
    CHECK(d2 == d1);
    CHECK(d3 == d1);
}

TEST(condact_lookup_jump_aliases_to_skip)
{
    const CondactDef *d = condact_lookup("jump", 0);
    CHECK(d != NULL);
    CHECK_INT(d->opcode, 116);
    CHECK_STR(d->name, "SKIP");
}

TEST(condact_lookup_debug_returns_220_regardless_of_v3)
{
    const CondactDef *d0 = condact_lookup("DEBUG", 0);
    const CondactDef *d1 = condact_lookup("debug", 1);
    CHECK(d0 != NULL);
    CHECK_INT(d0->opcode, FAKE_DEBUG_CONDACT_CODE);
    CHECK_INT(d0->opcode, 220);
    CHECK_INT(d0->num_params, 0);
    CHECK(d1 != NULL);
    CHECK_INT(d1->opcode, 220);
}

TEST(condact_lookup_unknown_name_returns_null)
{
    CHECK(condact_lookup("NOSUCHCONDACT", 0) == NULL);
    CHECK(condact_lookup("NOSUCHCONDACT", 1) == NULL);
}

/* THE v3 gate (brief step 2 / analysis 17.1): INDIR/SETAT are only
   reachable with -v3; without it the base "dumb" placeholder names
   are the only thing at opcodes 122/124. */
TEST(condact_lookup_v3_only_condact_rejected_without_v3)
{
    CHECK(condact_lookup("INDIR", 0) == NULL);
    CHECK(condact_lookup("SETAT", 0) == NULL);

    {
        const CondactDef *indir = condact_lookup("INDIR", 1);
        const CondactDef *setat = condact_lookup("SETAT", 1);
        CHECK(indir != NULL);
        CHECK_INT(indir->opcode, 122);
        CHECK_INT(indir->num_params, 1);
        CHECK_INT((int)indir->type1, (int)PARAM_VALUE);
        CHECK(setat != NULL);
        CHECK_INT(setat->opcode, 124);
        CHECK_INT(setat->num_params, 2);
        CHECK_INT((int)setat->type1, (int)PARAM_VALUE);
        CHECK_INT((int)setat->type2, (int)PARAM_VALUE);
    }
}

/* The "dumb" placeholder quirk (17.3): a DSF spelling "DUMB" resolves
   to opcode 120 without -v3; with -v3, opcodes 122/124 no longer
   answer to "dumb" at all (the override replaces them during the
   scan), but opcode 120 - never touched by ApplyV3Changes - still
   does either way. */
TEST(condact_lookup_dumb_placeholder_quirk)
{
    const CondactDef *d;

    d = condact_lookup("DUMB", 0);
    CHECK(d != NULL);
    CHECK_INT(d->opcode, 120);

    d = condact_lookup("dumb", 1);
    CHECK(d != NULL);
    CHECK_INT(d->opcode, 120);

    CHECK(condact_lookup("dumb", 0) != NULL); /* opcode 120, 122, or 124 - any is fine here */
}

/* Param-count spot check against the script-diffed table (brief step
   2): a zero-, one-, two- and three-name-field row, plus the two fake
   opcodes with special-cased lookups. */
TEST(condact_by_opcode_param_counts_spot_check)
{
    const CondactDef *d;

    d = condact_by_opcode(20, 0); /* QUIT */
    CHECK(d != NULL);
    CHECK_STR(d->name, "QUIT");
    CHECK_INT(d->num_params, 0);

    d = condact_by_opcode(0, 0); /* AT */
    CHECK(d != NULL);
    CHECK_INT(d->num_params, 1);

    d = condact_by_opcode(13, 0); /* EQ */
    CHECK(d != NULL);
    CHECK_STR(d->name, "EQ");
    CHECK_INT(d->num_params, 2);
    CHECK_INT((int)d->type1, (int)PARAM_FLAGNO);
    CHECK_INT((int)d->type2, (int)PARAM_VALUE);
    CHECK_INT(d->can_be_jump, 1);

    d = condact_by_opcode(143, 0); /* GETKEY, last real row */
    CHECK(d != NULL);
    CHECK_STR(d->name, "GETKEY");
    CHECK_INT(d->num_params, 0);

    d = condact_by_opcode(FAKE_DEBUG_CONDACT_CODE, 0);
    CHECK(d != NULL);
    CHECK_INT(d->num_params, 0);
    CHECK_STR(d->name, "DEBUG");
}

TEST(condact_by_opcode_out_of_range_returns_null)
{
    CHECK(condact_by_opcode(-1, 0) == NULL);
    CHECK(condact_by_opcode(144, 0) == NULL);
    CHECK(condact_by_opcode(256, 0) == NULL); /* FAKE_USERPTR_CONDACT_CODE never reaches this table */
}

TEST(condact_semantic_check_locno_out_of_range)
{
    Arena *a = arena_new(0);
    MessageList *ml = messagelist_new(a);
    VocTree *vt = voctree_new(a);
    const char *msg;

    ml->ltx_count = 3;
    msg = condact_semantic_check(a, 0 /* AT, locno */, 0, 1, 3, "3", vt, ml);
    CHECK(msg != NULL);
    CHECK_STR(msg, "Location 3 does not exist");

    msg = condact_semantic_check(a, 0, 0, 1, 2, "2", vt, ml);
    CHECK(msg == NULL);
    arena_free(a);
}

/* The locno_ QUIRK (19.39): two spaces after "Location", and the
   >=252 pseudo-location exemption. */
TEST(condact_semantic_check_locno__has_two_spaces_and_pseudo_loc_exempt)
{
    Arena *a = arena_new(0);
    MessageList *ml = messagelist_new(a);
    VocTree *vt = voctree_new(a);
    const char *msg;

    ml->ltx_count = 3;
    /* PLACE (opcode 46) Type2 is locno_. */
    msg = condact_semantic_check(a, 46, 0, 2, 5, "5", vt, ml);
    CHECK(msg != NULL);
    CHECK_STR(msg, "Location  5 does not exist");

    msg = condact_semantic_check(a, 46, 0, 2, 254, "254", vt, ml);
    CHECK(msg == NULL); /* LOC_CARRIED, exempt */
    arena_free(a);
}

TEST(condact_semantic_check_percent_and_window)
{
    Arena *a = arena_new(0);
    MessageList *ml = messagelist_new(a);
    VocTree *vt = voctree_new(a);
    const char *msg;

    /* CHANCE (opcode 10), Type1 percent. */
    msg = condact_semantic_check(a, 10, 0, 1, 0, "0", vt, ml);
    CHECK(msg != NULL);
    CHECK_STR(msg, "Invalid percent value, must be in the 1-99 range");
    msg = condact_semantic_check(a, 10, 0, 1, 100, "100", vt, ml);
    CHECK(msg != NULL);
    msg = condact_semantic_check(a, 10, 0, 1, 50, "50", vt, ml);
    CHECK(msg == NULL);

    /* WINDOW (opcode 78), Type1 window. */
    msg = condact_semantic_check(a, 78, 0, 1, 8, "8", vt, ml);
    CHECK(msg != NULL);
    CHECK_STR(msg, "Invalid window number, must be in the 0-7 range");
    msg = condact_semantic_check(a, 78, 0, 1, 7, "7", vt, ml);
    CHECK(msg == NULL);
    arena_free(a);
}

TEST(condact_semantic_check_flagno_value_procno_skip_always_pass)
{
    Arena *a = arena_new(0);
    MessageList *ml = messagelist_new(a);
    VocTree *vt = voctree_new(a);

    ml->ltx_count = 0; /* nothing declared - would fail if these were checked */
    /* ZERO (11, flagno), PROCESS (75, procno), SKIP (116, skip). */
    CHECK(condact_semantic_check(a, 11, 0, 1, 255, "255", vt, ml) == NULL);
    CHECK(condact_semantic_check(a, 75, 0, 1, 255, "255", vt, ml) == NULL);
    CHECK(condact_semantic_check(a, 116, 0, 1, 255, "255", vt, ml) == NULL);
    arena_free(a);
}

TEST(condact_semantic_check_vocabulary_noun_lookup)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    MessageList *ml = messagelist_new(a);
    VocTree *vt = voctree_new(a);
    const char *msg;

    CHECK_INT(voctree_add(vt, a, d, sl, "SWORD", 1, VOC_NOUN), 1);

    /* NOUN2 (69), Type1 vocabularyNoun. */
    msg = condact_semantic_check(a, 69, 0, 1, 0, "SWORD", vt, ml);
    CHECK(msg == NULL);

    msg = condact_semantic_check(a, 69, 0, 1, 0, "AXE", vt, ml);
    CHECK(msg != NULL);
    CHECK_STR(msg,
        "Word not defined in vocabulary or it has an unexpected word type "
        ": AXE");

    /* `_` is exempt. */
    msg = condact_semantic_check(a, 69, 0, 1, 0, "_", vt, ml);
    CHECK(msg == NULL);
    arena_free(a);
}

/* SYNONYM's vocabularyVerb parameter: a VERB miss retries as a NOUN,
   semantically only (UCondacts.pas:263). */
TEST(condact_semantic_check_vocabulary_verb_retries_as_noun)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    MessageList *ml = messagelist_new(a);
    VocTree *vt = voctree_new(a);
    const char *msg;

    CHECK_INT(voctree_add(vt, a, d, sl, "LAMP", 2, VOC_NOUN), 1);

    /* SYNONYM (36), Type1 vocabularyVerb - "LAMP" is not a verb, but
       IS a noun, so the retry passes. */
    msg = condact_semantic_check(a, 36, 0, 1, 0, "LAMP", vt, ml);
    CHECK(msg == NULL);

    msg = condact_semantic_check(a, 36, 0, 1, 0, "ZZZZZ", vt, ml);
    CHECK(msg != NULL);
    arena_free(a);
}

/* ---- voctree.c ---- */

TEST(voctree_add_then_lookup_roundtrips)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    VocTree *vt = voctree_new(a);
    VocEntry out;

    CHECK_INT(voctree_add(vt, a, d, sl, "sword", 5, VOC_NOUN), 1);
    CHECK_INT(voctree_lookup(vt, a, "SWORD", VOC_NOUN, &out), 1);
    CHECK_STR(out.voc_word, "SWORD");
    CHECK_INT(out.value, 5);
    CHECK_INT((int)out.voc_type, (int)VOC_NOUN);
    arena_free(a);
}

TEST(voctree_lookup_miss_returns_zero)
{
    Arena *a = arena_new(0);
    VocTree *vt = voctree_new(a);
    VocEntry out;
    CHECK_INT(voctree_lookup(vt, a, "NOPE", VOC_ANY, &out), 0);
    arena_free(a);
}

TEST(voctree_lookup_type_mismatch_fails_but_voc_any_matches)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    VocTree *vt = voctree_new(a);
    VocEntry out;

    CHECK_INT(voctree_add(vt, a, d, sl, "RUN", 1, VOC_VERB), 1);
    CHECK_INT(voctree_lookup(vt, a, "RUN", VOC_NOUN, &out), 0);
    CHECK_INT(voctree_lookup(vt, a, "RUN", VOC_VERB, &out), 1);
    CHECK_INT(voctree_lookup(vt, a, "RUN", VOC_ANY, &out), 1);
    arena_free(a);
}

/* PORT: 26.1 - the tree holds one type per distinct (transformed) text;
   a second insert of the SAME text under a DIFFERENT type is rejected
   exactly like a true duplicate. */
TEST(voctree_add_duplicate_text_rejected_regardless_of_type)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    VocTree *vt = voctree_new(a);

    CHECK_INT(voctree_add(vt, a, d, sl, "RUN", 1, VOC_VERB), 1);
    CHECK_INT(voctree_add(vt, a, d, sl, "run", 2, VOC_NOUN), 0);
    arena_free(a);
}

/* THE FixSpanishChars collision (brief step 2): "cafe"+e-acute (all
   lower, e9) and "CAFE"+E-acute (all upper, c9) both canonicalise to
   the identical byte string ("CAF" + 0xE9, ASCII upper / accent
   lower) and therefore collide as the SAME vocabulary word. */
TEST(voctree_add_collides_via_fixspanishchars_canonicalisation)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    VocTree *vt = voctree_new(a);
    char lower[5]; /* "caf" + e-acute (0xE9) */
    char upper[5]; /* "CAF" + E-acute (0xC9) */
    VocEntry out;

    lower[0]='c'; lower[1]='a'; lower[2]='f'; lower[3]=(char)0xE9; lower[4]='\0';
    upper[0]='C'; upper[1]='A'; upper[2]='F'; upper[3]=(char)0xC9; upper[4]='\0';

    CHECK_INT(voctree_add(vt, a, d, sl, lower, 9, VOC_NOUN), 1);
    /* Same canonical key, so this is a duplicate: rejected. */
    CHECK_INT(voctree_add(vt, a, d, sl, upper, 999, VOC_ADJECT), 0);

    /* And a lookup under either spelling finds the FIRST value, at the
       canonical key (ASCII upper, that one accent lower). */
    CHECK_INT(voctree_lookup(vt, a, lower, VOC_ANY, &out), 1);
    CHECK_INT(out.value, 9);
    CHECK_INT(voctree_lookup(vt, a, upper, VOC_ANY, &out), 1);
    CHECK_INT(out.value, 9);
    {
        char canon[5];
        canon[0]='C'; canon[1]='A'; canon[2]='F'; canon[3]=(char)0xE9; canon[4]='\0';
        CHECK_STR(out.voc_word, canon);
    }
    arena_free(a);
}

/* THE _VOC_<word> symbol side effect (brief step 2). */
TEST(voctree_add_defines_voc_symbol)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    VocTree *vt = voctree_new(a);
    long v = -1;

    CHECK_INT(voctree_add(vt, a, d, sl, "torch", 42, VOC_NOUN), 1);
    CHECK_INT(symbols_lookup(sl, a, "_VOC_TORCH", &v), 1);
    CHECK_INT(v, 42);
    arena_free(a);
}

/* Defect 19.52: a NEW word whose auto-generated _VOC_ symbol collides
   with a symbol defined some other way also makes voctree_add fail,
   even though the word text itself was never in the tree before. */
TEST(voctree_add_fails_when_voc_symbol_already_defined_elsewhere)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    VocTree *vt = voctree_new(a);

    CHECK_INT(symbols_add(sl, a, d, "_VOC_LAMP", 0), 1);
    CHECK_INT(voctree_add(vt, a, d, sl, "lamp", 7, VOC_NOUN), 0);
    arena_free(a);
}

/* PORT: GetVocabularyByNumber (97-109) checks the CURRENT node's own
   Value first, before ever recursing - so a root match short-circuits
   immediately, regardless of what its children hold. Only when the
   root does NOT match does the left-then-right preference apply. */
TEST(voctree_lookup_by_number_root_match_short_circuits)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    VocTree *vt = voctree_new(a);
    VocEntry out;

    /* "M" is the root (inserted first); value 1 matches the ROOT
       itself, so it wins even though "A" (left) also has value 1. */
    CHECK_INT(voctree_add(vt, a, d, sl, "M", 1, VOC_NOUN), 1);
    CHECK_INT(voctree_add(vt, a, d, sl, "A", 1, VOC_VERB), 1);
    CHECK_INT(voctree_add(vt, a, d, sl, "Z", 1, VOC_ADJECT), 1);

    CHECK_INT(voctree_lookup_by_number(vt, 1, VOC_ANY, &out), 1);
    CHECK_STR(out.voc_word, "M");
    arena_free(a);
}

TEST(voctree_lookup_by_number_prefers_left_subtree_when_root_misses)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    VocTree *vt = voctree_new(a);
    VocEntry out;

    /* Root "M" has a DIFFERENT value (99), so it cannot match; both
       "A" (left) and "Z" (right) hold the searched-for value 1 - the
       left subtree's match wins. */
    CHECK_INT(voctree_add(vt, a, d, sl, "M", 99, VOC_NOUN), 1);
    CHECK_INT(voctree_add(vt, a, d, sl, "A", 1, VOC_VERB), 1);
    CHECK_INT(voctree_add(vt, a, d, sl, "Z", 1, VOC_ADJECT), 1);

    CHECK_INT(voctree_lookup_by_number(vt, 1, VOC_ANY, &out), 1);
    CHECK_STR(out.voc_word, "A");
    arena_free(a);
}

/* PORT: 26.1's closing note - a word containing a folded-lower accent
   byte (0xE0-0xFF) sorts, byte for byte, AFTER a same-prefixed plain
   ASCII word, because the accented byte (>= 0xE0) always compares
   greater than any ASCII upper-case letter (0x41-0x5A) at the first
   position the two words differ. "CAFE" (plain) and "CAF"+e-acute
   share the "CAF" prefix and diverge at the 4th byte ('E'=0x45 vs
   0xE9), so this is the case the note is actually about - a word
   starting with a wholly different ASCII letter is ordered by that
   first letter instead, matching plain lexicographic BST order. */
TEST(voctree_inorder_ascending_by_canonical_key)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = symbols_new(a);
    VocTree *vt = voctree_new(a);
    Vec_VocEntry *walk;
    char cafe_accent[5];

    cafe_accent[0]='C'; cafe_accent[1]='A'; cafe_accent[2]='F';
    cafe_accent[3]=(char)0xE9; cafe_accent[4]='\0'; /* "CAF" + e-acute */

    CHECK_INT(voctree_add(vt, a, d, sl, cafe_accent, 1, VOC_NOUN), 1);
    CHECK_INT(voctree_add(vt, a, d, sl, "CAFE", 2, VOC_NOUN), 1);
    CHECK_INT(voctree_add(vt, a, d, sl, "APPLE", 3, VOC_NOUN), 1);

    CHECK_INT((int)voctree_count(vt), 3);
    walk = voctree_inorder(vt, a);
    CHECK_INT((int)vec_len_VocEntry(walk), 3);
    CHECK_STR(vec_at_VocEntry(walk, 0)->voc_word, "APPLE");
    CHECK_STR(vec_at_VocEntry(walk, 1)->voc_word, "CAFE");
    CHECK_STR(vec_at_VocEntry(walk, 2)->voc_word, cafe_accent);
    arena_free(a);
}

/* ---- messagelist.c ---- */

TEST(msglist_insert_or_dedup_first_insert_is_id_zero)
{
    Arena *a = arena_new(0);
    MsgList *t = msglist_new(a, 0);
    CHECK_INT(msglist_insert_or_dedup(t, a, "hello"), 0);
    CHECK_INT((int)msglist_count(t), 1);
    arena_free(a);
}

TEST(msglist_insert_or_dedup_dedups_exact_text)
{
    Arena *a = arena_new(0);
    MsgList *t = msglist_new(a, 0);
    CHECK_INT(msglist_insert_or_dedup(t, a, "hello"), 0);
    CHECK_INT(msglist_insert_or_dedup(t, a, "world"), 1);
    CHECK_INT(msglist_insert_or_dedup(t, a, "hello"), 0); /* existing id, no new entry */
    CHECK_INT((int)msglist_count(t), 2);
    arena_free(a);
}

/* The cap check is keyed on the LAST entry's id, not on how many
   entries actually exist (matches the Pascal's LastMessageID scan
   exactly) - a list seeded with a single entry at id 254 is already
   "full". */
TEST(msglist_seeded_with_id_254_alone_is_full)
{
    Arena *a = arena_new(0);
    MsgList *t = msglist_new(a, 0);
    msglist_add(t, a, 254, "sole entry");
    CHECK_INT((int)msglist_count(t), 1);
    CHECK_INT(msglist_insert_or_dedup(t, a, "new text"), -1);
    arena_free(a);
}

TEST(msglist_unlimited_bypasses_the_cap)
{
    Arena *a = arena_new(0);
    MsgList *t = msglist_new(a, 1); /* XTX-shaped: unlimited */
    msglist_add(t, a, 254, "sole entry");
    CHECK_INT(msglist_insert_or_dedup(t, a, "new text"), 255);
    arena_free(a);
}

TEST(messagelist_new_starts_all_counts_at_zero)
{
    Arena *a = arena_new(0);
    MessageList *ml = messagelist_new(a);
    CHECK_INT((int)ml->mtx_count, 0);
    CHECK_INT((int)ml->stx_count, 0);
    CHECK_INT((int)ml->ltx_count, 0);
    CHECK_INT((int)ml->otx_count, 0);
    CHECK_INT((int)ml->xtx_count, 0);
    CHECK_INT((int)ml->other_tx_count, 0);
    arena_free(a);
}

TEST(messagelist_cascade_home_table_success_leaves_opcode_unchanged)
{
    Arena *a = arena_new(0);
    MessageList *ml = messagelist_new(a);
    int opcode = MESSAGE_OPCODE;
    long id = messagelist_insert_cascade(ml, a, &opcode, "hi", 0);
    CHECK_INT((int)id, 0);
    CHECK_INT(opcode, MESSAGE_OPCODE);
    CHECK_INT((int)msglist_count(ml->mtx), 1);
    arena_free(a);
}

TEST(messagelist_cascade_classic_mode_hard_failure)
{
    Arena *a = arena_new(0);
    MessageList *ml = messagelist_new(a);
    int opcode = SYSMESS_OPCODE;
    long id;

    msglist_add(ml->stx, a, 254, "sole entry"); /* STX is SYSMESS's home table */
    id = messagelist_insert_cascade(ml, a, &opcode, "new text", 1 /* classic */);
    CHECK_INT((int)id, -1);
    CHECK_INT(opcode, SYSMESS_OPCODE); /* unchanged on hard failure */
    arena_free(a);
}

/* Defect 19.51: MESSAGE's home (MTX) full, non-classic -> retries STX
   with a literal two-byte "\n" (backslash, n) appended, and the
   opcode becomes SYSMESS. */
TEST(messagelist_cascade_message_overflow_appends_literal_backslash_n_and_becomes_sysmess)
{
    Arena *a = arena_new(0);
    MessageList *ml = messagelist_new(a);
    int opcode = MESSAGE_OPCODE;
    long id;
    const MsgEntry *e;

    msglist_add(ml->mtx, a, 254, "sole entry"); /* MTX is MESSAGE's home table */
    id = messagelist_insert_cascade(ml, a, &opcode, "hi", 0);
    CHECK_INT((int)id, 0);
    CHECK_INT(opcode, SYSMESS_OPCODE);
    CHECK_INT((int)msglist_count(ml->stx), 1);
    e = msglist_at(ml->stx, 0);
    CHECK(e != NULL);
    CHECK_STR(e->text, "hi\\n"); /* "hi" + backslash + 'n' - NOT "hi" + a newline byte */
    CHECK_INT((int)strlen(e->text), 4);
    arena_free(a);
}

/* The asymmetric swap (19.51): a SYSMESS retry becomes MES, not
   MESSAGE, and gets NO \n appended (only MESSAGE_OPCODE triggers
   that). */
TEST(messagelist_cascade_sysmess_overflow_becomes_mes_with_no_newline)
{
    Arena *a = arena_new(0);
    MessageList *ml = messagelist_new(a);
    int opcode = SYSMESS_OPCODE;
    long id;
    const MsgEntry *e;

    msglist_add(ml->stx, a, 254, "sole entry"); /* STX is SYSMESS's home table */
    id = messagelist_insert_cascade(ml, a, &opcode, "hi", 0);
    CHECK_INT((int)id, 0);
    CHECK_INT(opcode, MES_OPCODE);
    CHECK_INT((int)msglist_count(ml->mtx), 1);
    e = msglist_at(ml->mtx, 0);
    CHECK(e != NULL);
    CHECK_STR(e->text, "hi"); /* no \n appended - opcode was not MESSAGE_OPCODE */
    arena_free(a);
}

/* Both MTX and STX full: falls through to LTX unconditionally, opcode
   becomes DESC, with no further room check on the LTX attempt itself. */
TEST(messagelist_cascade_both_full_falls_to_ltx_becomes_desc)
{
    Arena *a = arena_new(0);
    MessageList *ml = messagelist_new(a);
    int opcode = MESSAGE_OPCODE;
    long id;

    msglist_add(ml->mtx, a, 254, "mtx sole entry");
    msglist_add(ml->stx, a, 254, "stx sole entry");
    id = messagelist_insert_cascade(ml, a, &opcode, "hi", 0);
    CHECK_INT((int)id, 0); /* LTX was empty, so it succeeds at id 0 */
    CHECK_INT(opcode, DESC_OPCODE);
    CHECK_INT((int)msglist_count(ml->ltx), 1);
    arena_free(a);
}

/* ---- objects.c ---- */

TEST(objectlist_add_then_find)
{
    Arena *a = arena_new(0);
    ObjectList *list = objectlist_new(a);
    objectlist_add(list, a, 0, 1, 2, 10, LOC_HERE, 0, 1, 0);
    CHECK_INT((int)objectlist_count(list), 1);
    CHECK_INT(objectlist_find(list, 1, 2), 1);
    arena_free(a);
}

/* The limit/error path this container actually has (26.3): FindObject
   is a pure query, never consulted by AddObject - a duplicate
   noun/adjective pair is accepted without complaint. */
TEST(objectlist_find_returns_false_for_unknown_pair_and_add_never_rejects)
{
    Arena *a = arena_new(0);
    ObjectList *list = objectlist_new(a);
    objectlist_add(list, a, 0, 5, 6, 0, LOC_HERE, 0, 0, 0);
    CHECK_INT(objectlist_find(list, 99, 99), 0);

    /* A second object with the SAME noun/adjective is accepted too -
       AddObject never consults FindObject. */
    objectlist_add(list, a, 1, 5, 6, 0, LOC_HERE, 0, 0, 0);
    CHECK_INT((int)objectlist_count(list), 2);
    CHECK_INT(objectlist_find(list, 5, 6), 1);
    arena_free(a);
}

TEST(objectlist_carried_and_worn_counters)
{
    Arena *a = arena_new(0);
    ObjectList *list = objectlist_new(a);
    objectlist_add(list, a, 0, 1, 1, 0, LOC_CARRIED, 0, 0, 0);
    objectlist_add(list, a, 1, 2, 2, 0, LOC_WORN, 0, 0, 1);
    objectlist_add(list, a, 2, 3, 3, 0, 0 /* a real location */, 0, 0, 0);
    CHECK_INT((int)list->carried_count, 1);
    CHECK_INT((int)list->worn_count, 1);
    arena_free(a);
}

/* ---- connections.c ---- */

TEST(connectionlist_add_then_find)
{
    Arena *a = arena_new(0);
    ConnectionList *list = connectionlist_new(a);
    connectionlist_add(list, a, 0, 1, 5);
    CHECK_INT((int)connectionlist_count(list), 1);
    CHECK_INT(connectionlist_find(list, 0, 1, 5), 1);
    CHECK_INT(connectionlist_find(list, 0, 1, 6), 0);
    arena_free(a);
}

/* The limit/error path this container actually has (19.35): only an
   EXACT (from, to, direction) triple is rejected as a duplicate by
   FindConnection - two DIFFERENT targets from the same location, same
   direction, are both accepted with no error at this layer. */
TEST(connectionlist_two_different_targets_same_direction_both_accepted)
{
    Arena *a = arena_new(0);
    ConnectionList *list = connectionlist_new(a);
    connectionlist_add(list, a, 0, 1, 5); /* loc 0, direction 5, -> loc 1 */
    connectionlist_add(list, a, 0, 2, 5); /* loc 0, direction 5, -> loc 2 (different target) */
    CHECK_INT((int)connectionlist_count(list), 2);
    CHECK_INT(connectionlist_find(list, 0, 1, 5), 1);
    CHECK_INT(connectionlist_find(list, 0, 2, 5), 1);
    arena_free(a);
}

/* ---- process.c ---- */

TEST(processtable_new_prefills_all_slots)
{
    Arena *a = arena_new(0);
    ProcessTable *t = processtable_new(a);
    const ProcessSlot *s0 = processtable_get(t, 0);
    const ProcessSlot *slast = processtable_get(t, MAX_PROCESSES);
    CHECK_INT((int)processtable_len(t), MAX_PROCESSES + 1);
    CHECK(s0 != NULL);
    CHECK_INT((int)s0->value, 0);
    CHECK_INT((int)vec_len_ProcessEntry(s0->entries), 0);
    CHECK(slast != NULL);
    CHECK_INT((int)slast->value, MAX_PROCESSES);
    arena_free(a);
}

TEST(processtable_get_out_of_range_returns_null)
{
    Arena *a = arena_new(0);
    ProcessTable *t = processtable_new(a);
    CHECK(processtable_get(t, -1) == NULL);
    CHECK(processtable_get(t, MAX_PROCESSES + 1) == NULL);
    arena_free(a);
}

/* THE GUARD (ruled, defect 19.36): a process number beyond the fixed
   256-slot table FATALs with a clear diagnostic instead of reproducing
   the reference's uncaught RTE 201. */
TEST(processtable_at_fatals_when_out_of_range)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    ProcessTable *t = processtable_new(a);
    FILE *f = scratch_open();
    ProcessSlot *slot;

    diag_set_stream(d, f);
    slot = processtable_at(t, a, d, MAX_PROCESSES + 1);
    CHECK(slot == NULL);
    CHECK_INT(diag_error_count(d), 1);
    CHECK_INT(diag_exit_code(d), 2);

    /* An in-range call still works normally. */
    slot = processtable_at(t, a, d, 3);
    CHECK(slot != NULL);
    CHECK_INT((int)slot->value, 3);
    fclose(f);
    arena_free(a);
}

TEST(process_add_entry_with_condacts)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    ProcessTable *t = processtable_new(a);
    ProcessSlot *slot = processtable_at(t, a, d, 7);
    Vec_ProcessCondact *condacts = vec_new_ProcessCondact(a);
    CondactParam params[MAX_CONDACT_PARAMS];
    const ProcessEntry *e;
    const ProcessCondact *c;
    int i;

    for (i = 0; i < MAX_CONDACT_PARAMS; i++) {
        params[i].value = i;
        params[i].indirection = (i == 1);
    }
    process_condacts_add(condacts, a, 0 /* AT */, 1, params, 0);
    process_add_entry(slot, a, 10, 20, NULL, condacts);

    CHECK_INT((int)vec_len_ProcessEntry(slot->entries), 1);
    e = vec_at_ProcessEntry(slot->entries, 0);
    CHECK_INT((int)e->verb, 10);
    CHECK_INT((int)e->noun, 20);
    CHECK(e->skip_label == NULL);
    CHECK_INT((int)vec_len_ProcessCondact(e->condacts), 1);
    c = vec_at_ProcessCondact(e->condacts, 0);
    CHECK_INT((int)c->opcode, 0);
    CHECK_INT(c->is_db, 0);
    CHECK_INT(c->num_params, 1);
    CHECK_INT((int)c->params[1].value, 1);
    CHECK_INT(c->params[1].indirection, 1);
    arena_free(a);
}

int main(void)
{
    RUN(upper_latin1_folds_ascii_letters);
    RUN(upper_latin1_folds_all_eight_accent_pairs);
    RUN(upper_latin1_folds_the_full_measured_range);
    RUN(upper_latin1_ff_folds_to_cp1252_9f_not_df);
    RUN(upper_latin1_e0_folds_to_c0);
    RUN(upper_latin1_f7_division_sign_has_no_case);
    RUN(upper_latin1_bytes_outside_the_block_are_identity);
    RUN(upper_latin1_leaves_already_uppercase_accents_unchanged);

    RUN(symbols_add_then_lookup_roundtrips);
    RUN(symbols_lookup_miss_returns_zero);
    RUN(symbols_add_folds_ascii_case_and_lookup_is_case_insensitive);
    RUN(symbols_fold_collides_lowercase_and_uppercase_accented_symbol);
    RUN(symbols_add_duplicate_keeps_original_value);
    RUN(symbols_add_verbose_prints_folded_name);
    RUN(symbols_add_verbose_silent_unless_enabled);
    RUN(symbols_add_verbose_suppressed_for_voc_prefix);
    RUN(symbols_iteration_is_insertion_order);

    RUN(labels_add_new_then_find);
    RUN(labels_are_case_sensitive);
    RUN(labels_key_includes_dollar_sign);
    RUN(labels_forward_reference_then_resolves);
    RUN(labels_duplicate_real_definition_rejected);
    RUN(labels_iteration_matches_insertion_order);
    RUN(labels_add_fatals_when_table_is_full);

    RUN(constants_match_uconstants_pas);

    RUN(condact_table_len_is_144);
    RUN(condact_lookup_is_case_insensitive_ascii);
    RUN(condact_lookup_jump_aliases_to_skip);
    RUN(condact_lookup_debug_returns_220_regardless_of_v3);
    RUN(condact_lookup_unknown_name_returns_null);
    RUN(condact_lookup_v3_only_condact_rejected_without_v3);
    RUN(condact_lookup_dumb_placeholder_quirk);
    RUN(condact_by_opcode_param_counts_spot_check);
    RUN(condact_by_opcode_out_of_range_returns_null);
    RUN(condact_semantic_check_locno_out_of_range);
    RUN(condact_semantic_check_locno__has_two_spaces_and_pseudo_loc_exempt);
    RUN(condact_semantic_check_percent_and_window);
    RUN(condact_semantic_check_flagno_value_procno_skip_always_pass);
    RUN(condact_semantic_check_vocabulary_noun_lookup);
    RUN(condact_semantic_check_vocabulary_verb_retries_as_noun);

    RUN(voctree_add_then_lookup_roundtrips);
    RUN(voctree_lookup_miss_returns_zero);
    RUN(voctree_lookup_type_mismatch_fails_but_voc_any_matches);
    RUN(voctree_add_duplicate_text_rejected_regardless_of_type);
    RUN(voctree_add_collides_via_fixspanishchars_canonicalisation);
    RUN(voctree_add_defines_voc_symbol);
    RUN(voctree_add_fails_when_voc_symbol_already_defined_elsewhere);
    RUN(voctree_lookup_by_number_root_match_short_circuits);
    RUN(voctree_lookup_by_number_prefers_left_subtree_when_root_misses);
    RUN(voctree_inorder_ascending_by_canonical_key);

    RUN(msglist_insert_or_dedup_first_insert_is_id_zero);
    RUN(msglist_insert_or_dedup_dedups_exact_text);
    RUN(msglist_seeded_with_id_254_alone_is_full);
    RUN(msglist_unlimited_bypasses_the_cap);
    RUN(messagelist_new_starts_all_counts_at_zero);
    RUN(messagelist_cascade_home_table_success_leaves_opcode_unchanged);
    RUN(messagelist_cascade_classic_mode_hard_failure);
    RUN(messagelist_cascade_message_overflow_appends_literal_backslash_n_and_becomes_sysmess);
    RUN(messagelist_cascade_sysmess_overflow_becomes_mes_with_no_newline);
    RUN(messagelist_cascade_both_full_falls_to_ltx_becomes_desc);

    RUN(objectlist_add_then_find);
    RUN(objectlist_find_returns_false_for_unknown_pair_and_add_never_rejects);
    RUN(objectlist_carried_and_worn_counters);

    RUN(connectionlist_add_then_find);
    RUN(connectionlist_two_different_targets_same_direction_both_accepted);

    RUN(processtable_new_prefills_all_slots);
    RUN(processtable_get_out_of_range_returns_null);
    RUN(processtable_at_fatals_when_out_of_range);
    RUN(process_add_entry_with_condacts);

    return test_summary("front_units");
}
