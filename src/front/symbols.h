/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/symbols.h - Copyright (C) 2026 Dan Gibson.

   PORT: USymbolList.pas (D:/DRC/src, branch nextdaad) - the symbol
   table: #define'd names, the auto-generated _VOC_<WORD> vocabulary
   symbols, and the built-ins drf.pas injects while parsing
   (drf.pas:233-289). Container only - WHICH symbols get defined is
   each call site's job.

   Backing store: map.h's Map, insertion-ordered, keys pre-folded
   through str_upper_latin1 (AnsiUpperCase's fold). Duplicate
   semantics: see symbols_add below. */
#ifndef NDRC_FRONT_SYMBOLS_H
#define NDRC_FRONT_SYMBOLS_H

#include "arena.h"
#include "diag.h"

typedef struct SymbolList SymbolList;

SymbolList *symbols_new(Arena *a);

/* PORT: AddSymbol (USymbolList.pas:29-46). Folds `name` through
   str_upper_latin1 before storing or comparing - this IS the
   AnsiUpperCase call at the top of AddSymbol, not a separate step: a
   caller must NOT pre-fold `name` itself. Returns 1 and stores the
   symbol on success; returns 0 and leaves the table unchanged if the
   folded name already exists (duplicate - AddSymbol's `Result := false`
   leg; the ORIGINAL value is kept, matching the Pascal, which never
   descends into the true branch on a match).

   PORT: also reproduces AddSymbol's own verbose side effect
   (USymbolList.pas:39): on a successful, non-duplicate insertion, if
   the folded name's first four bytes are NOT "_VOC", emits
   `Added Symbol: <FOLDED-NAME>=<value>` via diag_verbose (silent unless
   diag_set_verbose was turned on - this is the direct analogue of
   Pascal's global `Verbose` flag gating the same WriteLn). `d` must not
   be NULL. */
int symbols_add(SymbolList *list, Arena *a, Diag *d, const char *name,
                 long value);

/* PORT: GetSymbolValue (USymbolList.pas:48-54). Folds `name` through
   str_upper_latin1 (an arena is needed for that fold, hence the `a`
   parameter this lookup-only function would not otherwise need) and
   looks it up. Returns 1 and writes *out_value on a hit; returns 0 (and
   leaves *out_value untouched) on a miss - a safer C shape for "not
   found" than reproducing MAXLONGINT-as-sentinel, which risked
   colliding with a symbol genuinely defined to that value. */
int symbols_lookup(const SymbolList *list, Arena *a, const char *name,
                    long *out_value);

/* Entries in insertion order, for a caller (JSON export, ultimately)
   that needs to walk or reverse the table itself - map.h's map_at
   surface, one level up. Returns 1 and fills *out_name and *out_value
   for index i, 0 if i is out of range. *out_name is the FOLDED
   (stored) name. */
size_t symbols_count(const SymbolList *list);
int symbols_at(const SymbolList *list, size_t i, const char **out_name,
               long *out_value);

#endif /* NDRC_FRONT_SYMBOLS_H */
