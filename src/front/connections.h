/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/connections.h - Copyright (C) 2026 Dan Gibson.

   PORT: UConnections.pas (D:/DRC/src, branch nextdaad, analysis
   26.2) - the location-connection ("exit") records. Location
   DESCRIPTION TEXT is a separate table, LTX, owned by
   messagelist.c/.h (matching the Pascal's own unit split). */
#ifndef NDRC_FRONT_CONNECTIONS_H
#define NDRC_FRONT_CONNECTIONS_H

#include <stddef.h>

#include "arena.h"
#include "vec.h"

/* PORT: TConnectionList's data fields (UConnections.pas:9-13), `Next`
   omitted (Vec-backed, append-only, tail-recursive AddConnection). */
typedef struct ConnectionRecord {
    long from_loc, to_loc, direction;
} ConnectionRecord;

VEC_DECLARE(ConnectionRecord, ConnectionRecord *)

typedef struct ConnectionList ConnectionList;

/* PORT: the unit's implicit initial state (Connections starts nil -
   there is no explicit init block in UConnections.pas, unlike
   UObjects.pas/UMessageList.pas, but the effect is the same empty
   start). */
ConnectionList *connectionlist_new(Arena *a);

/* PORT: AddConnection (UConnections.pas:23-34). Tail-append,
   unconditionally - NO duplicate check at this layer (FindConnection
   below is a separate query, never called from insertion; analysis
   26.2/defect 19.35: two different targets on the same direction from
   one location are both accepted here, and the reference never
   rejects that either - only an EXACT triple match is a duplicate). */
void connectionlist_add(ConnectionList *list, Arena *a, long from_loc,
                         long to_loc, long direction);

/* PORT: FindConnection (UConnections.pas:37-46). Linear scan for an
   EXACT (from_loc, to_loc, direction) triple - the only duplicate
   shape the reference rejects (19.35: a second connection from the
   same location, same direction, to a DIFFERENT target passes this
   check too, since the triple differs). Returns 1 on a hit, 0
   otherwise. */
int connectionlist_find(const ConnectionList *list, long from_loc,
                         long to_loc, long direction);

size_t connectionlist_count(const ConnectionList *list);
const ConnectionRecord *connectionlist_at(const ConnectionList *list,
                                           size_t i);

#endif /* NDRC_FRONT_CONNECTIONS_H */
