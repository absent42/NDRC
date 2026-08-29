/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/tokens.c - builtin compression token tables and two-pass compressor.
   Copyright (C) 2026 Dan Gibson.

   Ported from drb.php's tokens section: the five $compressionJSON_XX
   literals (drb.php:135 ES, 136 PT, 137 EN, 138 DE, 139 FR - each copied
   verbatim into its own tokens_XX.h), the builtin-table selection switch
   (drb.php:1895-1901), generateTokens (drb.php:144-241), hex2str and
   getCompressableTables (drb.php:300-319), and the inline .tok override
   lookup drb.php performs in MAIN (drb.php:1749-1756, 1887-1916).

   Classic-pad filler substitution: see tokens_compress below. */
#include "tokens.h"
#include "tokens_de.h"
#include "tokens_en.h"
#include "tokens_es.h"
#include "tokens_fr.h"
#include "tokens_pt.h"

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================
   PORT: hex2str, drb.php:300-307.

   PHP's loop bound is `$i < strlen($hex)-1`, not `$i < strlen($hex)`:
   with an odd-length hex string the final unpaired digit is silently
   dropped rather than being treated as an error. Reproduced here via
   the equivalent (and unsigned-safe) `i + 1 < hex_len` - see the two
   worked cases in the header comment. */
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    /* PORT NOTE: PHP's hexdec() strips non-hex characters out of its
       argument before parsing the remainder as hex, which can shift
       nibble alignment in ways a direct pairwise decode cannot
       reproduce without a full hexdec port. Every token hex string in
       the builtin table and every known .tok file is well-formed, so
       this divergence (treat a stray non-hex digit as nibble 0) is not
       expected to be exercised; a genuinely malformed .tok file is out
       of this port's fidelity scope. */
    return 0;
}

Str *tokens_hex2str(Arena *a, const char *hex, size_t hex_len)
{
    Str *out = str_new(a);
    size_t i;

    for (i = 0; i + 1 < hex_len; i += 2) {
        int hi = hex_nibble(hex[i]);
        int lo = hex_nibble(hex[i + 1]);
        str_push(out, (char)((hi << 4) | lo));
    }
    return out;
}

/* ===================================================================
   PORT: getCompressableTables, drb.php:310-319. */
static Vec_MsgTable *get_compressable_tables(Arena *a, const TokenSet *ts, const Adventure *adv)
{
    Vec_MsgTable *out = vec_new_MsgTable(a);

    if (ts->compression != NULL && strcmp(ts->compression, "basic") == 0) {
        vec_push_MsgTable(out, adv->locations);
    } else if (ts->compression != NULL && strcmp(ts->compression, "advanced") == 0) {
        vec_push_MsgTable(out, adv->locations);
        vec_push_MsgTable(out, adv->messages);
        vec_push_MsgTable(out, adv->sysmess);
        vec_push_MsgTable(out, adv->xmessages);
    }
    /* PORT NOTE: drb.php:313's switch has no default case - any other
       compression value (a hand-edited .tok naming something besides
       "basic"/"advanced", or "none" which never reaches here because
       tokens_compress short-circuits on !has_tokens first) yields the
       same empty table list PHP's $compressableTables would be left
       with, reproduced here by simply pushing nothing. */
    return out;
}

/* ===================================================================
   Byte-level explode()/implode(), matching PHP's non-overlapping,
   leftmost-first substring split (drb.php:170,176,217-218). */

static Vec_Str *split_bytes(Arena *a, const unsigned char *data, size_t len,
                             const unsigned char *needle, size_t nlen)
{
    Vec_Str *parts = vec_new_Str(a);
    size_t start = 0, i = 0;

    /* PORT NOTE: PHP's explode() fatals on an empty search string
       (drb.php:170,217); this port treats an empty needle as never
       matching, reachable only via a malformed .tok. */
    if (nlen == 0) {
        Str *whole = str_new(a);
        str_append_n(whole, data, len);
        vec_push_Str(parts, whole);
        return parts;
    }
    while (i + nlen <= len) {
        if (memcmp(data + i, needle, nlen) == 0) {
            Str *part = str_new(a);
            str_append_n(part, data + start, i - start);
            vec_push_Str(parts, part);
            i += nlen;
            start = i;
        } else {
            i++;
        }
    }
    {
        Str *last = str_new(a);
        str_append_n(last, data + start, len - start);
        vec_push_Str(parts, last);
    }
    return parts;
}

