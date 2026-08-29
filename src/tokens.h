/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/tokens.h - EN compression token table and two-pass compressor.
   Copyright (C) 2026 Dan Gibson.

   Ported from drb.php's tokens section (drb.php:131-241, 300-319) plus
   the .tok override lookup drb.php performs inline in MAIN
   (drb.php:1749-1756, 1887-1916). See tokens.c for the PORT NOTEs
   marking every deviation from the PHP. */
#ifndef NDRC_TOKENS_H
#define NDRC_TOKENS_H

#include "arena.h"
#include "diag.h"
#include "model.h"
#include "str.h"
#include "vec.h"

typedef struct {
    int has_tokens;      /* compression != "none" (drb.php:1909) */
    Vec_Str *tokens;       /* hex2str-decoded raw token bytes, in
                              the JSON file's original order */
    int advanced;         /* 1 iff compression == "advanced" */
    const char *compression; /* raw compression string, drives
                              getCompressableTables (drb.php:310-319);
                              kept alongside `advanced` because a .tok
                              override can name "basic" or an
                              unrecognised value, each with different
                              (or empty) compressable-table behaviour -
                              a single bool cannot distinguish those. */
    int optimal_encode;  /* 1 = optimal-parse encoding: set by
                            tokselect_run, or by a .tok carrying
                            "encoder": "optimal" with compression
                            "advanced" - never otherwise. */
} TokenSet;

/* Parses the builtin table for `language` (tokens_en.h/es.h/de.h/pt.h/
   fr.h, each ported verbatim from its drb.php line - EN:137, ES:135,
   PT:136, DE:138, FR:139) via json.c and hex2str's every token. Selects
   among the five per drb.php:1895-1901's switch: "EN"/"PT"/"DE"/"FR"
   match explicitly, and the switch's default arm is ES - so any other
   string, including one the CLI never actually passes (main.c has
   already validated the language against the five supported strings
   before calling this), would also select ES. PORT NOTE: reproduced as
   the PHP's literal default-to-ES quirk, not specially guarded. NULL
   (with a diagnostic already reported via diag_fatal) only if the
   selected builtin literal somehow fails to parse - not expected in
   practice. */
TokenSet *tokens_load_builtin(Arena *a, Diag *d, const char *language);

/* Ports the .tok override lookup, drb.php:1749-1756 / 1887-1891 (see
   tokens.c for the candidate-order mechanism). Returns NULL if no
   file is found, or if found but unparseable (diag_fatal("Invalid
   tokens file"), drb.php:1908) - callers distinguish via
   diag_error_count. If out_resolved_path is non-NULL and a file WAS
   found, it is set to the matched candidate path, matching drb.php's
   $tokensFilename (drb.php:1887-1889). */
TokenSet *tokens_load_override(Arena *a, Diag *d, const char *json_input_path,
                                const char **out_resolved_path);

/* Two-pass compress (drb.php:144-241). Mutates adv in place with
   POST-pad indices; classic-mode padding contributes nothing to
   *savings; !has_tokens touches nothing. Mechanism detail:
   tokens.c (tokens_compress). */
Vec_Str *tokens_compress(Arena *a, Diag *d, const Adventure *adv, TokenSet *ts,
                          int classic_mode, long *savings);

/* PORT: the token dump, drb.php:224-236. Each surviving token's bytes
   are appended to out with 128 added to its LAST byte, advancing
   *current_address by one per byte written. An empty final_tokens
   (the !has_tokens path) writes a single 0x00 byte instead
   (drb.php:148-149's writeZero), advancing *current_address by one. */
void tokens_emit(Str *out, const Vec_Str *final_tokens, long *current_address);

/* PORT: hex2str, drb.php:300-307. Decodes each two-hex-digit pair to
   one output byte; a trailing unpaired hex digit is silently dropped,
   reproducing the PHP loop bound `$i < strlen($hex)-1` rather than
   `$i < strlen($hex)`. Exposed for direct testing per the brief. */
Str *tokens_hex2str(Arena *a, const char *hex, size_t hex_len);

/* The .tok candidate the override lookup WOULD use for this input
   (same resolution as tokens_load_override), or NULL. Lets the
   -auto-tokens path name the file it is bypassing. */
const char *tokens_probe_override(Arena *a, const char *json_input_path);

/* Writes ts as a .tok: {"compression": "advanced", "tokens":
   ["00","<hex>",...]} - two-digit lowercase hex per byte, tokens in
   vec order. 1 = written, 0 = I/O failure (caller reports). */
int tokens_write_tok(const char *path, const TokenSet *ts);

#endif /* NDRC_TOKENS_H */
