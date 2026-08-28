/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/tokenlist.c - Copyright (C) 2026 Dan Gibson.
   PORT: UTokenList.pas - see tokenlist.h for the full port-scope note. */
#include "tokenlist.h"

struct TokenList {
    Token *head;
};

TokenList *tokenlist_new(Arena *a)
{
    TokenList *l = arena_alloc(a, sizeof(*l));
    l->head = NULL;
    return l;
}

void tokenlist_add(TokenList *list, Arena *a, int id, const char *text,
                    long value, int lineno, int yycolno)
{
    Token *t = arena_alloc(a, sizeof(*t));
    Token *cur;

    t->id = id;
    t->text = text;
    t->value = value;
    t->line = lineno;
    t->col = yycolno - 1;   /* PORT: UTokenList.pas:49 */
    t->next = NULL;

    if (list->head == NULL) {
        list->head = t;
        return;
    }

    /* PORT: UTokenList.pas:39-53's tail recursion - walk to the
       current end on EVERY call, reproducing the O(n^2) total cost
       (defect 19.17, deliberate - see tokenlist.h). */
    cur = list->head;
    while (cur->next != NULL) cur = cur->next;
    cur->next = t;
}

Token *tokenlist_head(const TokenList *list)
{
    return list->head;
}
