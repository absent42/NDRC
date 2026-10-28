/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/tokselect.h - per-game compression token selection (-auto-tokens).
   Copyright (C) 2026 Dan Gibson. */
#ifndef NDRC_TOKSELECT_H
#define NDRC_TOKSELECT_H

#include "arena.h"
#include "diag.h"
#include "model.h"
#include "tokens.h"

/* Compressable table slots: locations, messages, sysmess, xmessages,
   objects. The objects slot is live only when with_objects is set. */
#define TOKSEL_TABLES 5

/* Selects up to 128 tokens from the adventure's own compressable text
   (locations, messages, sysmess, xmessages - the "advanced" set - plus
   objects when with_objects).
   Returns a TokenSet shaped like tokens_load_builtin's: entry 0 is the
   single byte 0x00, live tokens follow ordered longest-first (the
   sequential compressor applies tokens in table order; longest-first
   is what makes an overlap-rich selected table win under it).
   Greedy selection with lazy re-evaluation, gains scored by optimal
   parse. Deterministic: same input, same table, every platform.
   exclude_placeholders skips candidates containing '_' or '@'.
   classic_objects applies the classic article-strip rule to object
   texts (see tokselect_protected_prefix): no candidate from the
   protected prefix, none containing '.'. */
TokenSet *tokselect_run(Arena *a, Diag *d, const Adventure *adv,
                        int exclude_placeholders, int with_objects,
                        int classic_objects);

/* The compressable tables' Text strings in slot order (locations,
   messages, sysmess, xmessages, then objects when with_objects). */
Vec_Str *tokselect_corpus_strings(Arena *a, const Adventure *adv,
                                  int with_objects);

/* tokselect_run's greedy under integer costs: literal byte b costs
   lit_cost[b], any reference ref_cost, a token tab_cost_per_byte per
   byte; up to maxtok tokens of 2..maxlen bytes; no placeholder rule.
   strings[obj_from..] get the classic object rule (pass the length for
   none). Returns raw tokens, longest-first. Deterministic. */
Vec_Str *tokselect_select_costed(Arena *a, Diag *d, const Vec_Str *strings,
                                 size_t obj_from, unsigned maxtok,
                                 const int lit_cost[256], int ref_cost,
                                 int tab_cost_per_byte, int maxlen);

/* Leading bytes of an object text no token may cover under the classic
   rule: n when s has no space, else the index past the run of spaces
   after the first word. Exposed as a test seam. */
size_t tokselect_protected_prefix(const unsigned char *s, size_t n);

/* Deep-copies the compressable tables' Text strings, TOKSEL_TABLES
   tables in slot order, for the post-compression self-check. The
   objects slot is an empty table when with_objects is 0. */
Vec_MsgTable *tokselect_snapshot(Arena *a, const Adventure *adv,
                                 int with_objects);

/* Expands the adventure's (already compressed) compressable tables
   through final_tokens and compares with the snapshot. 1 = identical,
   0 = any mismatch. Reference bytes are 128..255, indexing
   final_tokens at byte-127, matching tokens_compress' pass-2
   delimiters. */
int tokselect_verify(const Vec_MsgTable *before, const Adventure *adv,
                     const Vec_Str *final_tokens, int with_objects);

typedef struct {
    int snap_index;      /* slot in the snapshot: 0 loc, 1 msg, 2 sys, 4 obj */
    long lookup_addr;    /* absolute address of the lookup table */
    size_t count;        /* lookup words to follow */
    size_t checked;      /* first `checked` entries are DDB-resident; the rest were dumped to the XMB and are skipped */
    const char *name;    /* "objects" etc, for the fatal message */
} DdbTableRef;

/* Decodes each DDB-resident entry through its lookup word and the
   emitted token table and compares with the snapshot. Returns 1 on
   success; 0 with *bad_ref and *bad_index set on the first mismatch or
   out-of-range address. *skipped = entries not checked. big_endian
   is the target's lookup word order (ST, AMIGA). ddb[0] bit 7 set:
   format 2, the text block at tokens_addr is parsed and each text
   Huffman-decoded; a malformed block gives *bad_ref = -1. */
int tokselect_verify_ddb(const Vec_MsgTable *before, const unsigned char *ddb,
                         size_t ddb_len, long base_addr, long tokens_addr,
                         int big_endian, const DdbTableRef refs[4],
                         int *bad_ref, size_t *bad_index, size_t *skipped);

/* Format 2 XMB image xmb (file 0 at offset 0): checks the "X2" +
   fingerprint header against the DDB's text block, then decodes each
   snapshot xmessage at offsets[i]. 0 with *bad_index = i on a
   mismatch, -1 on a block or header fault. */
int tokselect_verify_xmb2(const Vec_MsgTable *before, const unsigned char *ddb,
                          size_t ddb_len, long base_addr, long tokens_addr,
                          const unsigned char *xmb, size_t xmb_len,
                          const long *offsets, long *bad_index);

/* Test seam: total optimal-parse cost of strings under tokens
   (literal byte 1, any token reference 1). tokens NULL = no tokens. */
long tokselect_parse_total(Arena *a, const Vec_Str *strings,
                           const Vec_Str *tokens);

/* Optimal-path counterpart of tokens_compress: prunes ts->tokens under
   the optimal-parse metric (2 bounded sweeps, last-to-first), then
   DP-encodes every compressable message in place. References are byte
   j+127 where j indexes the returned final_tokens (entry 0 included) -
   the byte contract tokselect_verify decodes. classic_mode pads the
   table to 128 entries with literal spaces, like the sequential path.
   classic_objects: object texts keep the protected prefix literal and
   use no token containing '.', in pruning and encoding alike.
   *savings = original minus encoded text bytes. Returns NULL after
   diag_fatal when more than 128 live tokens survive pruning (the
   reference space is 128..255); only reachable via a hand-written
   marked .tok - tokselect_run enforces the cap itself. */
Vec_Str *tokselect_compress(Arena *a, Diag *d, const Adventure *adv,
                            TokenSet *ts, int classic_mode, int with_objects,
                            int classic_objects, long *savings);

#endif /* NDRC_TOKSELECT_H */
