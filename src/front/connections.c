/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/connections.c - Copyright (C) 2026 Dan Gibson. */
#include "connections.h"

struct ConnectionList {
    Vec_ConnectionRecord *items;
};

ConnectionList *connectionlist_new(Arena *a)
{
    ConnectionList *list = arena_alloc(a, sizeof(*list));
    list->items = vec_new_ConnectionRecord(a);
    return list;
}

void connectionlist_add(ConnectionList *list, Arena *a, long from_loc,
                         long to_loc, long direction)
{
    ConnectionRecord *r = arena_alloc(a, sizeof(*r));
    r->from_loc = from_loc;
    r->to_loc = to_loc;
    r->direction = direction;
    vec_push_ConnectionRecord(list->items, r);
}

int connectionlist_find(const ConnectionList *list, long from_loc,
                         long to_loc, long direction)
{
    size_t i, n = vec_len_ConnectionRecord(list->items);
    for (i = 0; i < n; i++) {
        const ConnectionRecord *r = vec_at_ConnectionRecord(list->items, i);
        if (r->from_loc == from_loc && r->to_loc == to_loc &&
            r->direction == direction) {
            return 1;
        }
    }
    return 0;
}

size_t connectionlist_count(const ConnectionList *list)
{
    return vec_len_ConnectionRecord(list->items);
}

const ConnectionRecord *connectionlist_at(const ConnectionList *list,
                                           size_t i)
{
    return vec_at_ConnectionRecord(list->items, i);
}
