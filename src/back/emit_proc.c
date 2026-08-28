/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/emit_proc.c - process emitter: pass-zero condact rewrites,
   tail-sharing dedup, bytecode emission, entry and process tables.
   Copyright (C) 2026 Dan Gibson.

   PORT: drb.php:819-1225 generateProcesses, plus its three helpers read
   first per protocol: getCondactsHash (drb.php:769-802), checkMaluva
   (drb.php:804-812) and MaluvaEmbedded (drb.php:814-817).

   Both v3 branches are live; t varies across all 35 table rows;
   XSPLITSCR is still strcmp'd pending a table column; classic_mode is
   0 iff the target is NEXTDAAD, else adv->classic_mode as resolved by
   main.c from the JSON settings OR'd with -c and written back before
   this function runs (drb.php:1797-1799; drb.php:1799 writes back into
   $adventure->classicMode). Emission verified against the cited
   drb.php ranges at each site below. */
#include "emit.h"

#include "arena.h"
#include "back/phpround.h"
#include "map.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Condact opcodes (drb.php:18-46). Only the ones this function reads
   or rewrites are ported; the rest live with each opcode's own table
   emitter. */
#define FAKE_DEBUG_CONDACT_CODE   220
#define FAKE_USERPTR_CONDACT_CODE 256
#define XMES_OPCODE       128
#define XPICTURE_OPCODE    130
#define XSAVE_OPCODE        131
#define XLOAD_OPCODE         132
#define XPART_OPCODE          133
#define XPLAY_OPCODE           134
#define XBEEP_OPCODE            135
#define XSPLITSCR_OPCODE         136
#define XUNDONE_OPCODE            137
#define XNEXTCLS_OPCODE            138
#define XNEXTRST_OPCODE             139
#define XSPEED_OPCODE                140
#define XDATA_OPCODE                  142
#define GETKEY_OPCODE                  143
#define LET_OPCODE     51
#define PAUSE_OPCODE   35
#define EXTERN_OPCODE  61
#define BEEP_OPCODE    64
#define AT_OPCODE       0
#define PROCESS_OPCODE 75
#define INDIR_OPCODE  122
#define XMES_FINAL_OPCODE 120
#define GFX_OPCODE     87

/* PORT: getDurationAdjustment (drb.php:1573-1576) is
   getBaseLength(target, subtarget) / DEFAULT_NOTE_DURATION, with
   DEFAULT_NOTE_DURATION=200 (drb.php:1530); t->duration_base_length is
   the table's transcription of getBaseLength (targets.c).

   drb.php:873/894's round() is PHP's, which pre-rounds - see
   phpround.h. Both call sites use php_round. Divergence from C's
   round() was measured on 28 cells (CONDACTS on MSX, the 12 MSX2
   subtargets and HTML); `PAUSE 50` on a base length of 230 is the
   minimal repro. */
#define DURATION_ADJUSTMENT(t) ((double)(t)->duration_base_length / 200.0)

/* PORT: drb.php:1051 - DONE/OK/NOTDONE/SKIP/RESTART/REDO. */
static const long TERMINATOR_OPCODES[] = { 22, 23, 103, 116, 117, 108 };
#define TERMINATOR_COUNT (sizeof(TERMINATOR_OPCODES) / sizeof(TERMINATOR_OPCODES[0]))

static int is_terminator(long opcode)
{
    size_t i;
    for (i = 0; i < TERMINATOR_COUNT; i++) {
        if (TERMINATOR_OPCODES[i] == opcode) return 1;
    }
    return 0;
}

typedef struct {
    long offset;   /* -1 = not yet assigned; drb.php's $hashInfo->offset */
} HashInfo;

/* PORT: drb.php:769-802 getCondactsHash. Reproduces defect S12.7
   deliberately: drb.php:778 computes an indirection-masked opcode into
   a local variable, but drb.php:779 appends the RAW, un-masked
   condact->Opcode to the hash string instead of using it - so two
   condacts differing only in Indirection1 hash identically. Indirection2
   never participates in the hash at all (no line ever appends it). Both
   are ported as-is: the masked `opcode` local below is computed and
   then discarded, exactly as the PHP wastes it. */
