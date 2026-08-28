/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_php_round.c - Copyright (C) 2026 Dan Gibson.

   Pins php_round against PHP 7.4.30's own round(), measured with the very
   php.exe the oracle's reference flow runs. Every expected value below is
   transcribed from that run, not derived. The full validation covered
   46279 values (all reachable base-length x Param1 products, a +/-4 ULP
   sweep around every half-integer from -4.5 to 400.5, and 40000
   pseudo-random doubles in [-500, 500]) with zero mismatches; these are
   the rows worth keeping under permanent gate. */
#include "test.h"
#include "arena.h"
#include "back/emit.h"
#include "back/phpround.h"
#include "diag.h"
#include "model.h"
#include "str.h"
#include "targets.h"

#include <math.h>
#include <string.h>

/* getDurationAdjustment (drb.php:1573-1576) for one targets.c base
   length, computed the same way emit_proc.c's DURATION_ADJUSTMENT macro
   does so the products here are bit-identical to the ones the emitter
   rounds. */
static double adjusted(long param1, int base_length)
{
    return (double)param1 * ((double)base_length / 200.0);
}

/* `PAUSE 50` on a base length of 230 (MSX, MSX2, HTML) is 50 * 1.15,
   which is 57.49999999999999289457 as a double - one ULP below 57.5:
   PHP rounds it up to 58, C's round() down to 57. */
TEST(pause_50_on_base_230)
{
    double product = adjusted(50, 230);

    /* The product really is below the half-way point - if this ever
       stops holding, the rest of this test is measuring nothing. */
    CHECK(product < 57.5);
    CHECK_INT((long)round(product), 57);        /* what C alone does */
    CHECK_INT((long)php_round(product), 58);    /* what drb.php does */
}

/* The other five Param1 values that diverge on base length 230, out of
   all 256 a byte parameter can hold. Measured, not predicted. */
TEST(all_diverging_params_on_base_230)
{
    static const struct { long param1; long expect; } rows[] = {
        {  50,  58 }, {  90, 104 }, { 110, 127 },
        { 170, 196 }, { 190, 219 }, { 210, 242 }
    };
    size_t i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        double product = adjusted(rows[i].param1, 230);
        CHECK_INT((long)php_round(product), rows[i].expect);
        CHECK_INT((long)round(product), rows[i].expect - 1);
    }
}

/* Every OTHER base length in targets.c (80, 100, 120, 195, 200, 300)
   diverges on none of the 256 reachable Param1 values, so php_round must
   agree with C's round() across all of them. This is the half of the
   fix that must NOT move any byte - it is why no committed golden
   changes. */
TEST(other_base_lengths_never_diverge)
{
    static const int bases[] = { 80, 100, 120, 195, 200, 300 };
    size_t b;
    long p;

    for (b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
        for (p = 0; p <= 255; p++) {
            double product = adjusted(p, bases[b]);
            CHECK_INT((long)php_round(product), (long)round(product));
        }
    }
}

/* Half away from zero, not to even: PHP's default PHP_ROUND_HALF_UP. */
TEST(exact_halves_round_away_from_zero)
{
    CHECK_INT((long)php_round(0.5), 1);
    CHECK_INT((long)php_round(1.5), 2);
    CHECK_INT((long)php_round(2.5), 3);
    CHECK_INT((long)php_round(-0.5), -1);
    CHECK_INT((long)php_round(-1.5), -2);
    CHECK_INT((long)php_round(-2.5), -3);
    CHECK_INT((long)php_round(4.5), 5);
    CHECK_INT((long)php_round(382.5), 383);   /* PAUSE 255 on base 300 */
}

/* The pre-round is symmetric about zero, and it is TIGHT: it lifts a
   value one ULP below the half-way point, but not one that misses by
   more than the precision floating point still guarantees. Both bounds
   measured. */
TEST(preround_is_symmetric_and_bounded)
{
    CHECK_INT((long)php_round(57.49999999999999289457), 58);
    CHECK_INT((long)php_round(-57.49999999999999289457), -58);
    CHECK_INT((long)php_round(2.4999999999999996), 3);

    /* NOT the pre-round: below 1.0, precision_places is 15, which fails
       php_round's `< 15` guard, so this takes the else branch and comes
       out 1 purely from floor(x + 0.5) - the addition carries to 1.0
       exactly. Kept here because PHP returns 1 for it either way, and a
       port that reached the same answer down some third path would be
       wrong about this input. */
    CHECK_INT((long)php_round(0.49999999999999994), 1);

    /* Far enough below to stay down on both sides. */
    CHECK_INT((long)php_round(57.49999999999899813474), 57);
    CHECK_INT((long)php_round(57.49999999989999821537), 57);
    CHECK_INT((long)php_round(-57.49999999999899813474), -57);
}

/* Zero and the sign of zero pass through untouched (PHP returns the
   value itself before any rounding runs). */
TEST(zero_passes_through)
{
    CHECK(php_round(0.0) == 0.0);
    CHECK(php_round(-0.0) == 0.0);
    CHECK_INT((long)php_round(0.0), 0);
}

