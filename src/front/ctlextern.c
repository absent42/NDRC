/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/ctlextern.c - Copyright (C) 2026 Dan Gibson.

   PORT: UCTLExtern.pas (D:/DRC/src, branch nextdaad). */
#include "ctlextern.h"

#include "str.h"
#include "vec.h"

struct CTLExternList {
    Vec_CStr *items;
};

CTLExternList *ctlextern_new(Arena *a)
{
    CTLExternList *l = arena_alloc(a, sizeof(*l));
    l->items = vec_new_CStr(a);
    return l;
}

void ctlextern_add(CTLExternList *list, Arena *a, const char *filename,
                    const char *extern_type)
{
    /* PORT: AddCTL_Extern (UCTLExtern.pas:14-15) - FileName + '|' +
       ExternType, tail-appended. */
    Str *s = str_new(a);
    str_append(s, filename);
    str_push(s, '|');
    str_append(s, extern_type);
    vec_push_CStr(list->items, str_cstr(s));
}

size_t ctlextern_count(const CTLExternList *list)
{
    return vec_len_CStr(list->items);
}

const char *ctlextern_at(const CTLExternList *list, size_t i)
{
    if (i >= vec_len_CStr(list->items)) return NULL;
    return vec_at_CStr(list->items, i);
}