static const char *condacts_hash(Arena *arena, const Adventure *adv, Vec_Condact *condacts, size_t from)
{
    Str *h = str_new(arena);
    size_t i, n = vec_len_Condact(condacts);

    for (i = from; i < n; i++) {
        Condact *c = vec_at_Condact(condacts, i);
        long opcode = c->Opcode;

        if (opcode == FAKE_DEBUG_CONDACT_CODE && !adv->debug_mode) continue;
        if (opcode == FAKE_USERPTR_CONDACT_CODE) continue;

        if (c->NumParams > 0 && c->Indirection1) opcode |= 0x80;
        (void)opcode;   /* computed, never read - defect S12.7 */

        str_appendf(h, "%ld ", c->Opcode);
        if (c->NumParams > 0) {
            str_appendf(h, "%ld ", c->Param1);
            if (c->NumParams > 1) {
                str_appendf(h, "%ld ", c->Param2);
                if (c->NumParams > 2) {
                    str_appendf(h, "%ld ", c->Param3);
                    if (c->NumParams > 3) {
                        str_appendf(h, "%ld ", c->Param4);
                    }
                }
            }
        }
    }
    return str_cstr(h);
}

/* PORT: drb.php:804-812 checkMaluva. adv->externs (model.h) IS loaded
   and could run the same per-extern MLV_ scan drb.php:807-809 does,
   but doesn't need to: hard-coded 0 by design, since the only guard
   consuming it is ANDed with !maluva_embedded() (drb.php:814-817),
   which is always false (S12.2) - dead on both sides of the port. */
static int check_maluva(const Adventure *adv)
{
    (void)adv;
    return 0;
}

/* PORT: drb.php:814-817 MaluvaEmbedded - defect S12.2, always returns
   true regardless of its arguments, which makes every guard shaped
   like `(!check_maluva(adv)) && !maluva_embedded()` permanently false.
   Ported dead, per ruling; adv->maluva_used is deliberately NOT
   consulted here (the PHP function ignores it too). */
static int maluva_embedded(void)
{
    return 1;
}

/* PHP's default trim() charset " \t\n\r\0\x0B", ported as a predicate
   rather than a string literal because an embedded NUL cannot survive
   inside a C string literal. */
static int is_php_trim_char(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0' || c == 0x0B;
}

static char *php_trim(Arena *a, const char *s)
{
    size_t len = strlen(s);
    size_t start = 0, end = len;

    while (start < end && is_php_trim_char((unsigned char)s[start])) start++;
    while (end > start && is_php_trim_char((unsigned char)s[end - 1])) end--;
    return arena_strndup(a, s + start, end - start);
}

/* PORT: drb.php:962 explode(',', $dataString) with the drb.php:966-970
   trim() loop folded into the same pass (PORT NOTE: the PHP trims in a
   separate loop after exploding; doing it inline here is
   observably identical since nothing reads the untrimmed pieces). */
static Vec_CStr *split_xdata(Arena *a, const char *s)
{
    Vec_CStr *v = vec_new_CStr(a);
    const char *start = s;
    const char *p = s;

    for (;;) {
        if (*p == ',' || *p == '\0') {
            char *piece = arena_strndup(a, start, (size_t)(p - start));
            vec_push_CStr(v, php_trim(a, piece));
            if (*p == '\0') break;
            start = p + 1;
        }
        p++;
    }
    return v;
}

/* PORT: drb.php:975 filter_var($element, FILTER_VALIDATE_INT, array()),
   applied to an element already trimmed and uppercased (drb.php:968),
   so filter_var's own internal whitespace tolerance is never
   exercised here and is not reproduced. Grammar pinned live on PHP
   7.4.30 (32-bit, PHP_INT_MAX = 2147483647): optional single leading
   '+' or '-', then one or more digits (empty string rejected); a
   leading '0' is rejected unless the whole digit run is exactly "0",
   so "0"/"+0"/"-0" parse but "00"/"007"/"-01" etc. fail; the parsed
   value must fit -2147483648..2147483647 or the call fails. Anything
   else returns false and the caller keeps the raw string. */
static int parse_xdata_int(const char *s, long *out)
{
    const char *p = s;
    const char *digits_start;
    size_t ndigits;
    long v;

    if (*p == '+' || *p == '-') p++;
    digits_start = p;
    while (*p >= '0' && *p <= '9') p++;
    ndigits = (size_t)(p - digits_start);
    if (ndigits == 0 || *p != '\0') return 0;
    if (digits_start[0] == '0' && ndigits > 1) return 0;

    errno = 0;
    v = strtol(s, NULL, 10);
    if (errno == ERANGE || v < -2147483647L - 1 || v > 2147483647L) return 0;

    *out = v;
    return 1;
}