/* THE PAUSE SITE, DRIVEN THROUGH THE EMITTER (emit_proc.c's pass-zero
   duration adjustment, drb.php:873). pause_50_on_base_230 above calls
   php_round directly, which is not enough to guard the fix: reverting
   emit_proc.c's two call sites to C's round() leaves every gate green
   except the live matrix. So this drives emit_processes itself on a
   minimal one-process, one-entry `PAUSE 50` adventure on MSX
   (duration_base_length 230), so the mutation fails `make test`
   outright. */
TEST(emit_processes_pauses_through_php_round)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    Str *out = str_new(a);
    const Target *t = target_lookup("MSX", NULL);
    Adventure adv;
    Condact *c;
    ProcEntry *e;
    Process *p;
    long addr = 0;

    CHECK(t != NULL);
    if (t == NULL) return;
    CHECK_INT(t->duration_base_length, 230);

    memset(&adv, 0, sizeof(adv));
    adv.v3code = 1;
    adv.processes = vec_new_Process(a);
    adv.other_strings = vec_new_Message(a);
    adv.xmessages = vec_new_Message(a);

    c = arena_calloc(a, sizeof(*c));
    c->Opcode = 35;              /* PAUSE */
    c->NumParams = 1;
    c->Param1 = 50;              /* 50 * (230/200) == 57.49999999999999289 */
    c->Condact = "PAUSE";

    e = arena_calloc(a, sizeof(*e));
    e->Entry = "_";
    e->Verb = 255;
    e->Noun = 255;
    e->condacts = vec_new_Condact(a);
    vec_push_Condact(e->condacts, c);

    p = arena_calloc(a, sizeof(*p));
    p->entries = vec_new_ProcEntry(a);
    vec_push_ProcEntry(p->entries, e);
    vec_push_Process(adv.processes, p);

    emit_processes(out, &addr, d, t, &adv, 0);

    /* The pass-zero rewrite adjusted the condact in place... */
    CHECK_INT(c->Param1, 58);
    CHECK_INT(c->DurationAdjusted, 1);

    /* ...and the emitted bytecode carries that adjusted parameter.
       C's round() would put 57 here. */
    CHECK(str_len(out) >= 2);
    if (str_len(out) >= 2) {
        CHECK_INT(str_bytes(out)[0], 35);
        CHECK_INT(str_bytes(out)[1], 58);
    }

    arena_free(a);
}

/* THE MML ARMS (emit_mml.c's three duration sites, drb.php:1649). A
   dotted note divides by 1.5, so six of the seven base lengths in
   targets.c reach a diverging product through plain MML. The case
   pinned here is `T90 L8` followed by a dotted note on base length
   230: 230 * (120/90) / (8/1.5) is 57.49999999999999289457 - bit-for-
   bit the same double `PAUSE 50` produces. drb.php writes 58; C's
   round() would write 57. One dotted note per arm covers all three
   swapped call sites. */
static void mml_boundary_case(const char *note, long expect_param1)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    const Target *t = target_lookup("MSX", NULL);
    long values[4];
    Condact *c = NULL;
    int produced;

    CHECK(t != NULL);
    if (t == NULL) { arena_free(a); return; }
    CHECK_INT(t->duration_base_length, 230);

    values[XPLAY_OCTAVE] = 4;
    values[XPLAY_VOLUME] = 15;
    values[XPLAY_LENGTH] = 8;    /* L8 */
    values[XPLAY_TEMPO]  = 90;   /* T90 */

    produced = mml_to_beep(a, d, note, values, t, &c);
    CHECK_INT(produced, 1);
    CHECK(c != NULL);
    if (c != NULL) CHECK_INT(c->Param1, expect_param1);
    arena_free(a);
}

TEST(mml_duration_boundary_on_base_230)
{
    /* The product every arm below computes, spelled out once: it really
       does land under the half-way point, and C's round() really would
       take it down. */
    double product = 230.0 * (120.0 / 90.0) / (8.0 / 1.5);
    CHECK(product < 57.5);
    CHECK_INT((long)round(product), 57);
    CHECK_INT((long)php_round(product), 58);

    mml_boundary_case("C.", 58);    /* A-G arm  (emit_mml.c:152) */
    mml_boundary_case("R.", 58);    /* R arm    (emit_mml.c:209) */
    mml_boundary_case("N5.", 58);   /* N arm    (emit_mml.c:249) */
}

/* The same three arms on an UNDOTTED note, where the product is a whole
   number and nothing about the swap can move a byte - the neutrality
   half of the change, matching other_base_lengths_never_diverge above.
   230 * (120/90) / 8 = 38.333..., which both roundings take to 38. */
TEST(mml_undotted_note_is_unchanged)
{
    double product = 230.0 * (120.0 / 90.0) / 8.0;
    CHECK_INT((long)php_round(product), (long)round(product));

    mml_boundary_case("C", 38);
    mml_boundary_case("R", 38);
    mml_boundary_case("N5", 38);
}

int main(void)
{
    RUN(pause_50_on_base_230);
    RUN(all_diverging_params_on_base_230);
    RUN(other_base_lengths_never_diverge);
    RUN(exact_halves_round_away_from_zero);
    RUN(preround_is_symmetric_and_bounded);
    RUN(zero_passes_through);
    RUN(emit_processes_pauses_through_php_round);
    RUN(mml_duration_boundary_on_base_230);
    RUN(mml_undotted_note_is_unchanged);
    return test_summary("php_round");
}
