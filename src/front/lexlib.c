/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/lexlib.c - Copyright (C) 2026 Dan Gibson.
   PORT: ULexLib.pas's I/O routines section - see lexlib.h for the
   full port-scope note. */
#include "lexlib.h"

#include <stdio.h>
#include <stdlib.h>

/* Matches ULexLib.pas's `const max_chars = 8192;` (ULexLib.pas:176) -
   both the line-buffer capacity and the unget_char overflow limit. */
#define LEXLIB_MAX_CHARS 8192

int yylineno;
int yycolno;

static char g_yytext_empty[1] = "";
char *yytext = g_yytext_empty;
unsigned yyleng;

static FILE *g_file = NULL;

/* The input buffer (ULexLib.pas:180-181's `bufptr`/`buf`), reworked
   from Pascal's 1-based array-plus-index-pointer into a plain 0-based
   stack: bufptr counts the characters currently buffered (0 = empty),
   and the next character to pop is buf[bufptr - 1]. This is the same
   8192-slot LIFO Pascal implements (one line's characters, reversed,
   plus the synthetic trailing '\n'), just addressed more directly;
   capacity and overflow behaviour are unchanged. */
static unsigned char buf[LEXLIB_MAX_CHARS];
static int bufptr;

/* One short of LEXLIB_MAX_CHARS so the synthetic '\n' still fits;
   fetch_line FATALs first. */
static unsigned char g_line[LEXLIB_MAX_CHARS - 1];
static size_t g_line_len;

static void lexlib_fatal(const char *msg)
{
    /* PORT: ULexLib.pas:165-170 fatal() - same text and exit 1; stderr
       rather than Pascal's stdout (deliberate, unobservable). */
    fprintf(stderr, "LexLib: %s\n", msg);
    exit(1);
}

int lexlib_open(const char *path)
{
    if (g_file != NULL) {
        fclose(g_file);
        g_file = NULL;
    }
    g_file = fopen(path, "rb");
    if (g_file == NULL) return 0;

    bufptr = 0;
    g_line_len = 0;
    yylineno = 0;
    yycolno = 0;   /* Pascal never initialises yycolno either - see lexlib.h */
    yytext = g_yytext_empty;
    yyleng = 0;
    return 1;
}

void lexlib_close(void)
{
    if (g_file != NULL) {
        fclose(g_file);
        g_file = NULL;
    }
    bufptr = 0;
    g_line_len = 0;
}

/* Mirrors FPC readln (ULexLib.pas:189): reads to '\n' or EOF, drops a
   '\r' only immediately before '\n'; returns 0 only at true EOF
   (ULexLib.pas:187) - a final unterminated line still returns 1.
   FATALs at the 8192nd char - own wording; the Pascal 'Lexer overflow'
   text belongs to yyscan's token check. */
static int fetch_line(void)
{
    int c;

    if (g_file == NULL) return 0;

    c = fgetc(g_file);
    if (c == EOF) return 0;

    g_line_len = 0;
    for (;;) {
        if (c == EOF) break;
        if (c == '\n') {
            if (g_line_len > 0 && g_line[g_line_len - 1] == '\r')
                g_line_len--;
            break;
        }
        if (g_line_len == LEXLIB_MAX_CHARS - 1)
            lexlib_fatal("source line too long (over 8191 characters)");
        g_line[g_line_len++] = (unsigned char)c;
        c = fgetc(g_file);
    }
    return 1;
}

unsigned char get_char(void)
{
    unsigned char c;

    /* PORT: ULexLib.pas:187-199 - `if (bufptr=0) and not eof(yyinput)
       then begin readln(...); ... end;`. */
    if (bufptr == 0) {
        if (fetch_line()) {
            size_t i, len = g_line_len;

            /* buf[0] is the synthetic trailing '\n' (Pascal buf[1],
               ULexLib.pas:192); buf[1..len] hold the line's
               characters in REVERSE (Pascal buf[2..length+1],
               ULexLib.pas:193-197), so the stack pops them back out
               in forward (original) order. */
            buf[0] = '\n';
            for (i = 0; i < len; i++)
                buf[i + 1] = g_line[len - 1 - i];
            bufptr = (int)(len + 1);

            yylineno++;
            yycolno = 1;
        }
    }

    /* PORT: ULexLib.pas:200-207. */
    if (bufptr > 0) {
        c = buf[bufptr - 1];
        bufptr--;
        yycolno++;
        return c;
    }
    return 0;
}

void unget_char(unsigned char c)
{
    /* PORT: ULexLib.pas:210-216. */
    if (bufptr == LEXLIB_MAX_CHARS)
        lexlib_fatal("input buffer overflow");
    buf[bufptr] = c;
    bufptr++;
    yycolno--;
}
