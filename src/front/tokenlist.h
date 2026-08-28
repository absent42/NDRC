/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/tokenlist.h - Copyright (C) 2026 Dan Gibson.

   PORT: UTokenList.pas (D:/DRC/src, branch nextdaad) - the TTokenList
   node shape and AddToken's append (UTokenList.pas:7-53) - plus
   lex_tokenize, the entry point lexer.pas's yylex/DSF.l's 43 actions
   compile down to (defined in lex.c; declared here since it returns
   this file's Token type, mirroring how src/back/emit.h collects
   declarations for several emit_*.c definitions).

   Token drops Pascal's `Previous` back-link (UTokenList.pas:16): no
   reader anywhere in the units this port covers ever follows it, and
   the Token struct here (the struct Task 6 consumes) carries no such
   field either. */
#ifndef NDRC_FRONT_TOKENLIST_H
#define NDRC_FRONT_TOKENLIST_H

#include "arena.h"
#include "diag.h"

typedef struct Token {
    int id;
    const char *text;
    long value;
    int line, col;
    struct Token *next;
} Token;

/* Pascal MaxLongInt, the lexer's "no literal value" sentinel; the
   literal 2147483647 collides with it (defect 19.13) - the parser
   owns that consequence. */
#define TOKEN_NO_VALUE 2147483647L

typedef struct TokenList TokenList;

TokenList *tokenlist_new(Arena *a);

/* PORT: UTokenList.pas's AddToken (UTokenList.pas:39-53). Appends by
   walking to the CURRENT tail on every call - deliberately NOT cached
   as a tail pointer, reproducing the O(n^2) total cost over a file's
   whole token stream (defect 19.17, PERF-NOTE) - ported as-is, not
   fixed. Stores col = yycolno - 1
   internally (UTokenList.pas:49), so callers pass the RAW yycolno
   exactly as every one of lexer.pas's 43 actions does - do not
   pre-subtract at the call site. `text` is stored by pointer, not
   copied: the caller must pass arena-owned (or otherwise
   process-lifetime) storage. */
void tokenlist_add(TokenList *list, Arena *a, int id, const char *text,
                    long value, int lineno, int yycolno);

/* The list built so far, head first. NULL if tokenlist_add has never
   been called (an empty source file, or every character was
   whitespace/a comment). */
Token *tokenlist_head(const TokenList *list);

/* PORT: lexer.pas's yylex, driving lex_tables.h (the emitted DFA) and
   DSF.l's 43 rule actions over lexlib.c - see lex.c for the walk and
   per-rule port. Single pass to EOF; calls diag_set_source(d, path)
   itself, so `path` is the source name for every diagnostic raised
   here.

   Returns the list head (NULL if empty), or NULL after diag reports:
   DSF.l's rule-43 catch-all (diag_syntax_error, exit class 1), an
   out-of-int32 literal (19.13, diag_fatal, exit class 2), or `path`
   failing to open (diag_fatal). */
Token *lex_tokenize(Arena *a, Diag *d, const char *path);

#endif /* NDRC_FRONT_TOKENLIST_H */
