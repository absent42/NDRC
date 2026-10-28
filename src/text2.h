/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/text2.h - NextDAAD text format 2: Huffman-costed selection,
   one-level entry nesting, text block serialisation.
   Copyright (C) 2026 Dan Gibson. */
#ifndef NDRC_TEXT2_H
#define NDRC_TEXT2_H

#include "arena.h"
#include "diag.h"
#include "huff.h"
#include "str.h"

typedef struct {
    int alphabet383;            /* flag bit 0 */
    Vec_Str *tokens;            /* ntok entries, raw bytes (pre-nesting) */
    Vec_Str *entries;           /* ntok entries as emitted (length-prefixed body, may hold refs) */
    unsigned char lengths[HUFF_MAXSYM]; unsigned nsym;   /* 256 or 511 */
    Vec_Str *coded;             /* one coded stream per corpus string, in corpus order */
    long text_bytes, block_bytes;
    long unique_bytes;          /* text_bytes less shared duplicate streams */
} Text2Result;

/* Runs the two-round costed selection for maxtok 128 and 383, codes every
   string, nests entries, and returns the smaller of the two by
   unique_bytes + block_bytes. group[i] >= 0 lets string i share an equal
   earlier stream of the same group (emit_messages' share); NULL or a
   negative group never shares. Deterministic. ntok may be 0 (no usable
   repeats; the backend falls back to format 1). Returns NULL after a
   fatal diagnostic when a text byte is 128 or above. */
Text2Result *text2_build(Arena *a, Diag *d, const Vec_Str *strings,
                         const int *group);

/* Serialises the text block (magic, flags, ntok, fingerprint, lengths, index, entries). */
Str *text2_block(Arena *a, const Text2Result *r);

/* Test seam: nests raw tokens one level and renumbers them (referenced
   tokens below 128, most-used first). uses = text references per token;
   perm[new] = old index. Returns the entries in the new order. */
Vec_Str *text2_nest(Arena *a, const Vec_Str *tokens, const long *uses,
                    size_t *perm);

#endif /* NDRC_TEXT2_H */
