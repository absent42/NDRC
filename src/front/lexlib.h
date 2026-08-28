/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/lexlib.h - Copyright (C) 2026 Dan Gibson.

   PORT: ULexLib.pas I/O routines only (get_char/unget_char, the line
   buffer, ULexLib.pas:51-79 doc comment). The utility routines and
   the yylex driver internals are ported in lex.c, not here; put_char
   (ULexLib.pas:77) is dead for drf and unported.

   File-scope state mirrors the Pascal unit vars; lexlib_open() fully
   resets it (ULexLib.pas:422-428), so repeated open/close within one
   process is safe - each open is a fresh scan.

   Character type: Pascal Char is one byte; ULexLib.pas performs no
   decoding of it (the ISO-8859-1 assumption - bytes simply pass
   through). get_char/unget_char here use `unsigned char` throughout
   so that bytes 128-255 round-trip without sign-extension surprises;
   get_char's EOF return value 0 is a real, in-range Char value (#0),
   exactly as in Pascal - not a distinct sentinel outside the type's
   range. */
#ifndef NDRC_FRONT_LEXLIB_H
#define NDRC_FRONT_LEXLIB_H

/* Opens `path` for scanning (binary mode - this module implements its
   own CRLF/LF line splitting rather than relying on the C runtime's
   text-mode translation, so behaviour is identical on every host).
   Fully resets yylineno, yycolno, yytext, yyleng and the input buffer,
   mirroring ULexLib.pas's unit initialization block. Returns 0 on
   failure (fopen failed), 1 on success. Closes a previously-open file
   first if the caller opens a second one without calling
   lexlib_close(). */
int lexlib_open(const char *path);

/* Closes the currently-open file, if any. Safe to call when nothing
   is open. Does not itself reset yylineno/yycolno/yytext/yyleng - a
   following lexlib_open() does that. */
void lexlib_close(void);

/* Returns 0 at EOF - a real in-range Char value (#0), as in Pascal,
   not a sentinel. Buffers one line reversed onto an 8192-slot stack
   matching ULexLib.pas's buf (ULexLib.pas:176,181), with a synthetic
   trailing '\n'; bare CR is data, not a terminator (readln semantics,
   ULexLib.pas:183-208). Lines over 8191 chars FATAL - a guard the
   Pascal line-copy loop (ULexLib.pas:193-197) lacks, overflowing buf
   silently instead. */
unsigned char get_char(void);

/* unget_char: returns one character to the input buffer, to be reread
   by a later get_char (ULexLib.pas:210-216) - used for pushback when
   a scan overshoots a token boundary. Pushing back onto an already-
   full buffer (bufptr already at the 8192-slot capacity - i.e. an
   8191-character line already loaded in full) FATALs with the
   byte-exact Pascal message `LexLib: input buffer overflow`
   (ULexLib.pas:212) - this IS existing Pascal behaviour, reproduced
   as-is, not a new guard. */
void unget_char(unsigned char c);

/* Position bookkeeping (ULexLib.pas:47,183-216): yylineno is 1-based
   after the first line loads; yycolno == N+1 after reading the Nth
   char of the current line and decrements on each unget_char (start
   column = yycolno - yyleng, as DRC's AddToken computes). Both 0
   before the first get_char, as in Pascal. */
extern int yylineno;
extern int yycolno;

/* yytext/yyleng (ULexLib.pas:46,48-49): the current match and its
   length, exposed as file-scope state exactly as the Pascal unit
   exposes them. This module only initialises them (empty, 0) at
   lexlib_open() and lexlib_close() - it does not itself accumulate a
   match (that is yyscan's job, ULexLib.pas:344-350, left to the
   driver layer per the file-header note above). yytext is a
   reassignable pointer, mirroring how Pascal's yyscan replaces
   yytext's value wholesale (`yytext := yytext + yyactchar`) rather
   than appending in place; ownership of whatever yytext is pointed at
   belongs to whichever code last assigned it. */
extern char *yytext;
extern unsigned yyleng;

#endif /* NDRC_FRONT_LEXLIB_H */
