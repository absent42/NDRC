/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/symbols.c - Copyright (C) 2026 Dan Gibson. */
#include "symbols.h"

#include <string.h>

#include "map.h"
#include "str.h"

struct SymbolList {
    Map *m;
};

SymbolList *symbols_new(Arena *a)
{
    SymbolList *list = arena_alloc(a, sizeof(*list));
    list->m = map_new(a);
    return list;
}

int symbols_add(SymbolList *list, Arena *a, Diag *d, const char *name,
                 long value)
{
    char *folded = str_upper_latin1(a, name);
    long *box;

    /* AddSymbol: a duplicate folded name is rejected, existing value
       kept. map_get doubles as the existence test - no stored value is
       NULL. */
    if (map_get(list->m, folded) != NULL) return 0;

    box = arena_alloc(a, sizeof(*box));
    *box = value;
    map_put(list->m, folded, box);

    /* PORT: USymbolList.pas:39 - `Copy(ASymbol, 1, 4) <> '_VOC'`, tested
       against the ALREADY-folded name, exactly as the Pascal orders it
       (AnsiUpperCase runs before this check there too). strncmp is safe
       here even for a folded name shorter than 4 bytes: it stops at the
       embedded NUL either operand supplies first. */
    if (strncmp(folded, "_VOC", 4) != 0) {
        diag_verbose(d, "Added Symbol: %s=%ld", folded, value);
    }
    return 1;
}

int symbols_lookup(const SymbolList *list, Arena *a, const char *name,
                    long *out_value)
{
    char *folded = str_upper_latin1(a, name);
    /* GetSymbolValue: a single lookup answers both "does it exist" and
       "what is it" - no value stored here is ever NULL, so a NULL
       result is unambiguously a miss. */
    long *box = (long *)map_get(list->m, folded);

    if (box == NULL) return 0;
    *out_value = *box;
    return 1;
}

size_t symbols_count(const SymbolList *list)
{
    return map_len(list->m);
}

int symbols_at(const SymbolList *list, size_t i, const char **out_name,
               long *out_value)
{
    MapEntry e;
    if (!map_at(list->m, i, &e)) return 0;
    *out_name = e.key;
    *out_value = *(const long *)e.val;
    return 1;
}
