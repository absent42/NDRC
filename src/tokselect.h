/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/tokselect.h - per-game compression token selection (-auto-tokens).
   Copyright (C) 2026 Dan Gibson. */
#ifndef NDRC_TOKSELECT_H
#define NDRC_TOKSELECT_H

#include "arena.h"
#include "diag.h"
#include "model.h"
#include "tokens.h"

/* Selects up to 128 tokens from the adventure's own compressable text
   (locations, messages, sysmess, xmessages - the "advanced" set).
   Returns a TokenSet shaped like tokens_load_builtin's: entry 0 is the
   single byte 0x00, live tokens follow ordered longest-first (the
   sequential compressor applies tokens in table order; longest-first
   is what makes an overlap-rich selected table win under it).
   Greedy selection with lazy re-evaluation, gains scored by optimal
   parse. Deterministic: same input, same table, every platform.
   exclude_placeholders skips candidates containing '_' or '@'. */
TokenSet *tokselect_run(Arena *a, Diag *d, const Adventure *adv,
                        int exclude_placeholders);

/* Deep-copies the compressable tables' Text strings, in table order
   (locations, messages, sysmess, xmessages), for the post-compression
   self-check. */
Vec_MsgTable *tokselect_snapshot(Arena *a, const Adventure *adv);

/* Expands the adventure's (already compressed) compressable tables
   through final_tokens and compares with the snapshot. 1 = identical,
   0 = any mismatch. Reference bytes are 128..255, indexing
   final_tokens at byte-127, matching tokens_compress' pass-2
   delimiters. */
int tokselect_verify(const Vec_MsgTable *before, const Adventure *adv,
                     const Vec_Str *final_tokens);

/* Test seam: total optimal-parse cost of strings under tokens
   (literal byte 1, any token reference 1). tokens NULL = no tokens. */
long tokselect_parse_total(Arena *a, const Vec_Str *strings,
                           const Vec_Str *tokens);

#endif /* NDRC_TOKSELECT_H */
