/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/messagelist.c - Copyright (C) 2026 Dan Gibson. */
#include "messagelist.h"

#include <string.h>

#include "constants.h"
#include "str.h"

struct MsgList {
    Vec_MsgEntry *items;
    int unlimited;
};

MsgList *msglist_new(Arena *a, int unlimited)
{
    MsgList *t = arena_alloc(a, sizeof(*t));
    t->items = vec_new_MsgEntry(a);
    t->unlimited = unlimited;
    return t;
}

void msglist_add(MsgList *t, Arena *a, long id, const char *text)
{
    MsgEntry *e = arena_alloc(a, sizeof(*e));
    e->id = id;
    e->text = arena_strdup(a, text);
    vec_push_MsgEntry(t->items, e);
}

long msglist_insert_or_dedup(MsgList *t, Arena *a, const char *text)
{
    size_t i, n = vec_len_MsgEntry(t->items);
    long last_id = -1;

    /* PORT: insertMessageFromProcessIntoSpecificList's nil-list branch
       (UMessageList.pas:47-51) - an empty list always inserts at id 0,
       skipping the dedup scan and the cap check entirely. */
    if (n == 0) {
        msglist_add(t, a, 0, text);
        return 0;
    }

    /* PORT: the WHILE scan (52-64) - exact text match (case- and
       accent-sensitive, pre-conversion) returns the EXISTING id
       without inserting. */
    for (i = 0; i < n; i++) {
        const MsgEntry *e = vec_at_MsgEntry(t->items, i);
        if (strcmp(e->text, text) == 0) return e->id;
        last_id = e->id;
    }

    /* PORT: the cap check (65-70) - only for a list that is not
       `unlimited` (XTX's "no limit by default" exemption), and only
       once the scan above has ruled out a dedup hit. -1 stands in for
       the Pascal's MAXLONGINT "no room" sentinel. */
    if (!t->unlimited && last_id == MAX_MESSAGES_PER_TABLE - 1) return -1;

    msglist_add(t, a, last_id + 1, text);
    return last_id + 1;
}

size_t msglist_count(const MsgList *t)
{
    return vec_len_MsgEntry(t->items);
}

const MsgEntry *msglist_at(const MsgList *t, size_t i)
{
    return vec_at_MsgEntry(t->items, i);
}

MessageList *messagelist_new(Arena *a)
{
    MessageList *ml = arena_alloc(a, sizeof(*ml));
    ml->mtx = msglist_new(a, 0);
    ml->stx = msglist_new(a, 0);
    ml->ltx = msglist_new(a, 0);
    ml->otx = msglist_new(a, 0);
    ml->xtx = msglist_new(a, 1); /* the one unlimited list */
    ml->other_tx = msglist_new(a, 0);
    ml->mtx_count = 0;
    ml->stx_count = 0;
    ml->ltx_count = 0;
    ml->otx_count = 0;
    ml->xtx_count = 0;
    ml->other_tx_count = 0;
    return ml;
}

long messagelist_insert_cascade(MessageList *ml, Arena *a, int *opcode,
                                 const char *text, int classic_mode)
{
    int orig_opcode = *opcode;
    MsgList *home = (orig_opcode == SYSMESS_OPCODE) ? ml->stx : ml->mtx;
    MsgList *other;
    long id = msglist_insert_or_dedup(home, a, text);

    /* PORT: `MessageID < MAX_MESSAGES_PER_TABLE` (UMessageList.pas:82).
       A literal transliteration would be wrong here: the Pascal
       compares against MAXLONGINT, a huge sentinel that is NEVER less
       than 255, so the check is really "did this succeed". This
       port's sentinel is -1, which genuinely IS less than 255 - so the
       success test is `id >= 0`, not `id < MAX_MESSAGES_PER_TABLE`. */
    if (id >= 0) return id;

    if (classic_mode) return -1; /* MAXLONGINT: hard failure */

    /* PORT: UMessageList.pas:95 - `AText := AText + '\n'`. Pascal
       string literals do not interpret backslash escapes, so this
       appends the two literal bytes 0x5C ('\\') 0x6E ('n'), NOT a
       newline byte. Only a MESSAGE_OPCODE retry gets this; the
       mutated text is reused for the LTX fallback below too, if the
       retry also fails. */
    if (orig_opcode == MESSAGE_OPCODE) {
        Str *s = str_new(a);
        str_append(s, text);
        str_append(s, "\\n");
        text = str_cstr(s);
    }

    other = (orig_opcode == SYSMESS_OPCODE) ? ml->mtx : ml->stx;
    id = msglist_insert_or_dedup(other, a, text);
    if (id >= 0) {
        /* PORT: UMessageList.pas:101 - the swap is asymmetric, NOT a
           plain MESSAGE<->SYSMESS toggle: a SYSMESS retry becomes MES,
           not MESSAGE; a MESSAGE or MES retry becomes SYSMESS. */
        *opcode = (orig_opcode == SYSMESS_OPCODE) ? MES_OPCODE
                                                   : SYSMESS_OPCODE;
        return id;
    }

    /* PORT: UMessageList.pas:105-108 - unconditional LTX fallback.
       `*opcode` becomes DESC_OPCODE regardless of whether THIS attempt
       finds room; its result (id >= 0, or -1) is returned as-is with
       no further check - the one path in this cascade that can hand a
       caller "no room" with no distinguishing signal (defect 19.51). */
    id = msglist_insert_or_dedup(ml->ltx, a, text);
    *opcode = DESC_OPCODE;
    return id;
}