/* PORT: drb.php:1515-1526 dataToLet. */
static Condact *data_to_let(Arena *a, long flagno, long value)
{
    Condact *c = arena_calloc(a, sizeof(*c));
    c->NumParams = 2;
    c->Indirection1 = 0;
    c->Param1 = flagno;
    c->Param2 = value;
    c->Condact = "LET";
    c->Opcode = LET_OPCODE;
    return c;
}

/* PORT: drb.php:958-997 XDATA_OPCODE handling, in full. Returns 0 when
   a fatal has already been reported (mirroring PHP's Error()==exit(2):
   the caller must stop immediately, matching the whole compiler halting
   at its first error); 1 on success, having replaced entry->condacts
   with a rebuilt Vec (PORT NOTE: Vec has no in-place insert/splice, so
   the replacement is a fresh copy-with-substitution rather than
   drb.php:992's array_splice - the resulting condact sequence is
   identical). The caller steps its own condactID back by one on
   success, exactly as drb.php:993 does. */
static int rewrite_xdata(Diag *d, Arena *a, const Adventure *adv, ProcEntry *entry, long condactID)
{
    Condact *condact = vec_at_Condact(entry->condacts, (size_t)condactID);
    const char *text = "";
    char *upper;
    Vec_CStr *elements;
    size_t i, n;
    Vec_Condact *lets;
    Vec_Condact *rebuilt;
    long base_flagno;

    /* PORT NOTE: PHP indexes $adventure->otherStrings[$condact->Param1]
       unguarded; an out-of-range Param1 hits PHP's undefined-index
       error and the request fatals there. C has no such trap, so this
       bounds guard substitutes text="" for an out-of-range Param1
       instead of trapping - a different diagnostic (PHP's undefined-
       index error vs. this function's own "not data enough" fatal
       below, reached once split_xdata("") yields under 2 elements),
       but the same outcome: rejection. Only reachable with malformed
       input (an XDATA condact whose Param1 does not name a string). */
    if (condact->Param1 >= 0 && (size_t)condact->Param1 < vec_len_Message(adv->other_strings)) {
        const Message *msg = vec_at_Message(adv->other_strings, (size_t)condact->Param1);
        text = str_cstr(msg->Text);
    }
    upper = str_upper_ascii(a, text);
    elements = split_xdata(a, upper);
    n = vec_len_CStr(elements);

    if (n < 2) {
        diag_fatal(d, "There is not data enough in XDATA condact");
        return 0;
    }
    for (i = 0; i < n; i++) {
        const char *e = vec_at_CStr(elements, i);
        if (e[0] == '\0') {
            diag_fatal(d, "Empty element in XDATA condact at position %zu", i);
            return 0;
        }
    }
    for (i = 0; i < n; i++) {
        const char *e = vec_at_CStr(elements, i);
        long v;
        if (!parse_xdata_int(e, &v)) {
            diag_fatal(d, "Non integer value in XDATA condact element #%zu '%s'", i, e);
            return 0;
        }
        if (v < 0 || v > 255) {
            diag_fatal(d, "XDATA values must be in the 0-255 range, element #%zu is not (%s)", i, e);
            return 0;
        }
    }

    parse_xdata_int(vec_at_CStr(elements, 0), &base_flagno);
    lets = vec_new_Condact(a);
    for (i = 1; i < n; i++) {
        long value;
        if (base_flagno > 255) {
            diag_fatal(d, "XDATA condact went over flag 255");
            return 0;
        }
        parse_xdata_int(vec_at_CStr(elements, i), &value);
        vec_push_Condact(lets, data_to_let(a, base_flagno, value));
        base_flagno++;
    }

    rebuilt = vec_new_Condact(a);
    for (i = 0; i < (size_t)condactID; i++) vec_push_Condact(rebuilt, vec_at_Condact(entry->condacts, i));
    for (i = 0; i < vec_len_Condact(lets); i++) vec_push_Condact(rebuilt, vec_at_Condact(lets, i));
    for (i = (size_t)condactID + 1; i < vec_len_Condact(entry->condacts); i++) vec_push_Condact(rebuilt, vec_at_Condact(entry->condacts, i));
    entry->condacts = rebuilt;

    return 1;
}

/* PORT NOTE (memory-safety, not a defect port - same shape as the
   FAKE_USERPTR_CONDACT_CODE guard above): drb.php:845/857 index
   $GLOBALS['xMessageOffsets'][$condact->Param1] with no bounds check.
   PHP autovivifies/undefined-indexes an out-of-range key harmlessly to
   null - arithmetic on it (the offset split below) then reads as 0,
   with a notice, and DRB carries on. adv->xmessage_offsets is a plain
   C array sized to vec_len_Message(adv->xmessages), and stays NULL when
   adv->xmessages is empty (model.h) since emit_xmessages is then never
   called; a malformed JSON's XMES condact naming a Param1 outside that
   range - or any XMES condact at all when xmessages is empty - must
   not read out of bounds. Guard added as a memory-safety port fix, not
   a defect fix; behaviour matches PHP's effective output (offset 0). */
