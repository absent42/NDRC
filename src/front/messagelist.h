/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/messagelist.h - Copyright (C) 2026 Dan Gibson.

   PORT: UMessageList.pas (D:/DRC/src, branch nextdaad) - all SIX text
   tables the Pascal unit declares (MTX, STX, LTX, OTX, XTX, OtherTX,
   UMessageList.pas:15), paired 1:1 with the JSON keys messages,
   sysmess, locations, objects, xmessages, other_strings. LTX/OTX hold
   location/object DESCRIPTION TEXT here; the structured field records
   live in objects.c/connections.c.

   The six *Count fields (UMessageList.pas:17) are plain mutable state
   the /MTX /STX /LTX /OTX section parser sets directly and they stay
   FROZEN afterwards even as messagelist_insert_cascade keeps
   appending during /PRO parsing - SemanticCheck compares against
   these frozen SECTION counts, not the live table length, so a
   reference to an auto-inserted overflow message is a semantic error
   even though the message now exists. */
#ifndef NDRC_FRONT_MESSAGELIST_H
#define NDRC_FRONT_MESSAGELIST_H

#include <stddef.h>

#include "arena.h"
#include "vec.h"

/* PORT: TMessageList's data fields (UMessageList.pas:9-13), `Next`
   omitted - list order is this container's own concern (Vec-backed,
   append-only, matching the Pascal's tail-recursive AddMessage). */
typedef struct MsgEntry {
    long id;
    const char *text; /* verbatim minus the two delimiter quotes;
                          character conversion happens at JSON export,
                          not here (26.4) */
} MsgEntry;

VEC_DECLARE(MsgEntry, MsgEntry *)

/* One of the six lists. Named MsgList (not "MsgTable") to keep clear
   distance from model.h's unrelated Vec_MsgTable (a vector of whole
   per-XMESSAGE-table entries in the BACK end's model) - this front-end
   container never includes model.h and the two are not interchangeable. */
typedef struct MsgList MsgList;

/* `unlimited` selects XTX's "no limit by default" exemption
   (insertMessageFromProcessIntoSpecificList's `IF AMessageList <> XTX`
   check, 26.4) - pass 1 only for the table messagelist_new wires up as
   xtx below; every other list is capped at MAX_MESSAGES_PER_TABLE
   (255) entries, ids 0..254. */
MsgList *msglist_new(Arena *a, int unlimited);

/* PORT: AddMessage (UMessageList.pas:30-40). Tail-append with an
   EXPLICIT id - no validation at all. Consecutive-numbering and the
   254 ceiling are the /MTX /STX /LTX /OTX section parser's job
   (USintactic.pas ParseMessageList, Task 6/7), exactly where the
   Pascal unit boundary draws it: AddMessage itself performs no check. */
void msglist_add(MsgList *t, Arena *a, long id, const char *text);

/* PORT: insertMessageFromProcessIntoSpecificList (UMessageList.pas:
   43-74). An empty list inserts at id 0. Otherwise: a full linear scan
   for an EXACT text match (case- and accent-sensitive, pre-conversion)
   returns the EXISTING id without inserting (message deduplication);
   failing that, if the list is not `unlimited` and the last entry's id
   is MAX_MESSAGES_PER_TABLE-1 (254), returns -1 ("no room", this
   port's stand-in for the Pascal's MAXLONGINT sentinel - -1 can never
   collide with a real id, which is always >= 0); otherwise appends at
   LastMessageID+1 and returns that id. */
long msglist_insert_or_dedup(MsgList *t, Arena *a, const char *text);

size_t msglist_count(const MsgList *t);
const MsgEntry *msglist_at(const MsgList *t, size_t i);

/* PORT: the six VAR TPMessageList / *Count pairs (UMessageList.pas:
   15-17), grouped into one container the way symbols.c/labels.c
   already group their own unit's globals into an instance. */
typedef struct MessageList {
    MsgList *mtx, *stx, *ltx, *otx, *xtx, *other_tx;
    long mtx_count, stx_count, ltx_count, otx_count, xtx_count,
        other_tx_count;
} MessageList;

/* PORT: the unit's initialization block (UMessageList.pas:111-117,
   all six lists nil) plus the *Count reset ParseCTL's own preamble
   performs (USintactic.pas:940-944) - MTX/STX/LTX/OTX/XTX/OtherTX
   counts all start at 0. */
MessageList *messagelist_new(Arena *a);

/* PORT: insertMessageFromProcess (UMessageList.pas:76-109, defect
   19.51) - the MTX/STX overflow cascade, retrying into the other
   table then LTX. `*opcode` is read and, on any but the classic-mode
   hard-failure path (step 1 fails, returns -1 unchanged), possibly
   rewritten in place. The final LTX fallback's result passes straight
   back with no further check - the one path where "no room" can reach
   a caller silently (19.51). Branch detail: messagelist.c. */
long messagelist_insert_cascade(MessageList *ml, Arena *a, int *opcode,
                                 const char *text, int classic_mode);

#endif /* NDRC_FRONT_MESSAGELIST_H */
