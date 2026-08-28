/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/objects.c - Copyright (C) 2026 Dan Gibson. */
#include "objects.h"

#include "constants.h"

ObjectList *objectlist_new(Arena *a)
{
    ObjectList *list = arena_alloc(a, sizeof(*list));
    list->items = vec_new_ObjectRecord(a);
    list->carried_count = 0;
    list->worn_count = 0;
    return list;
}

void objectlist_add(ObjectList *list, Arena *a, long value, long noun,
                     long adjective, long weight, long initially_at,
                     unsigned flags, int container, int wearable)
{
    ObjectRecord *r = arena_alloc(a, sizeof(*r));
    r->value = value;
    r->noun = noun;
    r->adjective = adjective;
    r->container = container;
    r->wearable = wearable;
    r->weight = weight;
    r->initially_at = initially_at;
    r->flags = flags;
    vec_push_ObjectRecord(list->items, r);

    /* PORT: UObjects.pas:45-46. */
    if (initially_at == LOC_CARRIED) list->carried_count++;
    if (initially_at == LOC_WORN) list->worn_count++;
}

int objectlist_find(const ObjectList *list, long noun, long adjective)
{
    size_t i, n = vec_len_ObjectRecord(list->items);
    for (i = 0; i < n; i++) {
        const ObjectRecord *r = vec_at_ObjectRecord(list->items, i);
        if (r->noun == noun && r->adjective == adjective) return 1;
    }
    return 0;
}

size_t objectlist_count(const ObjectList *list)
{
    return vec_len_ObjectRecord(list->items);
}

const ObjectRecord *objectlist_at(const ObjectList *list, size_t i)
{
    return vec_at_ObjectRecord(list->items, i);
}
