/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/emit_mml.c - mmlToBeep, XPLAY's note-by-note MML parser.
   Copyright (C) 2026 Dan Gibson.

   PORT: drb.php:1593-1702 mmlToBeep, whole function. Reproduces three
   defects: R arm (drb.php:1647) sets a typo field, not
   DurationAdjusted, so the PAUSE is re-adjusted on revisit (plus the
   255 cap, drb.php:874); the C Condact has no such field, so "not set"
   is the port's only shape, observably identical. N arm
   (drb.php:1666-1671) likewise sets no DurationAdjusted, and its
   Param2 = 48 + idx*2 (drb.php:1670) carries no pitch adjustment
   unlike the A-G arm's Param2 (drb.php:1625). V arm on MSX2
   (drb.php:1685-1691) leaves Indirection1/Indirection2/Param3/Param4/
   Condact PHP-undefined; arena_calloc zero-inits them, the
   observable-equivalent outcome.

   All three duration sites round through php_round, not C's round()
   (back/phpround.h); six of targets.c's seven base lengths reach a
   diverging product through plain MML here, against one for PAUSE -
   MML durations are game-authored, so a real game can hit this. */
#include "emit.h"

#include "arena.h"
#include "back/phpround.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#define PAUSE_OPCODE 35
#define BEEP_OPCODE  64
#define SFX_OPCODE   18

/* PORT: drb.php:1596-1598 $noteIdx, transcribed verbatim including the
   B+ = 12 (one full octave's worth of half-steps, not wrapped to 0) and
   C- = -1 entries the brief calls out by name. E# and B# have no entry
   in the PHP array either (PHP's undefined-key read then coerces null
   to 0 in the idx*2 arithmetic) - note_index below returns 0 for the
   same two absent keys, the observable-equivalent outcome; unreached by
   every fixture in this phase. */
typedef struct { const char *key; long idx; } NoteIdxEntry;

static const NoteIdxEntry NOTE_IDX[] = {
    {"C", 0}, {"C#", 1}, {"D", 2}, {"D#", 3}, {"E", 4}, {"F", 5}, {"F#", 6},
    {"G", 7}, {"G#", 8}, {"A", 9}, {"A#", 10}, {"B", 11},
    {"C+", 1}, {"D+", 3}, {"E+", 5}, {"F+", 6}, {"G+", 8}, {"A+", 10}, {"B+", 12},
    {"C-", -1}, {"D-", 1}, {"E-", 3}, {"F-", 4}, {"G-", 6}, {"A-", 8}, {"B-", 10},
};
#define NOTE_IDX_COUNT (sizeof(NOTE_IDX) / sizeof(NOTE_IDX[0]))

static long note_index(const char *note, size_t end)
{
    size_t i;
    for (i = 0; i < NOTE_IDX_COUNT; i++) {
        if (strlen(NOTE_IDX[i].key) == end && strncmp(NOTE_IDX[i].key, note, end) == 0) {
            return NOTE_IDX[i].idx;
        }
    }
    return 0;   /* E#/B#: absent from the PHP table too - see file header */
}

/* PORT: PHP intval(substr($note, $start)) - parses an optional sign
   then digits, skipping leading whitespace via isspace() (ASCII set) -
   distinct from emit_proc.c's is_php_trim_char, which is trim()'s own
   charset (also strips NUL, no isspace() equivalence claim). */
static long php_intval_at(const char *note, size_t note_len, size_t start)
{
    size_t i = start;
    long sign = 1;
    long val = 0;

    while (i < note_len && isspace((unsigned char)note[i])) i++;
    if (i < note_len && (note[i] == '+' || note[i] == '-')) {
        if (note[i] == '-') sign = -1;
        i++;
    }
    while (i < note_len && note[i] >= '0' && note[i] <= '9') {
        val = val * 10 + (note[i] - '0');
        i++;
    }
    return sign * val;
}

