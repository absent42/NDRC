/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/map.h - string-keyed hash map with insertion-ordered iteration.
   Copyright (C) 2026 Dan Gibson.

   Iteration order is insertion order, not hash order. Insertion order is
   correct for the SYMBOL table's storage, but DRC's JSON export emits
   symbols in REVERSE insertion order (getSymbolsJSON recurses on Next
   before emitting, UJSONExport.pas:26-30) - a caller producing that JSON
   must reverse map_at's order itself.

   The VOCABULARY is not map-shaped at all in DRC: it is a binary search
   tree keyed on FixSpanishChars(AnsiUpperCase(word)) and emitted sorted
   by that transformed key (UVocabularyTree.pas:75-79,
   UJSONExport.pas:32-36). A vocabulary table must NOT be built on this
   map's iteration order.

   Keys are compared exactly. DRF normalises symbols with AnsiUpperCase,
   which folds Latin-1 (verified: DRF rejects ca+e-acute vs CA+E-acute as
   duplicates), so str_upper_ascii is the WRONG normaliser for symbol
   keys - see docs/dev/phase0-inference-audit.md finding 4. str_upper_latin1
   (str.h) is the fold; src/front/symbols.c is the symbol table built on
   this map, folding every key through it before map_put/map_get. */
#ifndef NDRC_MAP_H
#define NDRC_MAP_H

#include <stddef.h>
#include "arena.h"

typedef struct MapEntry {
    const char *key;
    void *val;
} MapEntry;

typedef struct Map Map;

Map *map_new(Arena *a);

/* Inserts or replaces. The key is copied into the arena, so callers may
   pass a stack buffer. Replacing keeps the original insertion position.
   A NULL value is stored, and is distinguishable from absence via
   map_has. */
void map_put(Map *m, const char *key, void *val);

/* Returns NULL when absent. Use map_has to tell absence from a stored
   NULL. */
void *map_get(const Map *m, const char *key);

int map_has(const Map *m, const char *key);

size_t map_len(const Map *m);

/* Entry i in insertion order. Returns 1 and fills *out, or returns 0
   for an out-of-range index. The caller owns the copy, so it cannot go
   stale across a later map_put. */
int map_at(const Map *m, size_t i, MapEntry *out);

#endif /* NDRC_MAP_H */