static Str *join_bytes(Arena *a, const Vec_Str *parts, unsigned char delim)
{
    Str *out = str_new(a);
    size_t i;

    for (i = 0; i < vec_len_Str(parts); i++) {
        Str *p = vec_at_Str(parts, i);
        if (i > 0) str_push(out, (char)delim);
        str_append_n(out, str_bytes(p), str_len(p));
    }
    return out;
}

/* ===================================================================
   Shared JSON-object-to-TokenSet loader, used by both
   tokens_load_builtin and tokens_load_override (drb.php:1905-1916:
   json_decode then the hex2str loop over ->tokens, shared by both the
   builtin and the .tok-override paths in the PHP). */
static TokenSet *tokens_load_from_json_bytes(Arena *a, Diag *d,
                                              const unsigned char *data, size_t len)
{
    JsonResult r = json_parse(a, data, len);
    JsonValue *comp_v, *tokens_v;
    TokenSet *ts;
    size_t i;

    /* PORT: drb.php:1908 `if (!$compressionData) Error('Invalid tokens
       file');` - text copied verbatim. PHP's json_decode returns null
       (falsy) on a parse failure; a structurally wrong-but-parseable
       document (missing "compression"/"tokens", or "tokens" not an
       array) is treated the same way here, since either case leaves
       DRB unable to read $compressionData->tokens. */
    if (!r.ok || r.root == NULL) {
        diag_fatal(d, "Invalid tokens file");
        return NULL;
    }
    /* PORT NOTE: stricter than the PHP (drb.php:1908 only checks
       `!$compressionData`) - only reachable via a malformed
       hand-edited .tok file; the builtin table is always well-typed. */
    comp_v = json_get(r.root, "compression");
    tokens_v = json_get(r.root, "tokens");
    if (comp_v == NULL || comp_v->type != JSON_STRING ||
        tokens_v == NULL || tokens_v->type != JSON_ARRAY) {
        diag_fatal(d, "Invalid tokens file");
        return NULL;
    }

    ts = arena_calloc(a, sizeof(*ts));
    ts->compression = arena_strndup(a, comp_v->str, comp_v->str_len);
    ts->advanced = (strcmp(ts->compression, "advanced") == 0);
    ts->has_tokens = (strcmp(ts->compression, "none") != 0);   /* drb.php:1909 */
    ts->tokens = vec_new_Str(a);

    for (i = 0; i < vec_len_JsonValue(tokens_v->items); i++) {
        JsonValue *tok = vec_at_JsonValue(tokens_v->items, i);
        if (tok == NULL || tok->type != JSON_STRING) {
            diag_fatal(d, "Invalid tokens file");
            return NULL;
        }
        vec_push_Str(ts->tokens, tokens_hex2str(a, tok->str, tok->str_len));
    }
    return ts;
}

/* PORT: the builtin-table selection switch, drb.php:1895-1901. Only
   "EN"/"PT"/"DE"/"FR" match explicitly; every other value, including
   "ES" itself, falls through to the default arm and gets
   $compressionJSON_ES - reproduced literally below, PORT NOTE per
   tokens.h. main.c has already rejected any language string outside
   the five DRC supports before this is ever called. */
TokenSet *tokens_load_builtin(Arena *a, Diag *d, const char *language)
{
    const unsigned char *json;
    size_t len;

    if (strcmp(language, "EN") == 0) {
        json = (const unsigned char *)NDRC_COMPRESSION_JSON_EN;
        len = strlen(NDRC_COMPRESSION_JSON_EN);
    } else if (strcmp(language, "PT") == 0) {
        json = (const unsigned char *)NDRC_COMPRESSION_JSON_PT;
        len = strlen(NDRC_COMPRESSION_JSON_PT);
    } else if (strcmp(language, "DE") == 0) {
        json = (const unsigned char *)NDRC_COMPRESSION_JSON_DE;
        len = strlen(NDRC_COMPRESSION_JSON_DE);
    } else if (strcmp(language, "FR") == 0) {
        json = (const unsigned char *)NDRC_COMPRESSION_JSON_FR;
        len = strlen(NDRC_COMPRESSION_JSON_FR);
    } else {
        json = (const unsigned char *)NDRC_COMPRESSION_JSON_ES;   /* default arm, drb.php:1901 */
        len = strlen(NDRC_COMPRESSION_JSON_ES);
    }
    return tokens_load_from_json_bytes(a, d, json, len);
}