int mml_to_beep(Arena *a, Diag *d, const char *note, long values[4],
                const Target *t, Condact **out)
{
    size_t note_len = strlen(note);
    char cmd = note[0];
    long base_length = (long)t->duration_base_length;

    *out = NULL;

    /* PORT: drb.php:1604-1627 - Note: [A-G][#:halftone][num:length][.:period] */
    if (cmd >= 'A' && cmd <= 'G') {
        double period = 1.0;
        size_t len = note_len;
        size_t end;
        long idx;
        double length;
        Condact *c;

        while (len > 0 && note[len - 1] == '.') {
            period *= 1.5;
            len--;
        }
        length = (double)values[XPLAY_LENGTH] / period;

        end = 1;
        if (len > 1 && (note[1] == '#' || note[1] == '-' || note[1] == '+')) end = 2;
        idx = note_index(note, end);

        if (end < len) {
            length = (double)php_intval_at(note, len, end) / period;
        }

        c = arena_calloc(a, sizeof(*c));
        c->Opcode = BEEP_OPCODE;
        c->DurationAdjusted = 1;   /* drb.php:1621 - this arm sets it correctly */
        c->NumParams = 2;
        if (length == 0.0) {
            diag_fatal(d, "Wrong length at note %.*s", (int)len, note);
            *out = NULL;
            return 0;
        }
        c->Param1 = (long)php_round((double)base_length * (120.0 / (double)values[XPLAY_TEMPO]) / length);
        c->Param2 = values[XPLAY_OCTAVE] * 24 + idx * 2 + t->pitch_adjustment;
        c->Indirection1 = 0;
        c->Condact = "BEEP";
        *out = c;
        return 1;
    }

    /* PORT: drb.php:1630-1632 - Note length [1-64] state, default 4. */
    if (cmd == 'L') {
        values[XPLAY_LENGTH] = php_intval_at(note, note_len, 1);
        return 0;
    }

    /* PORT: drb.php:1634-1654 - Pause [1-64]. DEFECT: drb.php:1647 sets
       the typo field, not DurationAdjusted - see file header. */
    if (cmd == 'R') {
        double period = 1.0;
        size_t len = note_len;
        double length;
        Condact *c;

        while (len > 0 && note[len - 1] == '.') {
            period *= 1.5;
            len--;
        }
        length = (double)values[XPLAY_LENGTH] / period;
        if (len > 1) {
            length = (double)php_intval_at(note, len, 1) / period;
        }

        c = arena_calloc(a, sizeof(*c));
        c->Opcode = PAUSE_OPCODE;
        /* DurationAdjusted deliberately left 0 - see file header. */
        c->NumParams = 1;
        /* drb.php:1649 divides by length with no zero check; PHP
           intval(round(INF)) is 0 (pinned live); casting INF to long is
           UB in C, so guard to the same 0. The Param1 == 0 clamp below
           runs either way (drb.php:1651). */
        if (length == 0.0) {
            c->Param1 = 0;
        } else {
            c->Param1 = (long)php_round((double)base_length * (120.0 / (double)values[XPLAY_TEMPO]) / length);
        }
        if (c->Param1 == 0) c->Param1 = 1;   /* avoid PAUSE 0 -> GETKEY (v3) / very long pause (v2) */
        c->Indirection1 = 0;
        c->Condact = "PAUSE";
        *out = c;
        return 1;
    }

    /* PORT: drb.php:1656-1673 - Note Pitch [0-96]. DEFECT: no
       DurationAdjusted, and Param2 carries no pitch adjustment - see
       file header. */
    if (cmd == 'N') {
        double period = 1.0;
        size_t len = note_len;
        double length;
        long idx;
        Condact *c;

        while (len > 0 && note[len - 1] == '.') {
            period *= 1.5;
            len--;
        }
        length = (double)values[XPLAY_LENGTH] / period;
        idx = php_intval_at(note, len, 1);

        c = arena_calloc(a, sizeof(*c));
        c->Opcode = BEEP_OPCODE;
        /* DurationAdjusted deliberately left 0 - see file header. */
        c->NumParams = 2;
        /* PORT NOTE (memory-safety, not a defect port): drb.php:1669
           divides by $length with no zero check, same shape as the R
           arm above - see that arm's PORT NOTE for the live-pinned PHP
           behaviour (intval(round(X/0)) is always int(0) on this
           toolchain, regardless of sign or NaN). No clamp follows this
           one (drb.php has none for the N arm either - see file
           header), so Param1 is simply left 0. */
        if (length == 0.0) {
            c->Param1 = 0;
        } else {
            c->Param1 = (long)php_round((double)base_length * (120.0 / (double)values[XPLAY_TEMPO]) / length);
        }
        c->Param2 = 48 + idx * 2;   /* drb.php:1670 - no pitch_adjustment */
        c->Indirection1 = 0;
        c->Condact = "BEEP";
        *out = c;
        return 1;
    }

    /* PORT: drb.php:1675-1677 - Octave [1-8] state, default 4. */
    if (cmd == 'O') {
        values[XPLAY_OCTAVE] = php_intval_at(note, note_len, 1);
        return 0;
    }

    /* PORT: drb.php:1679-1681 - Tempo [32-255] state, default 120. */
    if (cmd == 'T') {
        values[XPLAY_TEMPO] = php_intval_at(note, note_len, 1) & 255;
        return 0;
    }

    /* PORT: drb.php:1683-1692 - Volume [0-15] state; MSX2 additionally
       emits an SFX condact - DEFECT, see file header. */
    if (cmd == 'V') {
        values[XPLAY_VOLUME] = php_intval_at(note, note_len, 1) & 15;
        if (strcmp(t->name, "MSX2") == 0) {
            Condact *c = arena_calloc(a, sizeof(*c));
            c->Opcode = SFX_OPCODE;
            c->NumParams = 2;
            c->Param1 = values[XPLAY_VOLUME];
            c->Param2 = 8;
            /* Indirection1/Indirection2/Param3/Param4/Condact/
               DurationAdjusted left zero/NULL by arena_calloc - see
               file header. */
            *out = c;
            return 1;
        }
        return 0;
    }

    /* PORT: drb.php:1694-1696 - Decreases one octave, clamped at 1. */
    if (cmd == '<') {
        if (values[XPLAY_OCTAVE] > 1) values[XPLAY_OCTAVE]--;
        return 0;
    }

    /* PORT: drb.php:1698-1700 - Increases one octave, clamped at 8. */
    if (cmd == '>') {
        if (values[XPLAY_OCTAVE] < 8) values[XPLAY_OCTAVE]++;
        return 0;
    }

    /* PORT: drb.php:1701 - S/M/& and anything else: no arm matches,
       $condact stays NULL. */
    return 0;
}
