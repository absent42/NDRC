/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/emit.h - DDB table emitters.
   Copyright (C) 2026 Dan Gibson.

   Ported from drb.php's per-table generate* functions: generateMessages
   (drb.php:526-568, shared by generateMTX/STX/LTX/OTX), the object
   tables (drb.php:716-765), generateConnections (drb.php:600-644) and
   generateVocabulary (drb.php:651-713). Each emitter appends bytes to
   `out` and advances `*addr` exactly as the PHP advances
   $currentAddress; none of them allocate except through `out`'s arena
   (str_arena, see str.h) or through Vec, matching
   the project's arena-only rule. */
#ifndef NDRC_BACK_EMIT_H
#define NDRC_BACK_EMIT_H

#include <stdio.h>

#include "diag.h"
#include "layout.h"
#include "model.h"
#include "str.h"
#include "targets.h"
#include "vec.h"

/* PORT: generateExterns, drb.php:105-129. Reads each entry's file
   (model.h's ExternEntry - already split into file_path/file_type at
   JSON-parse time, see model.c) from the CURRENT WORKING DIRECTORY and
   appends its bytes to out, sets adv->extvec[0/1/2] to the entry's
   START address (EXTERN/SFX/INT respectively), and unconditionally
   prints "TYPE path loaded at 0xNNNN" via diag_note (drb.php:126 is a
   plain, non-verbose-gated echo - diag_note's shape). ORDER FACT: the
   file's bytes are appended BEFORE the type-validity switch
   (drb.php:118 vs 119-125), so an invalid-type error leaves those
   bytes already emitted into out - ported as-is. Returns 0 after
   diag_fatal (missing file / bad type), 1 otherwise. t is unused
   (drb.php's generateExterns takes no target parameter of its own). */
int emit_externs(Str *out, long *addr, Diag *d, Adventure *adv, const Target *t);

/* PORT: drb.php:526-568 generateMessages. Used for messages, sysmess,
   locations and objects alike (drb.php:571-595 generateMTX/STX/LTX/
   OTX). dump_to_xmb and is_stx restore DRB's parameter surface
   (drb.php:532's LAST_DEFAULT_SYSMESS special case); xmb and xmb_addr
   restore drb.php:527's $XMBFileHandler/&$XMBCurrentAddress pair - the
   real XMB FILE stream and its write cursor, opened/closed by main.c
   (mirroring emit_xmb.c's own stdio
   conventions) and threaded through every call site regardless of that
   site's own dump_to_xmb value, since a 0 there makes the XMB-write
   branch below unreachable and xmb/xmb_addr simply unused. Both are
   nullable: NULL/ignored whenever dump_to_xmb is 0 for every message in
   msgs - the common case, and always true at OTX's own call site,
   which hard-codes 0 (main.c). */
void emit_messages(Str *out, long *addr, const Target *t, Vec_Message *msgs,
                    int dump_to_xmb, int is_stx, FILE *xmb, long *xmb_addr);

/* PORT: generateXMessages drb.php:449-524. Writes the XMB file(s) into
   the current working directory and fills adv->xmessage_offsets/size/
   padding/max_k. Returns 0 after reporting via diag_fatal (unsupported
   target, 64K overflow), 1 otherwise. */
int emit_xmessages(Diag *d, const Target *t, Adventure *adv);

/* PORT: drb.php:716-724 generateObjectNames. */
void emit_object_names(Str *out, long *addr, const Adventure *adv);

/* PORT: drb.php:737-755 generateObjectWeightAndAttr, including the two
   container warnings (drb.php:747, 749), reported via diag_note. */
void emit_object_weight_attr(Str *out, long *addr, Diag *d, const Adventure *adv);

/* PORT: drb.php:757-765 generateObjectExtraAttr. */
void emit_object_extra_attr(Str *out, long *addr, const Target *t, const Adventure *adv);

/* PORT: drb.php:726-735 generateObjectInitially. */
void emit_object_initially(Str *out, long *addr, const Adventure *adv);

/* PORT: drb.php:600-644 generateConnections. Returns the connections
   lookup table's address, as the PHP function does. */
long emit_connections(Str *out, long *addr, const Target *t, const Adventure *adv);

/* PORT: drb.php:651-713 generateVocabulary. */
void emit_vocabulary(Str *out, long *addr, const Adventure *adv);

/* PORT: drb.php:819-1225 generateProcesses - pass-zero condact
   rewrites (PROCESS existence check, XMES/XUNDONE/XPLAY/GETKEY/XDATA/
   XSPLITSCR and the eight deprecated condacts, PAUSE/BEEP duration
   adjustment), the tail-sharing dedup pre-pass and mid-emission
   registration (drb.php:1050-1130), bytecode emission (INDIR
   second-parameter indirection, the terminator set, the 0xFF
   no-terminator marker), and the entry/process tables. Defect detail:
   emit_proc.c (S12.7/S12.9/S12.2 notes at their sites). verbose is
   threaded explicitly; the PHP reads a global. Sets no return value:
   the caller computes the process table's own offset as
   `*addr - 2*process_count` after this returns, as drb.php:2033
   does. */
void emit_processes(Str *out, long *addr, Diag *d, const Target *t, Adventure *adv, int verbose);

/* PORT: drb.php:50-53 XPLAY_* value indices into mml_to_beep's values[4]
   (octave/volume/length/tempo state, carried across the whole MML
   string by emit_proc.c's XPLAY arm). Shared between that caller and
   emit_mml.c's mml_to_beep. */
#define XPLAY_OCTAVE 0
#define XPLAY_VOLUME 1
#define XPLAY_LENGTH 2
#define XPLAY_TEMPO  3

/* PORT: mmlToBeep drb.php:1593-1702. Parses ONE MML token (note is a
   NUL-terminated single token, e.g. "T140" or "F#4" - already split out
   by emit_proc.c's XPLAY arm reproducing drb.php:920-934's strpbrk
   tokenizer) into a rewritten condact (BEEP/PAUSE, or an SFX on MSX2's
   V arm) or returns 0 for state commands (L/O/T/V off MSX2/</>) and
   unrecognised characters (S/M/&), mutating values[4] in place exactly
   as PHP's by-reference $values parameter does. t->duration_base_length
   and t->pitch_adjustment (Task 1/1b) already carry what getBaseLength/
   getPitchAdjustment would resolve for t's target+subtarget, so no
   separate subtarget parameter is needed here.

   Returns 1 with *out set to the new condact (arena-allocated, zero-
   initialised so every unset PHP-undefined field reads as the
   observable-equivalent 0/NULL) when a condact is produced; 0 with
   *out set to NULL otherwise - including after the A-G arm's "Wrong
   length" diag_fatal (drb.php:1623), matching PHP's Error() halting
   the whole program: the caller must check diag_error_count(d) around
   the call and stop, same idiom as model.c's base_errors comparisons,
   since a 0 return alone cannot distinguish a legitimate state token
   from a reported fatal. */
int mml_to_beep(Arena *a, Diag *d, const char *note, long values[4],
                const Target *t, Condact **out);

#endif /* NDRC_BACK_EMIT_H */
