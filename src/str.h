/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/str.h - binary-safe growable byte string.
   Copyright (C) 2026 Dan Gibson.

   Serves as both the text builder and the DDB output buffer. It tracks
   length rather than relying on termination, so embedded NUL bytes are
   ordinary data. A NUL is always kept one past the end so str_cstr can
   hand back a C string without copying.

   Growth invalidates interior pointers: a str_cstr or str_bytes result
   obtained before a call that grows the string is stale afterwards and
   must not be read. Under AddressSanitizer the abandoned buffer is
   poisoned (arena_poison, arena.h), so a stale read is reported by the
   sanitize build rather than silently returning old bytes. */
#ifndef NDRC_STR_H
#define NDRC_STR_H

#include <stddef.h>
#include "arena.h"
#include "vec.h"

typedef struct Str Str;

VEC_DECLARE(Str, Str *)

Str *str_new(Arena *a);
Str *str_from(Arena *a, const char *s);

void str_push(Str *s, char c);
void str_append(Str *s, const char *t);
void str_append_n(Str *s, const void *t, size_t n);

/* printf-style append. Output of any length is handled. */
void str_appendf(Str *s, const char *fmt, ...);

/* Byte emission. Values are truncated to the written width. */
void str_push_u8(Str *s, unsigned v);
void str_push_u16le(Str *s, unsigned v);
void str_push_u16be(Str *s, unsigned v);

/* In-place back-patching for address fixups. Writing past the current
   length is a programming error and aborts. */
void str_set_u8(Str *s, size_t at, unsigned v);
void str_set_u16le(Str *s, size_t at, unsigned v);
void str_set_u16be(Str *s, size_t at, unsigned v);

/* Endianness-parameterised word helpers. The big_endian flag describes
   the output byte order (false = little-endian, true = big-endian). See
   analysis S24 for why the file's byte order name is used here instead
   of the DRC reference parameter name. */
void str_push_u16(Str *s, unsigned v, int big_endian);
void str_set_u16(Str *s, size_t at, unsigned v, int big_endian);

const char *str_cstr(Str *s);
const unsigned char *str_bytes(const Str *s);
size_t str_len(const Str *s);
void str_clear(Str *s);

/* ASCII-only case-insensitive compare. Bytes 0x80-0xFF are compared
   exactly. This matches DRB's vocabulary emission (drb.php:705-708 folds
   only bytes 32-127) and must NOT be used for DRF-style symbol
   normalisation, which is AnsiUpperCase and folds Latin-1 - see
   str_upper_latin1 below. */
int str_ieq(const char *a, const char *b);

/* ASCII-only uppercase copy, same reasoning: correct for DRB's
   vocabulary bytes, wrong for DRF's symbol table (str_upper_latin1
   below is the normaliser DRF-style code needs). */
char *str_upper_ascii(Arena *a, const char *s);

/* PORT: FPC AnsiUpperCase as DRF exercises it (USymbolList.pas
   AddSymbol/GetSymbolValue, UVocabularyTree.pas AddVocabulary): ASCII
   a-z folds to A-Z, and the whole lowercase-accented Latin-1/cp1252
   byte block 0xE0-0xFF folds - not just the eight bytes the lexer's
   identifier class gates onto DSF source (DSF.l:53-54). AnsiUpperCase
   has a second caller-reachable path that bypasses the lexer entirely:
   drf.pas:292-300's CLI "additional symbols" argument
   (`AddSymbol(SymbolList, AuxString, i)`), so the fold is reachable
   for every byte via that path.

   Pinned byte-for-byte against the reference drf.exe (D:/DRC/src,
   branch nextdaad), 2026-08-27, bytes 0xE0-0xFF swept: the fold is
   cp1252, not ISO-8859-1 - 0xF7 has no case (identity), 0xFF folds to
   0x9F (cp1252's only single-byte Y-diaeresis slot), every other byte
   in range folds by a flat -0x20 shift. Full mapping table: str.c
   (latin1_upper). */
char *str_upper_latin1(Arena *a, const char *s);

/* The arena all of this string's memory comes from. */
Arena *str_arena(const Str *s);

/* dst adopts src's buffer, length and capacity. Replaces what was
   struct assignment before Str became opaque, with identical
   semantics: no copy is made, and src must not be used afterwards.
   Under AddressSanitizer dst's previous buffer is poisoned, like a growth buffer's. */
void str_assign(Str *dst, const Str *src);

#endif /* NDRC_STR_H */