static long xmessage_offset_at(const Adventure *adv, long messno)
{
    if (adv->xmessage_offsets == NULL) return 0;
    if (messno < 0 || messno >= (long)vec_len_Message(adv->xmessages)) return 0;
    return adv->xmessage_offsets[messno];
}

void emit_processes(Str *out, long *addr, Diag *d, const Target *t, Adventure *adv, int verbose)
{
    long procID, entryID;
    Map *hash_map;
    Map *cond_offsets;

    /* =================================================================
       PASS ZERO (drb.php:822-1048): check the processes and rewrite
       some condacts (XMESSAGE into a Maluva call, XDATA into LETs,
       etc), fixing bugs like the ZX BEEP parameter order along the
       way. */
    for (procID = 0; procID < (long)vec_len_Process(adv->processes); procID++) {
        Process *process = vec_at_Process(adv->processes, (size_t)procID);

        for (entryID = 0; entryID < (long)vec_len_ProcEntry(process->entries); entryID++) {
            ProcEntry *entry = vec_at_ProcEntry(process->entries, (size_t)entryID);
            long condactID;

            for (condactID = 0; condactID < (long)vec_len_Condact(entry->condacts); condactID++) {
                Condact *condact = vec_at_Condact(entry->condacts, (size_t)condactID);

                if (condact->Opcode == PROCESS_OPCODE) {
                    if (!condact->Indirection1 && condact->Param1 >= (long)vec_len_Process(adv->processes)) {
                        diag_fatal(d, "Invalid call to process #%ld. Specified process does not exist",
                                   condact->Param1);
                        return;
                    }
                } else if (condact->Opcode == XMES_OPCODE) {
                    /* PORT: drb.php:839-867. Converts XMESS into an
                       XMES_FINAL bytecode condact (v3 - drb.php:841-
                       852) or a Maluva CALL via EXTERN (v2 - drb.php:
                       853-865) against the offset table the xmessages
                       emitter built (emit_xmb.c's adv->xmessage_offsets,
                       drb.php:845/857's $GLOBALS['xMessageOffsets']). */
                    long messno = condact->Param1;

                    if (adv->v3code) {
                        long offset = xmessage_offset_at(adv, messno);
                        if (offset > 0xFFFF) {
                            diag_fatal(d, "Size of xMessages exceeds the 64K limit");
                            return;
                        }
                        condact->Opcode = XMES_FINAL_OPCODE;
                        condact->NumParams = 2;
                        condact->Param1 = offset & 0xFF;         /* Offset LSB */
                        condact->Param2 = (offset & 0xFF00) >> 8; /* Offset MSB */
                        condact->Condact = "XMES";
                    } else {
                        /* PORT: drb.php:853-865 - v2 rewrites XMES into
                           a Maluva CALL via EXTERN, with the Maluva
                           function number (3) in the MIDDLE parameter
                           and the offset split LSB/MSB across Param1
                           and Param3. */
                        long offset = xmessage_offset_at(adv, messno);
                        if (offset > 0xFFFF) {
                            diag_fatal(d, "Size of xMessages exceeds the 64K limit");
                            return;
                        }
                        condact->Opcode = EXTERN_OPCODE;
                        condact->NumParams = 3;
                        condact->Param1 = offset & 0xFF;          /* Offset LSB */
                        condact->Param3 = (offset & 0xFF00) >> 8; /* Offset MSB */
                        condact->Param2 = 3; /* Maluva function 3 */
                        condact->Condact = "EXTERN";
                    }
                    /* PORT: drb.php:866, defect S12.2 - maluva_embedded
                       always returns true, so this guard's Error can
                       never fire; ported as the dead branch it is. Its
                       text carries a "[target subtarget]" suffix that
                       the XUNDONE guard below (drb.php:886) does not. */
                    if (!check_maluva(adv) && !maluva_embedded()) {
                        const char *subtarget = t->subtarget ? t->subtarget : "";
                        diag_fatal(d, "XMES condact requires Maluva Extension [%s %s]",
                                   t->name, subtarget);
                        return;
                    }
                } else if (condact->Opcode == PAUSE_OPCODE) {
                    if (!condact->DurationAdjusted) {
                        condact->DurationAdjusted = 1;
                        condact->Param1 = (long)php_round((double)condact->Param1 * DURATION_ADJUSTMENT(t));
                        if (condact->Param1 > 255) condact->Param1 = 255;
                    }
                } else if (condact->Opcode == XUNDONE_OPCODE) {
                    /* PORT: drb.php:877-886. v3 fatals with a doubled
                       period - the message text already ends in one,
                       diag_fatal's "Error: %s.\n" shape adds another
                       (drb.php:879's bare Error() call) - already
                       ported in 1a. The v2 rewrite below (drb.php:880-
                       885) is dormant: no committed fixture exercises
                       XUNDONE under a v2 target, so it is untested by
                       the oracle gate; ported for byte-exact parity
                       with drb.php regardless. */
                    if (adv->v3code) {
                        diag_fatal(d, "XUNDONE condact has been deprecated.");
                        return;
                    }
                    condact->Opcode = EXTERN_OPCODE;
                    condact->NumParams = 2;
                    condact->Param1 = 0;
                    condact->Param2 = 7;
                    condact->Indirection1 = 0;
                    condact->Condact = "EXTERN";
                    if (!check_maluva(adv) && !maluva_embedded()) {
                        diag_fatal(d, "XUNDONE condact requires Maluva Extension");
                        return;
                    }
                } else if (condact->Opcode == BEEP_OPCODE) {
                    if (!condact->DurationAdjusted) {
                        condact->DurationAdjusted = 1;
                        condact->Param1 = (long)php_round((double)condact->Param1 * DURATION_ADJUSTMENT(t));
                        if (condact->Param1 > 255) condact->Param1 = 255;
                    }
                    if (condact->Param2 < 48 || condact->Param2 > 238) {
                        condact->Opcode = PAUSE_OPCODE;
                        condact->Condact = "PAUSE";
                        condact->NumParams = 1;
                    } else if (t->beep_swap) {
                        long tmp = condact->Param1;
                        condact->Param1 = condact->Param2;
                        condact->Param2 = tmp;
                    }
                } else if (condact->Opcode == XPLAY_OPCODE) {
                    /* PORT: drb.php:913-948. Default state (drb.php:
                       916), then drb.php:918's other_strings[Param1]
                       lookup (bounds-guarded exactly like
                       xmessage_offset_at above - PHP's undefined-index
                       read coerces to "" via strtoupper(null), the
                       observable-equivalent outcome), then the
                       tokenizer (drb.php:920-934): strpbrk(mml+1,
                       charset) IS drb.php's strpbrk(substr($mml,1),
                       charset) - both return the first match through
                       to the end of the searched string, so no
                       substring copy is needed to find note's extent,
                       only to hand mml_to_beep a NUL-terminated single
                       token. */
                    long values[4];
                    Vec_Condact *xplay = vec_new_Condact(str_arena(out));
                    char *mml_upper;
                    const char *mml;

                    values[XPLAY_OCTAVE] = 4;
                    values[XPLAY_VOLUME] = 8;
                    values[XPLAY_LENGTH] = 4;
                    values[XPLAY_TEMPO] = 120;

                    {
                        const char *text = "";
                        if (condact->Param1 >= 0 &&
                            (size_t)condact->Param1 < vec_len_Message(adv->other_strings)) {
                            const Message *msg = vec_at_Message(adv->other_strings, (size_t)condact->Param1);
                            text = str_cstr(msg->Text);
                        }
                        mml_upper = str_upper_ascii(str_arena(out), text);
                    }

                    mml = mml_upper;
                    while (mml != NULL && *mml != '\0') {
                        const char *next = strpbrk(mml + 1, "ABCDEFGABLNORTVSM<>&");
                        size_t note_len = next ? (size_t)(next - mml) : strlen(mml);
                        const char *note = arena_strndup(str_arena(out), mml, note_len);
                        Condact *beep = NULL;
                        int errors_before = diag_error_count(d);

                        mml_to_beep(str_arena(out), d, note, values, t, &beep);
                        if (diag_error_count(d) > errors_before) return;   /* drb.php:1623 Error() */
                        if (beep != NULL) vec_push_Condact(xplay, beep);
                        mml = next;
                    }

                    if (vec_len_Condact(xplay) > 0) {
                        /* PORT: drb.php:935-938 array_splice +
                           condactID-- rewind. No Vec splice: rebuild
                           the list with the chain substituted (as
                           rewrite_xdata does), then condactID-- so
                           pass zero revisits the chain's first condact
                           - the revisit is what applies the 48-238
                           clamp, beep_swap order and DurationAdjusted
                           re-adjustment (255 cap) to arms that did not
                           set DurationAdjusted (see emit_mml.c). */
                        Vec_Condact *rebuilt = vec_new_Condact(str_arena(out));
                        size_t i;
                        for (i = 0; i < (size_t)condactID; i++)
                            vec_push_Condact(rebuilt, vec_at_Condact(entry->condacts, i));
                        for (i = 0; i < vec_len_Condact(xplay); i++)
                            vec_push_Condact(rebuilt, vec_at_Condact(xplay, i));
                        for (i = (size_t)condactID + 1; i < vec_len_Condact(entry->condacts); i++)
                            vec_push_Condact(rebuilt, vec_at_Condact(entry->condacts, i));
                        entry->condacts = rebuilt;
                        condactID--;
                    } else {
                        /* PORT: drb.php:940-947 - empty (or entirely
                           state-only) chain rewrites XPLAY into an
                           always-true indirect `AT 38`. */
                        condact->Opcode = AT_OPCODE;
                        condact->Condact = "AT";
                        condact->NumParams = 1;
                        condact->Param1 = 38;
                        condact->Indirection1 = 1;
                    }
                } else if (condact->Opcode == GETKEY_OPCODE) {
                    condact->Opcode = PAUSE_OPCODE;
                    condact->NumParams = 1;
                    condact->Param1 = 0;
                    condact->Indirection1 = 0;
                    condact->Condact = "PAUSE";
                    if (!adv->v3code) {
                        diag_fatal(d, "GETKEY condact requires DAAD v3");
                        return;
                    }
                } else if (condact->Opcode == XDATA_OPCODE) {
                    if (!rewrite_xdata(d, str_arena(out), adv, entry, condactID)) return;
                    condactID--;
                } else if (condact->Opcode == XSPLITSCR_OPCODE) {
                    if (adv->v3code) {
                        condact->Opcode = GFX_OPCODE;
                        condact->NumParams = 2;
                        condact->Param2 = 15;
                        condact->Condact = "GFX";
                    } else {
                        condact->Opcode = EXTERN_OPCODE;
                        condact->NumParams = 2;
                        condact->Param2 = 6;
                        condact->Condact = "EXTERN";
                    }
                    /* Spaces inside the brackets are the PHP's own. */
                    if (strcmp(t->name, "CPC") != 0 && strcmp(t->name, "C64") != 0) {
                        diag_fatal(d, "XSPLITSCR is not supported by target [ %s ]", t->name);
                        return;
                    }
                } else if (condact->Opcode == XPICTURE_OPCODE) {
                    diag_fatal(d, "XPICTURE condact has been deprecated.");
                    return;
                } else if (condact->Opcode == XNEXTCLS_OPCODE) {
                    diag_fatal(d, "XNEXTCLS condact has been deprecated.");
                    return;
                } else if (condact->Opcode == XNEXTRST_OPCODE) {
                    diag_fatal(d, "XNEXTRST condact has been deprecated.");
                    return;
                } else if (condact->Opcode == XSPEED_OPCODE) {
                    diag_fatal(d, "XSPEED condact has been deprecated.");
                    return;
                } else if (condact->Opcode == XSAVE_OPCODE) {
                    diag_fatal(d, "XSAVE condact has been deprecated.");
                    return;
                } else if (condact->Opcode == XLOAD_OPCODE) {
                    diag_fatal(d, "XLOAD condact has been deprecated.");
                    return;
                } else if (condact->Opcode == XPART_OPCODE) {
                    diag_fatal(d, "XPART condact has been deprecated");
                    return;
                } else if (condact->Opcode == XBEEP_OPCODE) {
                    diag_fatal(d, "XBEEP condact has been deprecated");
                    return;
                }
            }
        }
    }

    /* =================================================================
       PASS ONE (drb.php:1053-1079): register every TAIL position's hash
       with offset -1, first registration winning. Nothing is emitted
       here. */
    hash_map = map_new(str_arena(out));
    cond_offsets = map_new(str_arena(out));

    if (!adv->classic_mode) {
        for (procID = 0; procID < (long)vec_len_Process(adv->processes); procID++) {
            Process *process = vec_at_Process(adv->processes, (size_t)procID);

            for (entryID = 0; entryID < (long)vec_len_ProcEntry(process->entries); entryID++) {
                ProcEntry *entry = vec_at_ProcEntry(process->entries, (size_t)entryID);
                size_t cn = vec_len_Condact(entry->condacts);
                size_t cid;

                for (cid = 0; cid < cn; cid++) {
                    const char *hash = condacts_hash(str_arena(out), adv, entry->condacts, cid);
                    if (hash[0] != '\0' && !map_has(hash_map, hash)) {
                        HashInfo *hi = arena_alloc(str_arena(out), sizeof(*hi));
                        hi->offset = -1;
                        map_put(hash_map, hash, hi);
                    }
                }
            }
        }
    }

    /* =================================================================
       DUMP: emit bytecode and record which address each entry's
       condacts start at (drb.php:1081-1197). */
    for (procID = 0; procID < (long)vec_len_Process(adv->processes); procID++) {
        Process *process = vec_at_Process(adv->processes, (size_t)procID);

        for (entryID = 0; entryID < (long)vec_len_ProcEntry(process->entries); entryID++) {
            ProcEntry *entry = vec_at_ProcEntry(process->entries, (size_t)entryID);
            char key[48];
            int skip = 0;
            int terminator_found = 0;
            long condactID;

            if (!adv->classic_mode) {
                const char *hash = condacts_hash(str_arena(out), adv, entry->condacts, 0);
                if (hash[0] != '\0') {
                    HashInfo *hi = map_get(hash_map, hash);
                    if (hi->offset != -1) {
                        long *off = arena_alloc(str_arena(out), sizeof(long));
                        *off = hi->offset;
                        snprintf(key, sizeof key, "%ld_%ld", procID, entryID);
                        map_put(cond_offsets, key, off);
                        skip = 1;
                    } else {
                        layout_pad(out, addr, t);
                        hi->offset = *addr;
                    }
                }
                /* hash=='': an empty hash opts out of sharing - no
                   padding call either, matching the PHP exactly (there
                   is no addPaddingIfRequired call on that path). */
            } else {
                layout_pad(out, addr, t);
            }

            if (skip) continue;

            {
                long *off = arena_alloc(str_arena(out), sizeof(long));
                *off = *addr;
                snprintf(key, sizeof key, "%ld_%ld", procID, entryID);
                map_put(cond_offsets, key, off);
            }

            for (condactID = 0; condactID < (long)vec_len_Condact(entry->condacts); condactID++) {
                Condact *condact = vec_at_Condact(entry->condacts, (size_t)condactID);
                long opcode = condact->Opcode;
                int has_second_param_indirection = (condact->NumParams > 1) && condact->Indirection2;
                long i;

                if (opcode == FAKE_DEBUG_CONDACT_CODE && !adv->debug_mode) continue;
                if (opcode == FAKE_USERPTR_CONDACT_CODE) {
                    /* PORT: drb.php:1117-1123; drb.php:1119
                       auto-vivifies out-of-range extvec keys. The
                       header patch reads only 0..12, so they are
                       observably ignored. A C long[13] write would
                       corrupt: guard to 0..12. In range it is
                       unguarded like the PHP - slots 0-2 clobberable,
                       defect S12.9 preserved. diag_note fires
                       unconditionally. */
                    long usrextvec = condact->Param1;
                    if (usrextvec >= 0 && usrextvec < 13) {
                        adv->extvec[usrextvec] = *addr;
                    }
                    diag_note(d, "UserPtr #%ld set to 0x%04lX", usrextvec, (unsigned long)*addr);
                    continue;
                }

                /* PORT: drb.php:1125-1130. Registers the TAIL hash at
                   this position so a LATER entry can share into the
                   MIDDLE of this one's bytestream. The padding half of
                   the condition (!t->padding_platform) is live on
                   ST/PC/AMIGA/HTML - spec 5.1's ST golden exercises it
                   - and only ever short-circuits away on the
                   non-padding targets. */
                if (!adv->classic_mode) {
                    if ((*addr % 2 == 0) || !t->padding_platform) {
                        const char *hash2 = condacts_hash(str_arena(out), adv, entry->condacts, (size_t)condactID);
                        if (hash2[0] != '\0') {
                            HashInfo *hi2 = map_get(hash_map, hash2);
                            if (hi2->offset == -1) hi2->offset = *addr;
                        }
                    }
                }

                if (condact->NumParams > 0 && condact->Indirection1) opcode |= 0x80;
                if (opcode == FAKE_DEBUG_CONDACT_CODE && verbose) diag_note(d, "Debug condact found, inserted.");

                /* PORT: drb.php:1115/1140-1145. The NumParams guard is
                   part of the condition, not a nicety - a one-param
                   condact with Indirection2 (impossible in valid
                   input, since Indirection2 only ever applies to a
                   second parameter) must NOT emit INDIR. */
                if (has_second_param_indirection) {
                    str_push_u8(out, (unsigned)INDIR_OPCODE);
                    str_push_u8(out, (unsigned)condact->Param2);
                    *addr += 2;
                }

                str_push_u8(out, (unsigned)opcode);
                (*addr)++;

                /* PORT NOTE: drb.php's matching switch has no default
                   arm - NumParams > 4 writes nothing yet drb.php:1145
                   still advances the address (desync). This port's
                   default arm instead pushes a zero byte per such
                   index, so *addr stays equal to bytes written - a
                   deliberate divergence. NumParams can never exceed
                   MAX_CONDACT_PARAMS (3) for input DRF can produce, so
                   this arm is unreachable from real input. */
                for (i = 0; i < condact->NumParams; i++) {
                    long param = 0;
                    switch (i) {
                        case 0: param = condact->Param1; break;
                        case 1: param = condact->Param2; break;
                        case 2: param = condact->Param3; break;
                        case 3: param = condact->Param4; break;
                        default: break;
                    }
                    str_push_u8(out, (unsigned)param);
                }
                *addr += condact->NumParams;

                if (!adv->classic_mode && is_terminator(opcode)) {
                    terminator_found = 1;
                    if (verbose) {
                        size_t cn = vec_len_Condact(entry->condacts);
                        if (condactID != (long)cn - 1) {
                            long human_entry_id = entryID + 1;
                            const Condact *next_condact = vec_at_Condact(entry->condacts, (size_t)condactID + 1);
                            const char *condact_name = next_condact->Condact;
                            const char *terminator_name = condact->Condact;
                            const char *entry_text = entry->Entry ? entry->Entry : "";
                            /* Always "hasn't been": this whole block only
                               runs when !classic_mode. Ported as a
                               ternary anyway, matching the classicMode
                               branches elsewhere in this port. */
                            const char *dumped = adv->classic_mode ? "has been" : "hasn't been";
                            diag_note(d,
                                "Warning: Condact '%s' found after a terminator '%s' in entry #%ld (%s) at process #%ld . Condact %s dumped to DDB file.",
                                condact_name, terminator_name, human_entry_id, entry_text, procID, dumped);
                        }
                    }
                    break;
                }
            }
            if (adv->classic_mode || !terminator_found) {
                str_push_u8(out, 0xFFu);   /* mark of end of entry */
                (*addr)++;
            }
        }
    }

    /* =================================================================
       Entry and process tables (drb.php:1199-1225). */
    layout_pad(out, addr, t);
    {
        Map *proc_offsets = map_new(str_arena(out));

        for (procID = 0; procID < (long)vec_len_Process(adv->processes); procID++) {
            Process *process = vec_at_Process(adv->processes, (size_t)procID);
            char pkey[24];
            long *poff = arena_alloc(str_arena(out), sizeof(long));

            *poff = *addr;
            snprintf(pkey, sizeof pkey, "%ld", procID);
            map_put(proc_offsets, pkey, poff);

            for (entryID = 0; entryID < (long)vec_len_ProcEntry(process->entries); entryID++) {
                ProcEntry *entry = vec_at_ProcEntry(process->entries, (size_t)entryID);
                char ckey[48];
                long *coff;

                snprintf(ckey, sizeof ckey, "%ld_%ld", procID, entryID);
                coff = map_get(cond_offsets, ckey);
                str_push_u8(out, (unsigned)entry->Verb);
                str_push_u8(out, (unsigned)entry->Noun);
                str_push_u16(out, (unsigned)*coff, t->big_endian);
                *addr += 4;
            }
            /* PORT NOTE: drb.php:1214's comment claims "doble 00"
               (Spanish: "double 00") for the end-of-process marker,
               but WriteZero writes exactly one zero byte here, same as
               every other single-byte writeXxx call - the comment is
               simply wrong. Ported as the single byte it actually
               is. */
            str_push_u8(out, 0x00u);
            (*addr)++;
            layout_pad(out, addr, t);
        }

        layout_pad(out, addr, t);
        for (procID = 0; procID < (long)vec_len_Process(adv->processes); procID++) {
            char pkey[24];
            long *poff;

            snprintf(pkey, sizeof pkey, "%ld", procID);
            poff = map_get(proc_offsets, pkey);
            str_push_u16(out, (unsigned)*poff, t->big_endian);
            *addr += 2;
        }
    }
}