/* ===================================================================
   .tok override lookup: drb.php:1749-1756 (path resolution) plus the
   drb.php:1887-1891 consumption (loading whichever path was found). */

static int file_exists_c(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

static int read_whole_file(Arena *a, const char *path,
                            const unsigned char **out_data, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long size;
    unsigned char *buf;
    size_t got;

    if (f == NULL) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    size = ftell(f);
    if (size < 0) { fclose(f); return 0; }
    rewind(f);
    buf = arena_alloc(a, (size_t)size);
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) return 0;
    *out_data = buf;
    *out_len = (size_t)size;
    return 1;
}

/* PORT: replace_extension, drb.php:280-286, matching main.c's own
   private copy of the same function byte-for-byte (see its PORT NOTE
   for the pathinfo()/dirname() derivation this reproduces).

   Output is user-visible (the resolved path is printed), so the "./"
   dirname prefix must match DRB's $tokensFilename exactly -
   ".\g.tok", not "g.tok". */
#ifdef _WIN32
#define NDRC_TOK_DIRSEP "\\"
#else
#define NDRC_TOK_DIRSEP "/"
#endif

static char *replace_extension(Arena *a, const char *filename, const char *new_ext)
{
    size_t len = strlen(filename);
    size_t i;
    long dot = -1, sep = -1;
    Str *out;

    for (i = 0; i < len; i++) {
        if (filename[i] == '.') dot = (long)i;
        if (filename[i] == '/' || filename[i] == '\\') sep = (long)i;
    }
    if (dot <= sep) dot = -1;   /* a dot in the dirname is not an extension */

    out = str_new(a);
    if (sep >= 0) str_append_n(out, filename, (size_t)sep);   /* dirname(), sans separator */
    else str_push(out, '.');                                  /* pathinfo() dirname for a bare filename */
    str_append(out, NDRC_TOK_DIRSEP);
    if (dot >= 0) str_append_n(out, filename + sep + 1, (size_t)(dot - sep - 1));
    else str_append(out, filename + sep + 1);
    str_push(out, '.');
    str_append(out, new_ext);
    return (char *)str_cstr(out);
}

/* ASCII-only whole-path lowercase, matching PHP's strtolower() as used
   on these filesystem paths (drb.php:1751,1754 - the brief's "fragility
   noted": strtolower runs over the WHOLE path, directory included). */
static char *lower_path(Arena *a, const char *s)
{
    size_t len = strlen(s);
    char *out = arena_alloc(a, len + 1);
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        out[i] = (char)c;
    }
    out[len] = '\0';
    return out;
}

/* PORT: the .tok lookup order, drb.php:1749-1756 read together with its
   consumption at drb.php:1887 (`if (file_exists($tokensFilename))`).
   Ported as a single function rather than mirroring the PHP's two-step
   "resolve $tokensFilename, then separately check it" shape, since C
   has no implicit global for the in-flight variable - the effect is
   identical: whichever path the PHP would have ended up checking at
   line 1887 is exactly the path returned here (or NULL if that final
   check would have failed). */
static const char *find_tok_override(Arena *a, const char *input_path)
{
    char *p1 = replace_extension(a, input_path, "tok");
    char *lp1;
    char *p2;
    char *lp2;

    if (file_exists_c(p1)) return p1;

    lp1 = lower_path(a, p1);
    if (file_exists_c(lp1)) return lp1;

    p2 = replace_extension(a, input_path, "TOK");
    lp2 = lower_path(a, p2);
    /* PORT NOTE: lp2 is always byte-identical to lp1 (p1 and p2 differ
       only in the extension's case, and lower_path erases that), so
       this check can never newly succeed after lp1 already failed
       above. Ported anyway, literally, per drb.php:1754 - it is
       harmless dead code there too. */
    if (file_exists_c(lp2)) return lp2;

    /* drb.php leaves $tokensFilename set to p2 (original case, "TOK"
       extension) when every check above fails, and line 1887's
       file_exists($tokensFilename) is what actually decides whether a
       tokens file is used - on a case-insensitive filesystem this can
       still find a real "*.tok"/"*.TOK" file beside the input even
       though none of the explicit checks above matched it. */
    if (file_exists_c(p2)) return p2;

    return NULL;
}

