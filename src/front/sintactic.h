/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/sintactic.h - Copyright (C) 2026 Dan Gibson.

   PORT: USintactic.pas - parse driver, preprocessor, world-section
   grammar, condact machinery, FixForwardLabels. Unit globals are
   file-scope statics in sintactic.c; the getters below are the read
   surface for symbol injection, JSON export and tests. */
#ifndef NDRC_FRONT_SINTACTIC_H
#define NDRC_FRONT_SINTACTIC_H

#include "arena.h"
#include "diag.h"

#include "connections.h"
#include "ctlextern.h"
#include "labels.h"
#include "messagelist.h"
#include "objects.h"
#include "process.h"
#include "symbols.h"
#include "tokenlist.h"
#include "voctree.h"

/* The nine drf.pas option flags (UConstants.pas VAR block, set by
   drf.pas:351-413) plus target/subtarget. Task 9's CLI parse fills
   this; tests fill it by hand. Zero-initialising the struct gives
   every flag drf.pas's own default EXCEPT check_maluva, which
   defaults TRUE in the reference (UConstants.pas: CheckMaluva) and is
   cleared by -check-maluva-disabled - callers must set it to 1 for
   default behaviour. */
typedef struct FrontOptions {
    const char *target;    /* upper-cased target name (drf.pas:328) */
    const char *subtarget; /* upper-cased subtarget, or "" / NULL */
    int v3;                    /* -v3 (V3CODE) */
    int verbose;               /* -verbose (Verbose; diag_set_verbose
                                  must be set to match - symbols.c and
                                  this file both gate on diag's flag) */
    int ascii7;                /* -7 (ASCII7) */
    int no_semantic;           /* -no-semantic (NoSemantic) */
    int semantic_warnings;     /* -semantic-warnings */
    int force_normal_messages; /* -force-normal-messages */
    int force_x_messages;      /* -force-x-messages */
    int check_maluva;          /* CheckMaluva; -check-maluva-disabled
                                  clears it (default 1) */
    int replace_xcondacts;     /* -replace-xcondacts */
} FrontOptions;

/* Creates fresh instances of every front container (the analogue of
   the Pascal units' initialization blocks running at program start)
   and resets the parser's file-scope state. Must be called before
   sintactic_parse; call sintactic_symbols() after it to inject the
   drf.pas built-in symbols (Task 9) or test symbols. */
void sintactic_init(Arena *a, Diag *d);

/* PORT: Sintactic(ATarget, ASubtarget) (USintactic.pas:934-958).
   `stream` is the token list head as lex_tokenize returns it (the
   FIRST REAL token - drf.pas's fake T_NOTHING head token, drf.pas:230,
   is created internally here since the C token stream carries no fake
   head; PORT NOTE in sintactic.c). Parses /CTL through /END, building
   the model in the containers below. Returns 0 on success, or the
   diag exit class (1 syntax, 2 fatal) after the FIRST error - the
   Pascal Halt()s inside SyntaxError; this port longjmps out of the
   recursive descent instead, with the diagnostic already reported. */
int sintactic_parse(Arena *a, Diag *d, Token *stream,
                     const FrontOptions *opts);

/* PORT: FixForwardLabels (USintactic.pas:56-93). drf.pas:302-304
   calls it as a separate step after Sintactic, so it stays separate
   here. Call only after a 0-return parse; errors reuse /END's
   position (reference behaviour). Returns 0, or the diag exit class
   after the first error. */
int sintactic_fix_forward_labels(void);

/* Unit-global read surface (valid after sintactic_init). */
SymbolList *sintactic_symbols(void);
VocTree *sintactic_voctree(void);
MessageList *sintactic_messages(void);
ConnectionList *sintactic_connections(void);
ObjectList *sintactic_objects(void);
ProcessTable *sintactic_processes(void);
LabelTable *sintactic_labels(void);
CTLExternList *sintactic_externs(void);

/* PORT: USintactic.pas interface VARs ClassicMode / DebugMode /
   MaluvaUsed / LastProcess (UProcess.pas), consumed by the JSON
   settings object and the export walk (Task 8). */
int sintactic_classic_mode(void);
int sintactic_debug_mode(void);
int sintactic_maluva_used(void);
long sintactic_last_process(void);

#endif /* NDRC_FRONT_SINTACTIC_H */
