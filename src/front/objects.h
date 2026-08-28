/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/objects.h - Copyright (C) 2026 Dan Gibson.

   PORT: UObjects.pas (D:/DRC/src, branch nextdaad, analysis 26.3) -
   the structured per-object FIELD records (noun, adjective, weight,
   flags, ...). Object DESCRIPTION TEXT is a separate table, OTX,
   owned by messagelist.c/.h (matching the Pascal's own unit split). */
#ifndef NDRC_FRONT_OBJECTS_H
#define NDRC_FRONT_OBJECTS_H

#include <stddef.h>

#include "arena.h"
#include "vec.h"

/* PORT: TObjectList's data fields (UObjects.pas:9-16), `Next` omitted
   (Vec-backed, append-only). Field names/types mirror the Pascal
   (and, not coincidentally, the JSON export field names, analysis
   26.3): `flags` is 16 bits (Word) matching a 16-custom-flag
   assembly; `container`/`wearable` are booleans. */
typedef struct ObjectRecord {
    long value;
    long noun, adjective;
    int container, wearable;
    long weight, initially_at;
    unsigned flags;
} ObjectRecord;

VEC_DECLARE(ObjectRecord, ObjectRecord *)

typedef struct ObjectList {
    Vec_ObjectRecord *items;
    unsigned carried_count; /* CarriedObjects (Word) */
    unsigned worn_count;    /* WornObjects (Word) */
} ObjectList;

/* PORT: the unit's initialization block (UObjects.pas:59-62):
   ObjectList nil, CarriedObjects/WornObjects 0. */
ObjectList *objectlist_new(Arena *a);

/* PORT: AddObject (UObjects.pas:30-48). Tail-append, unconditionally -
   NO duplicate noun/adjective rejection at this layer (FindObject
   below is a separate query, never called from insertion, 26.3);
   order is preserved (matches the JSON export order). Also reproduces
   the CarriedObjects/WornObjects side-effect counters, incremented
   when `initially_at` is LOC_CARRIED/LOC_WORN respectively - used by
   the post-section built-in NUM_CARRIED/NUM_WORN-style symbols
   (analysis 14.1); reading these back and defining the symbols is
   Task 6/7's job, this container only counts. */
void objectlist_add(ObjectList *list, Arena *a, long value, long noun,
                     long adjective, long weight, long initially_at,
                     unsigned flags, int container, int wearable);

/* PORT: FindObject (UObjects.pas:51-57). Linear scan for an EXISTING
   (noun, adjective) pair, used by the SYNONYM/convertible-noun paths
   (analysis 24.4, Task 6/7's concern to call this) - exposed here
   since it is part of this unit's own surface. Returns 1 on a hit, 0
   otherwise (the Pascal has no "which object" return - a boolean
   existence test only). */
int objectlist_find(const ObjectList *list, long noun, long adjective);

size_t objectlist_count(const ObjectList *list);
const ObjectRecord *objectlist_at(const ObjectList *list, size_t i);

#endif /* NDRC_FRONT_OBJECTS_H */