const char *tokens_probe_override(Arena *a, const char *json_input_path)
{
    return find_tok_override(a, json_input_path);
}

int tokens_write_tok(const char *path, const TokenSet *ts)
{
    FILE *f = fopen(path, "wb");
    size_t i, j;

    if (f == NULL) return 0;
    fputs("{\"compression\": \"advanced\", \"tokens\": [", f);
    for (i = 0; i < vec_len_Str(ts->tokens); i++) {
        Str *t = vec_at_Str(ts->tokens, i);
        const unsigned char *b = str_bytes(t);
        if (i > 0) fputc(',', f);
        fputc('"', f);
        for (j = 0; j < str_len(t); j++) fprintf(f, "%02x", b[j]);
        fputc('"', f);
    }
    fputs("]}", f);
    return fclose(f) == 0;
}

TokenSet *tokens_load_override(Arena *a, Diag *d, const char *json_input_path,
                                const char **out_resolved_path)
{
    const char *tok_path = find_tok_override(a, json_input_path);
    const unsigned char *data;
    size_t len;

    if (tok_path == NULL) return NULL;
    if (out_resolved_path != NULL) *out_resolved_path = tok_path;
    if (!read_whole_file(a, tok_path, &data, &len)) return NULL;
    return tokens_load_from_json_bytes(a, d, data, len);
}

/* ===================================================================
   PORT: generateTokens, drb.php:144-241, including the classic-mode
   pad (drb.php:202-206) - see file header PORT NOTE. */

Vec_Str *tokens_compress(Arena *a, Diag *d, const Adventure *adv, TokenSet *ts,
                          int classic_mode, long *savings)
{
    Vec_Str *final_tokens = vec_new_Str(a);
    Vec_MsgTable *compressable;
    Vec_Str *string_list;
    long *token_savings;
    int *has_saving;
    size_t num_tokens, num_strings, j, i;
    long total_saving = 0;

    *savings = 0;
    if (!ts->has_tokens) return final_tokens;   /* drb.php:146-150 */

    compressable = get_compressable_tables(a, ts, adv);

    /* *** FIRST PASS: drb.php:157-178 - working copies of every
       compressable table's text, so pass one's progressive replacement
       never touches the real Adventure tables. */
    string_list = vec_new_Str(a);
    for (i = 0; i < vec_len_MsgTable(compressable); i++) {
        Vec_Message *table = vec_at_MsgTable(compressable, i);
        size_t k;
        for (k = 0; k < vec_len_Message(table); k++) {
            Message *m = vec_at_Message(table, k);
            Str *copy = str_new(a);
            str_append_n(copy, str_bytes(m->Text), str_len(m->Text));
            vec_push_Str(string_list, copy);
        }
    }
    num_strings = vec_len_Str(string_list);

    num_tokens = vec_len_Str(ts->tokens);
    token_savings = arena_calloc(a, num_tokens * sizeof(*token_savings));
    has_saving = arena_calloc(a, num_tokens * sizeof(*has_saving));

    for (j = 0; j < num_tokens; j++) {
        Str *token = vec_at_Str(ts->tokens, j);
        size_t tlen = str_len(token);
        for (i = 0; i < num_strings; i++) {
            Str *s = vec_at_Str(string_list, i);
            Vec_Str *parts = split_bytes(a, str_bytes(s), str_len(s), str_bytes(token), tlen);
            size_t nparts = vec_len_Str(parts);

            if (nparts > 1) {
                size_t k;
                /* Once per token replacement (nparts-1 occurrences):
                   the FIRST ever replacement of token j sets its
                   saving to -1 (wastes a byte, drb.php:174's comment);
                   every later one adds tlen-1. */
                for (k = 0; k + 1 < nparts; k++) {
                    if (has_saving[j]) token_savings[j] += (long)tlen - 1;
                    else { token_savings[j] = -1; has_saving[j] = 1; }
                }
            }
            vec_set_Str(string_list, i, join_bytes(a, parts, (unsigned char)(j + 127)));
        }
    }

    /* Remove tokens which aren't worth using: drb.php:180-197. */
    /* PORT NOTE: the `num_tokens > 0` guard has no PHP counterpart -
       drb.php:182 indexes $compressionData->tokens[0] unconditionally,
       which for an empty "tokens": [] in a hand-edited .tok produces an
       undefined-array-key notice and a null first entry rather than a
       clean skip. This port simply omits token 0 when there are no
       tokens to omit it from. Only reachable through a malformed .tok -
       the builtin table's tokens array is never empty. */
    if (num_tokens > 0) vec_push_Str(final_tokens, vec_at_Str(ts->tokens, 0));   /* never remove token 0 */
    for (j = 1; j < num_tokens; j++) {
        long sav = has_saving[j] ? token_savings[j] : 0;

        if (sav > 0) {
            vec_push_Str(final_tokens, vec_at_Str(ts->tokens, j));
            total_saving += sav;
        } else {
            Str *token = vec_at_Str(ts->tokens, j);
            if (sav == 0) {
                diag_verbose(d,
                    "Warning: token [%s] won't be used cause it was not used by any text.",
                    str_cstr(token));
            } else {
                diag_verbose(d,
                    "Warning: token [%s] won't be used cause using it wont save any bytes, but waste %ld byte.",
                    str_cstr(token), labs(sav));
            }
        }
    }
    *savings = total_saving;

    /* *** SECOND PASS: drb.php:199-221. */
    diag_verbose(d, "Compression tokens used: %zu.", vec_len_Str(final_tokens));

    /* PORT: drb.php:202-206. Runs strictly BEFORE the replacement loop
       below (pass two proper), so the padding tokens - all identical
       single spaces - are already in final_tokens by the time real
       text gets scanned for matches: the first filler to reach a given
       string claims every literal space in it, and every later filler
       finds nothing left. One Str is allocated for the space byte and
       reused across every pushed entry - safe since nothing ever
       mutates a token's Str in place, only reads it (str_len/
       str_bytes) to drive split_bytes/join_bytes. */
    if (classic_mode) {
        Str *space = str_new(a);
        str_push(space, ' ');
        while (vec_len_Str(final_tokens) < 128) vec_push_Str(final_tokens, space);
        diag_verbose(d, "Filling tokens table up to 128 tokens for classic mode compatibility.");
    }

    for (j = 0; j < vec_len_Str(final_tokens); j++) {
        Str *token = vec_at_Str(final_tokens, j);
        size_t tlen = str_len(token);
        for (i = 0; i < vec_len_MsgTable(compressable); i++) {
            Vec_Message *table = vec_at_MsgTable(compressable, i);
            size_t k;
            for (k = 0; k < vec_len_Message(table); k++) {
                Message *m = vec_at_Message(table, k);
                Vec_Str *parts = split_bytes(a, str_bytes(m->Text), str_len(m->Text),
                                              str_bytes(token), tlen);
                m->Text = join_bytes(a, parts, (unsigned char)(j + 127));
            }
        }
    }

    return final_tokens;
}

/* ===================================================================
   PORT: the token dump, drb.php:224-236 (and drb.php:148-150's
   writeZero for the no-tokens case, folded into this shared emitter -
   see tokens.h). */
void tokens_emit(Str *out, const Vec_Str *final_tokens, long *current_address)
{
    size_t n = vec_len_Str(final_tokens);
    size_t j;

    /* PORT NOTE: this n==0 check folds two PHP paths together:
       has_tokens==false (drb.php:148-150's writeZero, one zero byte)
       and has_tokens==true with an empty token table (drb.php:224-236
       emits nothing). Both land here as n==0, so a has_tokens==true,
       zero-token .tok emits one byte where DRB emits none - only
       reachable with a malformed .tok. */
    if (n == 0) {
        str_push_u8(out, 0);
        (*current_address)++;
        return;
    }
    for (j = 0; j < n; j++) {
        Str *token = vec_at_Str(final_tokens, j);
        size_t tlen = str_len(token);
        const unsigned char *bytes = str_bytes(token);
        size_t i;
        for (i = 0; i < tlen; i++) {
            unsigned v = bytes[i];
            if (i == tlen - 1) v += 128;
            str_push_u8(out, v);
            (*current_address)++;
        }
    }
}
