/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/backend.c - the DRB back end, driven by a parsed option set.
   Copyright (C) 2026 Dan Gibson.

   PORT: drb.php's MAIN body, drb.php:1707-2135 - argument checking, the
   header write and offset patch pass, and the two-phase (main-body +
   per-table generate*) emission driver, plus its stdout report. Split
   out of main.c's run_from_json verbatim, so that the DSF->DDB join
   drives the same stage the --from-json CLI does; the argv walk, the
   file read and the ndrc banner stay behind in the CLI layer (main.c),
   and BackendOptions carries what the walk produced. Every DRB
   `Error($msg)` is diag_fatal(d, msg) then `return diag_exit_code(d)` -
   see main.c's file header for that mapping and for the stdout stream
   redirect this stage relies on. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "backend.h"
#include "diag.h"
#include "json.h"
#include "layout.h"
#include "model.h"
#include "str.h"
#include "targets.h"
#include "text2.h"
#include "tokens.h"
#include "tokselect.h"
#include "vec.h"
#include "back/emit.h"
#include "back/emit_hdr.h"
#include "back/finish.h"
#include "back/srcmap.h"
#include "front/srcpos.h"

/* NDRC's own usage text (drb.php's Syntax() has no analogue for a
   wrapped --from-json subcommand - see the file header PORT NOTE).
   One copy, used at every argument-count bounds check below. */
const char BACKEND_USAGE_MSG[] =
    "usage: ndrc --from-json TARGET [SUBTARGET] LANG input.json [output.ddb] [options]";

/* PORT: drb.php:1928 - filesize('0.XMB') when the file already exists
   (task-4-brief.md's XMessages-then-append interplay: emit_xmessages may
   have just written 0.XMB when the adventure has XMessages, and the -X
   open below finds it and appends from this cursor), else 0. Mirrors
   read_whole_file's fseek/ftell pattern rather than a platform stat call,
   for the same portability reason. */
static long xmb_existing_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    if (f == NULL) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    size = ftell(f);
    fclose(f);
    return size < 0 ? 0 : size;
}

/* PORT: replace_extension, drb.php:280-286. PHP pathinfo dirname is
   '.' (truthy) for a bare name, so the falsy branch is dead PHP code -
   verified against live php.exe. tokens.c keeps a private copy of
   this helper (no shared header). */
#ifdef _WIN32
#define NDRC_DIRSEP "\\"
#else
#define NDRC_DIRSEP "/"
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
    str_append(out, NDRC_DIRSEP);
    if (dot >= 0) str_append_n(out, filename + sep + 1, (size_t)(dot - sep - 1));
    else str_append(out, filename + sep + 1);
    str_push(out, '.');
    str_append(out, new_ext);
    return (char *)str_cstr(out);
}

/* PORT: prettyFormat, drb.php:272-278 - strtoupper(dechex($value)),
   left-padded with '0' to at least 4 digits, "0x" prefixed. "%04lX"
   reproduces this: it zero-pads to (at least) 4 hex digits and, unlike
   str_pad, naturally never truncates a longer value either - matching
   str_pad's is-at-least-length semantics for the in-range addresses
   this phase ever prints (every offset here is well under 0x10000). */
static void fmt_addr(char *buf, size_t n, long value)
{
    snprintf(buf, n, "0x%04lX", (unsigned long)value);
}

static void print_map_line(int verbose, const char *label, long value)
{
    char buf[24];
    if (!verbose) return;
    fmt_addr(buf, sizeof buf, value);
    printf("%s[%s]\n", label, buf);
}

/* Not in DRC: reports one table's emit_messages sharing and folds the
   bytes it saved into the savings total. */
static void report_share(int verbose, const ShareStats *s, const char *table,
                         long *savings)
{
    if (verbose && s->texts > 0)
        printf("Shared %lu duplicate texts in %s (%ld bytes).\n",
               (unsigned long)s->texts, table, s->bytes);
    *savings += s->bytes;
}

/* Expands entry k one level into dec (byte 0 is its length). */
static void text2_expand(Str *dec, const Vec_Str *entries, size_t k, int depth)
{
    const Str *e = vec_at_Str(entries, k);
    size_t i;

    for (i = 1; i < str_len(e); i++) {
        unsigned b = str_bytes(e)[i];
        if (b >= 128 && depth == 0 && b - 128 < vec_len_Str(entries))
            text2_expand(dec, entries, b - 128, 1);
        else str_push_u8(dec, b >= 128 ? 0xFFu : b);  /* 0xFF never matches */
    }
}

/* Decodes coded stream c to its text; 0 on overrun, bad token or
   trailing bytes. Adds the bits and symbols read (EOM included). */
static int text2_decodes_to(Arena *a, const Text2Result *r, const Str *c,
                            const Str *want, long *bits, long *syms)
{
    Str *dec = str_new(a);
    size_t pos = 0;

    for (;;) {
        int sym = huff_decode_one(r->lengths, r->nsym, str_bytes(c),
                                  str_len(c), &pos);
        if (sym < 0) return 0;
        (*syms)++;
        if (sym == 10) break;
        if (sym < 128) str_push_u8(dec, (unsigned)sym);
        else if ((size_t)sym - 128 >= vec_len_Str(r->entries)) return 0;
        else text2_expand(dec, r->entries, (size_t)sym - 128, 0);
    }
    *bits += (long)pos;
    return (pos + 7) / 8 == str_len(c) && str_len(dec) == str_len(want) &&
           memcmp(str_bytes(dec), str_bytes(want), str_len(want)) == 0;
}

/* Text bytes + terminators; with share, equal texts count once
   (emit_messages' share within one table). */
static long table_bytes(const Vec_Message *m, int share)
{
    long n = 0;
    size_t i, j;
    for (i = 0; m != NULL && i < vec_len_Message(m); i++) {
        const Str *s = vec_at_Message(m, i)->Text;
        for (j = 0; share && j < i; j++) {
            const Str *p = vec_at_Message(m, j)->Text;
            if (str_len(p) == str_len(s) &&
                memcmp(str_bytes(p), str_bytes(s), str_len(s)) == 0)
                break;
        }
        if (!share || j == i) n += (long)str_len(s) + 1;
    }
    return n;
}

/* -nxtext: codes the snapshot `pre` (format 1 already compressed into
   adv) and keeps format 2 whenever it has a token (the loader refuses
   ntok 0); the index is the print-speed gain, so a larger total is
   only reported. Writes the XMB either way. On format 2 fills coded[]
   (tables in snapshot slot order), *block and *savings. -1 after a
   fatal. */
static int nxtext_select(Arena *a, Diag *d, const Target *target,
                         Adventure *adv, const Vec_MsgTable *pre,
                         const Vec_Str *final_tokens, int verbose,
                         Vec_Message *coded[TOKSEL_TABLES], Str **block,
                         long *savings)
{
    Vec_Str *corpus = vec_new_Str(a), *xcoded = vec_new_Str(a);
    Vec_Message *xraw = vec_at_MsgTable(pre, 3);
    Text2Result *r;
    long f1 = 0, f2, original = 0, bits = 0, syms = 0;
    size_t t, k, idx = 0, total = 0;
    int *group;

    for (t = 0; t < TOKSEL_TABLES; t++)
        total += vec_len_Message(vec_at_MsgTable(pre, t));
    group = arena_alloc(a, (total + 1) * sizeof(*group));
    for (t = 0; t < TOKSEL_TABLES; t++) {
        const Vec_Message *m = vec_at_MsgTable(pre, t);
        for (k = 0; k < vec_len_Message(m); k++, idx++) {
            vec_push_Str(corpus, vec_at_Message(m, k)->Text);
            /* Slot 3 (XMB) is never shared; -X is refused. */
            group[idx] = t == 3 ? -1 : (int)t;
        }
        original += table_bytes(m, 0);
    }
    idx = 0;
    r = text2_build(a, d, corpus, group);
    if (r == NULL) return -1;

    f1 = table_bytes(adv->locations, 1) + table_bytes(adv->messages, 1) +
         table_bytes(adv->sysmess, 1) + table_bytes(adv->objects, 1) +
         table_bytes(xraw, 0);
    for (k = 0; k < vec_len_Str(final_tokens); k++)
        f1 += (long)str_len(vec_at_Str(final_tokens, k));
    if (vec_len_Str(final_tokens) == 0) f1++;
    f2 = r->unique_bytes + r->block_bytes + (vec_len_Message(xraw) ? 4 : 0);

    if (vec_len_Str(r->entries) == 0) {
        printf("Text format 2 has no tokens; format 1 emitted.\n");
        /* tokselect_compress encoded adv's xmessages; the XMB is raw. */
        adv->xmessages = xraw;
        if (vec_len_Message(xraw) && !emit_xmessages(d, target, adv))
            return -1;
        return 0;
    }
    if (verbose && f2 >= f1)
        printf("Text format 2 is %ld bytes larger than format 1.\n", f2 - f1);

    for (t = 0; t < TOKSEL_TABLES; t++) {
        const Vec_Message *m = vec_at_MsgTable(pre, t);
        coded[t] = vec_new_Message(a);
        for (k = 0; k < vec_len_Message(m); k++, idx++) {
            Message *cm = arena_calloc(a, sizeof(*cm));
            cm->Text = vec_at_Str(r->coded, idx);
            if (!text2_decodes_to(a, r, cm->Text, vec_at_Message(m, k)->Text,
                                  &bits, &syms)) {
                diag_fatal(d, "Text format 2 self-check failed: coded text "
                              "%lu does not decode back to source",
                           (unsigned long)idx);
                return -1;
            }
            vec_push_Message(coded[t], cm);
            if (t == 3) vec_push_Str(xcoded, cm->Text);
        }
    }
    *block = text2_block(a, r);
    if (vec_len_Str(xcoded) &&
        !emit_xmessages2(d, target, adv, xcoded,
                         (unsigned)str_bytes(*block)[5] |
                         ((unsigned)str_bytes(*block)[6] << 8)))
        return -1;
    if (verbose)
        printf("Text format 2: %u tokens, %.2f bits per symbol, %ld text bytes.\n",
               (unsigned)vec_len_Str(r->entries),
               syms ? (double)bits / (double)syms : 0.0, r->text_bytes);
    *savings = original - r->text_bytes;
    return 1;
}

/* Rebuilds the format 2 XMB image from the streams emit_xmessages2
   wrote (headers at each file start, streams at their offsets) and
   decodes it through the emitted DDB's text block. 0 after a fatal. */
static int xmb2_check(Arena *a, Diag *d, const Adventure *adv,
                      const Vec_MsgTable *pre, const Vec_Message *xcoded,
                      const Str *block, const Str *out, long base_address,
                      long tokens_addr)
{
    long size = adv->xmessage_size, file = adv->xmessage_max_k * 1024, f;
    long bad = -1;
    unsigned char *img = arena_calloc(a, (size_t)size + 1);
    size_t k;

    for (f = 0; file > 0 && f + 4 <= size; f += file) {
        img[f] = 'X';
        img[f + 1] = '2';
        img[f + 2] = str_bytes(block)[5];
        img[f + 3] = str_bytes(block)[6];
    }
    for (k = 0; k < vec_len_Message(xcoded); k++) {
        const Str *c = vec_at_Message(xcoded, k)->Text;
        long o = adv->xmessage_offsets[k];
        if (o < 4 || o + (long)str_len(c) > size) { bad = (long)k; break; }
        memcpy(img + o, str_bytes(c), str_len(c));
    }
    if (k == vec_len_Message(xcoded) &&
        tokselect_verify_xmb2(pre, str_bytes(out), str_len(out), base_address,
                              tokens_addr, img, (size_t)size,
                              adv->xmessage_offsets, &bad))
        return 1;
    if (bad < 0)
        diag_fatal(d, "auto-tokens self-check failed: XMB header does not "
                      "match the emitted text block");
    else
        diag_fatal(d, "auto-tokens self-check failed: emitted xmessages "
                      "text %ld does not decode back to source", bad);
    return 0;
}

/* PORT: drb.php:804-812 checkMaluva / drb.php:814-817 MaluvaEmbedded,
   restated here for the drb.php:1922 call site (emit_proc.c keeps its
   own copies for its per-condact guards; not shared - both are file-
   local statics). S12.2: MaluvaEmbedded always returns true, so
   drb.php:1922's guard is permanently dead - these stubs reproduce the
   dead branch without porting the real per-extern MLV_ scan
   (drb.php:807-809). */
static int xmes_check_maluva(void) { return 0; }
static int xmes_maluva_embedded(void) { return 1; }

/* -srcmap: source file name to DSM file index, each name resolved once. */
typedef struct { const char *name; int id; } SrcFileId;

static int srcmap_file_id(SrcMap *m, const char *dsm_path, SrcFileId *cache,
                          int *n, const char *name)
{
    char *rel;
    int i, id;
    for (i = 0; i < *n; i++)
        if (strcmp(cache[i].name, name) == 0) return cache[i].id;
    rel = srcmap_rel_path(dsm_path, name);
    id = srcmap_file(m, rel);
    free(rel);
    if (*n < 64) {
        cache[*n].name = name;
        cache[*n].id = id;
        (*n)++;
    }
    return id;
}

/* The DSM for the written DDB, from the front end's srcpos table. shift
   turns a DDB address into a file offset. Returns 1 on success. */
static int write_srcmap(const char *path, const ProcAddrs *pa, long shift,
                        const uint8_t *ddb, size_t len)
{
    SrcMap *m = srcmap_new();
    SrcFileId cache[64];
    int ncache = 0, i, rc;
    size_t k;

    for (i = 0; i < srcpos_proc_count(); i++) {
        SrcPos p = srcpos_proc(i);
        if (p.file != NULL)
            srcmap_proc(m, i, srcmap_file_id(m, path, cache, &ncache, p.file), p.line);
    }
    for (k = 0; pa->entries != NULL && k < vec_len_ProcEntryAddr(pa->entries); k++) {
        const ProcEntryAddr *e = vec_at_ProcEntryAddr(pa->entries, k);
        SrcPos p = srcpos_entry((int)e->proc, (int)e->entry);
        if (p.file != NULL)
            srcmap_entry(m, (unsigned)(e->record + shift),
                         srcmap_file_id(m, path, cache, &ncache, p.file), p.line);
    }
    for (k = 0; pa->conds != NULL && k < vec_len_ProcCondAddr(pa->conds); k++) {
        const ProcCondAddr *c = vec_at_ProcCondAddr(pa->conds, k);
        SrcPos p;
        if (c->src < 0) continue;
        p = srcpos_cond((int)c->owner->proc, (int)c->owner->entry, c->src);
        if (p.file != NULL)
            srcmap_cond(m, (unsigned)(c->addr + shift), (unsigned)(c->owner->record + shift),
                        srcmap_file_id(m, path, cache, &ncache, p.file), p.line, p.col);
    }
    for (i = 0; i < srcpos_define_count(); i++)
        srcmap_sym(m, srcpos_define_name(i), srcpos_define_value(i));
    rc = srcmap_write(m, path, ddb, len);
    srcmap_free(m);
    return rc == 0;
}

int backend_run(Arena *a, Diag *d, const unsigned char *json_data,
                size_t json_len, const BackendOptions *opts)
{
    int classic;
    long base_address;
    FILE *xmb_fh;
    long xmb_addr;
    char *target_upper, *language, *subtarget_upper;
    const char *output_path = opts->output_path;
    const Target *target;
    int lang_bit;
    JsonResult jr;
    Adventure *adv;
    TokenSet *ts;
    Vec_MsgTable *pre_texts = NULL;
    Vec_Str *final_tokens = NULL;
    int compress_objects, share_texts;
    int text2 = 0;                       /* -nxtext chose format 2 */
    Vec_Message *t2_tabs[TOKSEL_TABLES] = { NULL };
    Str *t2_block = NULL;
    ShareStats otx_share = { 0, 0 }, stx_share = { 0, 0 };
    ShareStats mtx_share = { 0, 0 }, ltx_share = { 0, 0 };
    long addr;
    Str *out;
    long previous_address;
    long extern_size, otx_size, obj_size, objweight_size, objattr_size;
    long objinitially_size, voc_size, token_size, stx_size, mtx_size;
    long ltx_size, con_size, pro_size;
    long object_lookup_offset, object_names_offset, object_weight_attr_offset;
    long object_extra_attr_offset, initially_at_offset, vocabulary_offset;
    long compressed_text_offset, sysmess_lookup_offset, message_lookup_offset;
    long location_lookup_offset, connections_lookup_offset, process_list_offset;
    long text_savings = 0;
    long offsets[NDRC_HEADER_PATCH_WORDS];
    long ddb_size;
    FILE *fp;
    ProcAddrs proc_addrs = { NULL, NULL };
    size_t unprefixed_len;

    if (opts->target_arg == NULL) {
        diag_fatal(d, "%s", BACKEND_USAGE_MSG);
        return diag_exit_code(d);
    }
    /* PORT: drb.php:1714-1715. */
    target_upper = str_upper_ascii(a, opts->target_arg);
    if (!target_name_valid(target_upper)) {
        diag_fatal(d, "Invalid target machine '%s'", target_upper);
        return diag_exit_code(d);
    }

    /* PORT: drb.php:1717-1723 - a subtarget is consumed and validated
       only for the four targets isValidSubtarget requires one of
       (target_takes_subtarget - targets.h/targets.c, derived from the
       row table rather than a second hand-written list).
       isValidSubtarget's stray debug echo (drb.php:1240) prints from
       INSIDE the validation call, before the invalid-subtarget Error
       and before any Target line - reproduced here by printing it
       ahead of the target_lookup call standing in for
       isValidSubtarget. */
    subtarget_upper = NULL;
    if (target_takes_subtarget(target_upper)) {
        if (opts->subtarget_arg == NULL) {
            diag_fatal(d, "%s", BACKEND_USAGE_MSG);
            return diag_exit_code(d);
        }
        subtarget_upper = str_upper_ascii(a, opts->subtarget_arg);
        printf("Debug: Checking subtarget %s for target %s\n", subtarget_upper, target_upper);
        target = target_lookup(target_upper, subtarget_upper);
        if (target == NULL) {
            diag_fatal(d, "Invalid subtarget '%s' for target '%s'", subtarget_upper, target_upper);
            return diag_exit_code(d);
        }
    } else {
        /* target_name_valid already gated target_upper above: every
           bare (non-subtarget) target has exactly one row with
           subtarget NULL, so this lookup cannot fail - no defensive
           NULL check needed here (unlike the subtarget branch above,
           where an out-of-range subtarget is a real, user-reachable
           case). */
        target = target_lookup(target_upper, NULL);
    }

    /* PORT: drb.php:1724-1726. */
    printf("Target: %s", target->name);
    if (subtarget_upper != NULL) printf(" (%s)", target->subtarget);
    printf("\n");

    /* PORT: drb.php:1727-1728. */
    if (opts->lang_arg == NULL) {
        diag_fatal(d, "%s", BACKEND_USAGE_MSG);
        return diag_exit_code(d);
    }
    language = str_upper_ascii(a, opts->lang_arg);
    if (strcmp(language, "EN") == 0 || strcmp(language, "ES") == 0 ||
        strcmp(language, "DE") == 0 || strcmp(language, "PT") == 0 ||
        strcmp(language, "FR") == 0) {
        /* PORT: drb.php:1845 - "Set spanish language  (DE and EN keep
           English)" (double space verbatim). ES and PT set the bit; EN,
           DE and FR leave it clear. */
        lang_bit = (strcmp(language, "ES") == 0 || strcmp(language, "PT") == 0) ? 1 : 0;
    } else {
        diag_fatal(d, "Invalid target language");
        return diag_exit_code(d);
    }

    /* PORT: drb.php:1729-1730. */
    if (opts->input_name == NULL) {
        diag_fatal(d, "%s", BACKEND_USAGE_MSG);
        return diag_exit_code(d);
    }

    /* PORT: drb.php:1731-1747. A NULL json_data is the caller's failed
       file_exists/file_get_contents (both are drb's own "File not
       found"), reported here so it keeps its place after the Target
       line. */
    if (json_data == NULL) {
        diag_fatal(d, "File not found");
        return diag_exit_code(d);
    }
    jr = json_parse(a, json_data, json_len);
    if (!jr.ok) {
        /* PORT: drb.php:1735/1741 - 'Invalid json file: ' (trailing
           space) . ' - Syntax error, malformed JSON' (leading space).
           NDRC's json.c reports a positioned reason PHP's json_decode
           never gives (json.h); that detail is appended in parens,
           marked here as an NDRC addition rather than folded into the
           ported text, which stays exactly what DRB would have printed
           for its one JSON_ERROR_SYNTAX case (the only json_last_error
           branch this dialect can plausibly hit in practice). */
        diag_fatal(d,
            "Invalid json file:  - Syntax error, malformed JSON (NDRC: line %d, col %d: %s)",
            jr.line, jr.col, jr.err);
        return diag_exit_code(d);
    }

    /* PORT: parseOptionalParameters, drb.php:1358-1397, invoked from
       drb.php:1769 - AFTER the JSON decode has already succeeded. The
       loop is the caller's (see run_from_json); its first error arrives
       as opts->option_error, reported here so it keeps drb's own
       position - after the Target line and the JSON decode, before the
       output-name defaulting below. */
    if (opts->option_error != NULL) {
        diag_fatal(d, "%s", opts->option_error);
        return diag_exit_code(d);
    }
    diag_set_verbose(d, opts->verbose);

    /* PORT: drb.php:1770. */
    if (output_path == NULL) {
        output_path = replace_extension(a, opts->input_name, "DDB");
    }

    /* PORT: drb.php:1771. Literal string comparison, matching PHP's ==
       on the two names - no path canonicalisation either side. */
    if (strcmp(opts->input_name, output_path) == 0) {
        diag_fatal(d, "Input and output file name cannot be the same");
        return diag_exit_code(d);
    }

    /* PORT: drb.php:1773 - checked after the output-filename
       defaulting/collision checks above, matching drb.php's order
       (parseOptionalParameters returns, then 1770/1771, then this). */
    if (opts->forced_padding && opts->forced_no_padding) {
        diag_fatal(d, "You can't force padding and no padding at the same time");
        return diag_exit_code(d);
    }

    /* PORT: drb.php:1775. */
    if (opts->verbose) printf("Verbose mode on\n");

    /* PORT: drb.php:1779-1780. */
    if (strcmp(target->name, "C64") != 0 && strcmp(target->name, "CP4") != 0 &&
        opts->prepend_c64) {
        diag_fatal(d, "Adding C64 header was requested but target is not C64 or CP4");
        return diag_exit_code(d);
    }
    if (strcmp(target->name, "ZX") != 0 && opts->prepend_plus3) {
        diag_fatal(d, "Adding +3DOS header was requested but target is not ZX Spectrum");
        return diag_exit_code(d);
    }
    if (opts->nxtext) {
        const char *bad = NULL;
        if (strcmp(target->name, "NEXTDAAD") != 0)
            bad = "-nxtext requires the NEXTDAAD target";
        else if (opts->dump_to_xmb) bad = "-X is not available with -nxtext";
        else if (opts->tok_tee) bad = "--tok is not available with -nxtext";
        if (bad != NULL) {
            diag_fatal(d, "%s", bad);
            return diag_exit_code(d);
        }
    }

    /* PORT: model_from_json runs DRB's replaceEscapeChars/checkStrings
       passes (drb.php:1787-1789) and extracts settings[0] (drb.php:
       1797-1801) - see model.h. Every diagnostic it can report is
       already a diag_fatal; NULL means one was. */
    adv = model_from_json(a, d, jr.root);
    if (adv == NULL) return diag_exit_code(d);

    /* PORT: drb.php:1797-1799 - classic-mode resolution, run
       immediately after settings[0] is read (model_from_json's job,
       matching drb.php:1797) and before every check that depends on
       it: the debug-mode check below (drb.php:1802-1806 comes after
       1797-1799 in the PHP), the NEXTDAAD refusal, and the verbose
       report. forcedClassicMode ORs onto the JSON value; the result is
       written back into adv->classic_mode, mirroring drb.php:1799
       writing back into $adventure->classicMode - emit_proc.c and this
       file's own verbose report both read adv->classic_mode
       afterward, matching the PHP reading $adventure->classicMode
       post-resolution everywhere downstream. tokens_compress also
       takes the resolved value as an explicit parameter, per the
       brief's Interfaces block - both mechanisms carry the same
       resolved value from this point on. */
    classic = adv->classic_mode || opts->forced_classic;
    adv->classic_mode = classic;

    /* PORT: drb.php:1800-1801 - $adventure->debugMode read from
       settings[0] is model_from_json's own job (matching drb.php:1800);
       forcedDebugMode OR's onto it here, same shape as classic_mode's
       own OR-in immediately above. */
    if (opts->forced_debug) adv->debug_mode = 1;

    /* PORT: drb.php:1802-1806, run immediately after settings[0] is
       read (model_from_json's job, matching drb.php:1800-1801) and
       before any of the checks below. Live from this task on: -d is
       wired for real above, and DEBUG.DSF's own #debug directive sets
       debug_mode=1 in every committed fixture that exercises this path
       (task-6-brief.md). */
    if (adv->debug_mode && !target->debug_allowed) {
        printf("Debug mode active, but target is not ZX. Debug mode deactivated.");
        adv->debug_mode = 0;
    }

    /* Pre-DRC interpreters cannot read a NEXTDAAD DDB, so #classic is
       refused for NEXTDAAD; -c cannot bypass the refusal. */
    if (classic && strcmp(target->name, "NEXTDAAD") == 0) {
        diag_fatal(d,
            "#classic is not supported on NEXTDAAD: the original pre-DRC "
            "interpreters cannot read a NEXTDAAD database at all, since the "
            "machine byte and pointer base both differ");
        return diag_exit_code(d);
    }

    /* PORT: drb.php:1810-1818. adv->v3code may be 0 or 1 here -
       drb.php:1817 only prints the "Linking DAAD v3 DDB." line when
       $v3code is truthy, so a v2 run prints no such line; that gating
       is already how the v3code check below reads. classic_mode
       applies to every target except NEXTDAAD (fatal above), using the
       same resolved adv->classic_mode the refusal check above just
       read. forced_no_padding/forced_padding are BackendOptions fields
       the caller's own arg parsing filled (no Adventure field, same
       shape as dump_to_xmb below). dumpToXMB is the same shape,
       opts->dump_to_xmb (no Adventure field either - drb.php reads
       $adventure->dumpToXMB, this port reads the caller's flag
       directly, same value). */
    if (opts->verbose) {
        if (adv->classic_mode) printf("Classic mode ON, optimizations disabled.\n");
        else printf("Classic mode OFF, optimizations enabled.\n");
        if (adv->debug_mode) printf("Debug mode ON, generating DEBUG information for ZesarUX debugger.\n");
        /* PORT: drb.php:1814-1815. */
        if (opts->forced_no_padding) printf("No padding has been forced.\n");
        if (opts->forced_padding) printf("Padding has been forced.\n");
        /* PORT: drb.php:1816. */
        if (opts->dump_to_xmb) printf("Generating TX sections in a separated .TX file.\n");
        if (adv->v3code) printf("Linking DAAD v3 DDB.\n");
    }

    /* PORT: drb.php:1823 - getBaseAddressByTarget(target,subtarget)
       consults $GLOBALS['adventure']->forcedBaseAddress FIRST, ahead of
       its own per-target switch (drb.php:1287), so -b= overrides the
       target's own default base address everywhere that value is read
       from here on: the verbose Base address line below, the initial
       `addr`/`out` emission cursor, and every target->base_address read
       downstream (ddb_size, the two Database-starts prints - grepped
       per task-6-brief.md Step 3). */
    base_address = opts->forced_base >= 0 ? opts->forced_base : (long)target->base_address;

    /* PORT: drb.php:1823-1831. */
    if (opts->verbose) {
        /* PORT: drb.php:1829, 1310-1313 as fixed upstream at ff45ff2
           (2026-08-28): the flag's inversion was corrected, so the
           display now names the real file byte order. Bytes never
           changed - the old double inversion cancelled. */
        printf("Endianness is %s endian\n", target->big_endian ? "big" : "little");
        {
            char buf[24];
            fmt_addr(buf, sizeof buf, base_address);
            printf("Base address      [%s]\n", buf);
        }
    }

    addr = base_address;
    out = str_new(a);
    /* PORT: drb.php:288-297/1373-1375 - the forced-padding/no-padding
       write context (layout.h), set as soon as `out` exists so the
       FIRST layout_pad call (after emit_externs, below) already sees
       it; forced_padding/forced_no_padding themselves were already
       validated non-conflicting above (drb.php:1773). */
    layout_set_forced(opts->forced_padding, opts->forced_no_padding, out, output_path);
    emit_header(out, &addr, target, adv, lang_bit);

    /* PORT: drb.php:1887-1903, the .tok override lookup (see tokens.h)
       falling back to the builtin EN table. drb.php reuses a single
       $tokensFilename variable to probe, load AND print (drb.php:1749-
       1756, 1887-1891), so the text it prints always names whichever
       candidate it actually opened; resolved_path here is that same
       candidate (tokens_load_override's out-param), not a fixed re-guess
       of the first candidate. */
    {
        int errors_before = diag_error_count(d);
        const char *resolved_path = NULL;
        TokenSet *override = NULL;

        if (opts->auto_tokens) {
            const char *bypassed = tokens_probe_override(a, opts->input_name);
            if (bypassed != NULL) {
                printf("Warning: -auto-tokens overrides tokens file %s.\n",
                       bypassed);
            }
            if (opts->verbose) printf("Auto-selecting compression tokens.\n");
            /* tokselect_run always sets optimal_encode; selecting without
               objects keeps the frozen table equal to the reload's. */
            compress_objects = !opts->no_object_tokens;
            ts = tokselect_run(a, d, adv,
                               strcmp(target->name, "NEXTDAAD") != 0,
                               compress_objects,
                               strcmp(target->name, "NEXTDAAD") != 0);
        } else {
            override = tokens_load_override(a, d, opts->input_name, &resolved_path);
            if (override != NULL) {
                ts = override;
                if (opts->verbose) {
                    printf("Loading tokens from %s.\n", resolved_path);
                }
            } else if (diag_error_count(d) > errors_before) {
                return diag_exit_code(d);   /* override found but malformed */
            } else {
                ts = tokens_load_builtin(a, d, language);
                if (ts == NULL) return diag_exit_code(d);
                if (opts->verbose) printf("Loading default compression tokens for '%s'.\n", language);
            }
        }
    }
    if (opts->no_object_tokens && !ts->optimal_encode) {
        diag_fatal(d, "-no-object-tokens requires -auto-tokens");
        return diag_exit_code(d);
    }
    if (opts->no_object_tokens) ts->objects_off = 1;
    /* Separate switches so each can be gated per target later. */
    share_texts = ts->optimal_encode;
    compress_objects = ts->optimal_encode && !ts->objects_off;

    /* PORT: drb.php:1919-1924. Dump XMessages if available, before the
       extern pad. The Maluva guard (drb.php:1922) is dead - see
       xmes_maluva_embedded above. */
    if (vec_len_Message(adv->xmessages)) {
        if (!xmes_check_maluva() && !xmes_maluva_embedded()) {
            diag_fatal(d, "XMESSAGE condact requires Maluva Extension");
            return diag_exit_code(d);
        }
        /* dumpToXMB forces the 64K limit unconditionally, regardless of
           target - drb.php:422/426, read inside generateXMessages
           (drb.php:454) before its own per-target switch (drb.php:
           427-446) runs. */
        /* -nxtext writes the XMB once, after coding (nxtext_select). */
        if (!opts->nxtext) {
            Target xmb_target = *target;
            if (opts->dump_to_xmb) xmb_target.xmessage_size_k = 64;
            if (!emit_xmessages(d, &xmb_target, adv)) return diag_exit_code(d);
        }
    }

    /* PORT: drb.php:1926-1932 - the -X append open, BEFORE the extern
       region. Cursor = existing file size when 0.XMB already exists:
       this is exactly the XMessages-then-append interplay (module
       docstring, task-4-brief.md) - when the adventure has XMessages,
       emit_xmessages above has already written 0.XMB, and this open
       finds it and appends from that existing size (xmb_existing_size,
       mirroring PHP's file_exists+filesize pair). ndrc opens the file
       with stdio in the cwd exactly as emit_xmb.c does (relative fopen,
       no path handling); "ab" is NDRC's own append-only mode (PHP opens
       "a+", but nothing here ever reads the stream back). */
    xmb_fh = NULL;
    xmb_addr = 0;
    if (opts->dump_to_xmb) {
        xmb_addr = xmb_existing_size("0.XMB");
        xmb_fh = fopen("0.XMB", "ab");
        if (xmb_fh == NULL) {
            diag_fatal(d, "Can't create output TX file");
            return diag_exit_code(d);
        }
    }

    /* PORT: drb.php:1934-1938. */
    previous_address = addr;
    if (!emit_externs(out, &addr, d, adv, target)) return diag_exit_code(d);
    layout_pad(out, &addr, target);
    extern_size = addr - previous_address;
    previous_address = addr;

    /* Not in DRC: the optimal encoder compresses before OTX is written so
       object texts go out compressed; kept below the XMB writes above,
       whose bytes must not change. */
    if (ts->optimal_encode) {
        /* Objects always snapshotted: the emitted-DDB check decodes
           them whether compressed or not. */
        pre_texts = tokselect_snapshot(a, adv, 1);
        if (opts->verbose) printf("Optimal-parse text encoding.\n");
        /* Classic name printers scan stored object bytes; NEXTDAAD
           strips decoded text. */
        final_tokens = tokselect_compress(a, d, adv, ts, classic,
                                          compress_objects,
                                          strcmp(target->name, "NEXTDAAD") != 0,
                                          &text_savings);
        if (final_tokens == NULL) return diag_exit_code(d);
        if (!tokselect_verify(pre_texts, adv, final_tokens, compress_objects)) {
            diag_fatal(d, "auto-tokens self-check failed: compressed "
                          "text does not decode back to source");
            return diag_exit_code(d);
        }
        if (opts->verbose && compress_objects)
            printf("Object texts compressed.\n");
        if (opts->nxtext) {
            text2 = nxtext_select(a, d, target, adv, pre_texts, final_tokens,
                                  opts->verbose, t2_tabs, &t2_block,
                                  &text_savings);
            if (text2 < 0) return diag_exit_code(d);
        }
    }

    /* PORT: drb.php:1940-1945 generateOTX, backed by emit_messages -
       see emit.h. drb.php:1941 hard-codes dumpToXMB=false for OTX
       regardless of adv->dumpToXMB - object texts must be dumped to RAM
       so the -x flag can find them (see the PHP comment there). xmb_fh/
       &xmb_addr are threaded through regardless (unused down that
       branch when the dump flag passed here is 0). */
    emit_messages(out, &addr, target, text2 ? t2_tabs[4] : adv->objects, 0, 0,
                  xmb_fh, &xmb_addr, NULL, share_texts, text2, &otx_share);
    report_share(opts->verbose, &otx_share, "objects", &text_savings);
    /* PORT: drb.php:1942 - the object_data-not-objects quirk (S12/
       S6.6): the lookup table is 2 bytes per OBJECT_DATA entry, not
       per OTX (objects) entry, even though it is OTX's own address
       being adjusted. */
    object_lookup_offset = addr - 2 * (long)vec_len_ObjectData(adv->object_data);
    print_map_line(opts->verbose, "Object texts      ", object_lookup_offset);
    layout_pad(out, &addr, target);
    otx_size = addr - previous_address;
    previous_address = addr;

    /* PORT: drb.php:1947-1951 generateObjectNames. */
    object_names_offset = addr;
    print_map_line(opts->verbose, "Object words      ", object_names_offset);
    emit_object_names(out, &addr, adv);
    obj_size = addr - previous_address;
    previous_address = addr;

    /* PORT: drb.php:1954-1960 generateObjectWeightAndAttr. drb.php:
       1956's "Weight & std attr" map line has no `echo` before its
       string literal - S12.8, dropped deliberately, not a typo in our
       port. */
    object_weight_attr_offset = addr;
    emit_object_weight_attr(out, &addr, d, adv,
                            compress_objects || text2
                                ? vec_at_MsgTable(pre_texts, 4)
                                : adv->objects);
    layout_pad(out, &addr, target);
    objweight_size = addr - previous_address;
    previous_address = addr;

    /* PORT: drb.php:1962-1967 generateObjectExtraAttr. NO
       addPaddingIfRequired call here, unlike every neighbouring
       table - ported literally. */
    object_extra_attr_offset = addr;
    print_map_line(opts->verbose, "Extra attr        ", object_extra_attr_offset);
    emit_object_extra_attr(out, &addr, target, adv);
    objattr_size = addr - previous_address;
    previous_address = addr;

    /* PORT: drb.php:1970-1976 generateObjectInitially. */
    initially_at_offset = addr;
    print_map_line(opts->verbose, "Initially at      ", initially_at_offset);
    emit_object_initially(out, &addr, adv);
    layout_pad(out, &addr, target);
    objinitially_size = addr - previous_address;
    previous_address = addr;

    /* PORT: drb.php:1980 - the redundant double pad before Vocabulary
       (analysis padding table): this addPaddingIfRequired call is on
       top of the one that already ran at the end of InitiallyAt,
       above. Harmless on NEXTDAAD (not a padding platform) but ported
       structurally regardless. */
    layout_pad(out, &addr, target);
    vocabulary_offset = addr;
    print_map_line(opts->verbose, "Vocabulary        ", vocabulary_offset);
    emit_vocabulary(out, &addr, adv);
    layout_pad(out, &addr, target);
    voc_size = addr - previous_address;
    previous_address = addr;

    /* PORT: drb.php:1989-1995. compressedTextOffset is decided before
       compression runs (drb.php:1990), which is why the "Tokens [...]"
       map line prints BEFORE the token warnings tokens_compress emits
       internally via diag_verbose - matching the measured reference
       transcript's order exactly. */
    compressed_text_offset = ts->has_tokens || text2 ? addr : 0;
    print_map_line(opts->verbose, "Tokens            ", compressed_text_offset);
    if (!ts->optimal_encode) {
        final_tokens = tokens_compress(a, d, adv, ts, classic, &text_savings);
        if (final_tokens == NULL) return diag_exit_code(d);
    }
    if (text2) {
        /* Format 2: the text block stands in for the token table. */
        str_append_n(out, str_bytes(t2_block), str_len(t2_block));
        addr += (long)str_len(t2_block);
    } else {
        tokens_emit(out, final_tokens, &addr);
    }
    layout_pad(out, &addr, target);
    token_size = addr - previous_address;
    previous_address = addr;

    /* PORT: drb.php:1998-2004 generateSTX, backed by emit_messages.
       is_stx=1 marks the sysmess call (drb.php:532's LAST_DEFAULT_
       SYSMESS special case); dump_to_xmb is this task's own local flag,
       matching drb.php:1999's $adventure->dumpToXMB read. */
    emit_messages(out, &addr, target, text2 ? t2_tabs[2] : adv->sysmess,
                  opts->dump_to_xmb, 1, xmb_fh, &xmb_addr, NULL,
                  share_texts, text2, &stx_share);
    report_share(opts->verbose, &stx_share, "sysmess", &text_savings);
    sysmess_lookup_offset = addr - 2 * (long)vec_len_Message(adv->sysmess);
    print_map_line(opts->verbose, "Sysmess           ", sysmess_lookup_offset);
    layout_pad(out, &addr, target);
    stx_size = addr - previous_address;
    previous_address = addr;

    /* PORT: drb.php:2006-2012 generateMTX. */
    emit_messages(out, &addr, target, text2 ? t2_tabs[1] : adv->messages,
                  opts->dump_to_xmb, 0, xmb_fh, &xmb_addr, NULL,
                  share_texts, text2, &mtx_share);
    report_share(opts->verbose, &mtx_share, "messages", &text_savings);
    message_lookup_offset = addr - 2 * (long)vec_len_Message(adv->messages);
    print_map_line(opts->verbose, "Messages          ", message_lookup_offset);
    layout_pad(out, &addr, target);
    mtx_size = addr - previous_address;
    previous_address = addr;

    /* PORT: drb.php:2015-2021 generateLTX. */
    emit_messages(out, &addr, target, text2 ? t2_tabs[0] : adv->locations,
                  opts->dump_to_xmb, 0, xmb_fh, &xmb_addr, NULL,
                  share_texts, text2, &ltx_share);
    report_share(opts->verbose, &ltx_share, "locations", &text_savings);
    location_lookup_offset = addr - 2 * (long)vec_len_Message(adv->locations);
    print_map_line(opts->verbose, "Locations         ", location_lookup_offset);
    layout_pad(out, &addr, target);
    ltx_size = addr - previous_address;
    previous_address = addr;

    /* Not in DRC: decodes the emitted texts through their lookups, which
       tokselect_verify cannot see (sharing happens at emission). -X
       dumps sysmess past 62 and every message and location (emit_msg.c). */
    if (ts->optimal_encode) {
        size_t nobj = vec_len_Message(adv->objects);
        size_t nsys = vec_len_Message(adv->sysmess);
        size_t nmsg = vec_len_Message(adv->messages);
        size_t nloc = vec_len_Message(adv->locations);
        /* object_lookup_offset is sized by object_data (drb.php:1942);
           the table itself holds one word per object text. */
        long otx_table = object_lookup_offset
            + 2 * ((long)vec_len_ObjectData(adv->object_data) - (long)nobj);
        DdbTableRef refs[4] = {
            { 4, otx_table, nobj, nobj, "objects" },
            { 2, sysmess_lookup_offset, nsys,
              opts->dump_to_xmb && nsys > 63 ? 63 : nsys, "sysmess" },
            { 1, message_lookup_offset, nmsg,
              opts->dump_to_xmb ? 0 : nmsg, "messages" },
            { 0, location_lookup_offset, nloc,
              opts->dump_to_xmb ? 0 : nloc, "locations" },
        };
        int bad_ref = 0;
        size_t bad_index = 0, skipped = 0;
        /* Format 2 is keyed on version bit 7; the header patch sets it too. */
        if (text2) str_set_u8(out, 0, (adv->v3code ? 3u : 2u) | 0x80u);
        if (!tokselect_verify_ddb(pre_texts, str_bytes(out),
                                  str_len(out), base_address,
                                  compressed_text_offset, target->big_endian,
                                  refs, &bad_ref, &bad_index, &skipped)) {
            if (bad_ref < 0)
                diag_fatal(d, "auto-tokens self-check failed: emitted text "
                              "block is malformed");
            else
                diag_fatal(d, "auto-tokens self-check failed: emitted %s "
                              "text %lu does not decode back to source",
                           refs[bad_ref].name, (unsigned long)bad_index);
            return diag_exit_code(d);
        }
        if (text2 && vec_len_Message(t2_tabs[3]) > 0 &&
            !xmb2_check(a, d, adv, pre_texts, t2_tabs[3], t2_block, out,
                        base_address, compressed_text_offset))
            return diag_exit_code(d);
        if (opts->verbose && skipped > 0)
            printf("File check: skipped %lu XMB-dumped texts.\n",
                   (unsigned long)skipped);
    }
    if (ts->optimal_encode && opts->tok_tee) {
        const char *tee = opts->tok_tee_path != NULL
                          ? opts->tok_tee_path
                          : replace_extension(a, opts->input_name, "tok");
        if (!tokens_write_tok(tee, ts)) {
            diag_fatal(d, "Can't create tokens file %s", tee);
            return diag_exit_code(d);
        }
        if (opts->verbose) printf("Tokens written to %s.\n", tee);
    }

    /* PORT: drb.php:2023-2028 generateConnections. emit_connections
       runs its own trailing pad internally (emit_con.c PORT NOTE), so
       no separate layout_pad call follows here - matching drb.php,
       which has none either. */
    connections_lookup_offset = emit_connections(out, &addr, target, adv);
    print_map_line(opts->verbose, "Connections       ", connections_lookup_offset);
    con_size = addr - previous_address;
    previous_address = addr;

    /* PORT: drb.php:2031-2036 generateProcesses. Can diag_fatal
       (PROCESS-opcode reference to a non-existent process, emit_proc.c)
       - guard and halt exactly as DRC halts at its first error, before
       computing an offset from a possibly-incomplete address or
       writing anything to disk. */
    emit_processes(out, &addr, d, target, adv, opts->verbose,
                   opts->srcmap_path != NULL ? &proc_addrs : NULL);
    if (diag_error_count(d) > 0) return diag_exit_code(d);
    process_list_offset = addr - 2 * (long)vec_len_Process(adv->processes);
    print_map_line(opts->verbose, "Processes         ", process_list_offset);
    pro_size = addr - previous_address;
    previous_address = addr;
    (void)pro_size;   /* used below via SIZE PER BLOCK DETAIL */

    /* PORT: drb.php:2039-2073, the header patch pass - see emit_hdr.h
       for the exact word order. */
    offsets[0] = compressed_text_offset;
    offsets[1] = process_list_offset;
    offsets[2] = object_lookup_offset;
    offsets[3] = location_lookup_offset;
    offsets[4] = message_lookup_offset;
    offsets[5] = sysmess_lookup_offset;
    offsets[6] = connections_lookup_offset;
    offsets[7] = vocabulary_offset;
    offsets[8] = initially_at_offset;
    offsets[9] = object_names_offset;
    offsets[10] = object_weight_attr_offset;
    offsets[11] = object_extra_attr_offset;
    offsets[12] = addr;   /* drb.php:2067-2068 fileSize */
    /* PORT: drb.php:2069-2070. If target is PLUS3, put the XMessage size
       instead of the DDB size at the SPARE/filesize header position, as
       it's needed for the +3DOS XMessage support. Dormant in every gate
       (no PLUS3-with-xmessages golden fixture) - PORT NOTE per spec
       5.5. This is the one permitted subtarget strcmp outside the CLI,
       mirroring DRB's own literal test. */
    if (target->subtarget != NULL && strcmp(target->subtarget, "PLUS3") == 0
        && adv->xmessage_size != 0) {
        offsets[12] = adv->xmessage_size;
    }
    emit_header_patch(out, target, adv, offsets, text2);

    /* PORT: drb.php:2074-2075 - fclose($outputFileHandler) then
       fclose($XMBFileHandler), right after the header patch pass. This
       port's DDB write is deferred to a single fwrite near the end
       (Task 7 PORT NOTE, in-memory build), so only the XMB stream's
       close has a literal counterpart here - closed as soon as nothing
       further writes to it (every emit_messages call site above has
       already run). */
    if (opts->dump_to_xmb) fclose(xmb_fh);

    /* PORT: drb.php:256-270 summary(), drb.php:2077. */
    if (opts->verbose) {
        printf("\n");
        printf("Adventure Totals\n");
        printf("================\n");
        printf("Locations   : %lu\n", (unsigned long)vec_len_Message(adv->locations));
        printf("Objects     : %lu\n", (unsigned long)vec_len_Message(adv->objects));
        printf("Messages    : %lu\n", (unsigned long)vec_len_Message(adv->messages));
        printf("Sysmess     : %lu\n", (unsigned long)vec_len_Message(adv->sysmess));
        if (vec_len_Message(adv->xmessages)) {
            printf("XMessages   : %lu\n", (unsigned long)vec_len_Message(adv->xmessages));
        }
        printf("Connections : %lu\n", (unsigned long)vec_len_Connection(adv->connections));
        printf("Processes   : %lu\n", (unsigned long)vec_len_Process(adv->processes));
        printf("\n");
        printf("%s for %s created.\n", output_path, target->name);
    }

    /* PORT: drb.php:2079. Simplified from PHP's own message (which
       embeds a literal "\n" inside the Error() text, producing a
       doubled newline+period artifact when combined with Error()'s own
       ".\n" suffix - a PHP-side formatting defect diag.h's fixed
       "Error: %s.\n" shape cannot reproduce without violating its own
       no-trailing-period contract). This boundary is reachable by any
       sufficiently large, well-formed game whose DDB genuinely exceeds
       the 65535-byte address space - not only by malformed input; it is
       simply never exercised by any fixture currently committed to this
       repo (all well under the boundary), so neither the byte nor map
       gates exercise it yet. */
    if (addr > 0xFFFF) {
        diag_fatal(d, "DDB file goes %ld bytes over the 65535 memory address boundary",
                   addr - 0xFFFFL);
        return diag_exit_code(d);
    }

    /* PORT: drb.php:2082-2100, the SIZE PER BLOCK DETAIL block -
       always printed, never gated on verbose. object_data's own count
       byte lives in the header, not a table of its own, so
       "Object definitions" here is obj_size: generateObjectNames's 2-
       bytes-per-object table (drb.php's naming, not this port's -
       reproduced as-is). The lowercase "object texts" label and the
       "System Mesages" misspelling are both ported verbatim, as is the
       S12.4 defect: "DDB Header & others" prints EXTERNSize a second
       time rather than its own (never separately tracked) size. */
    ddb_size = addr - base_address;
    printf("\nSIZE PER BLOCK DETAIL\n");
    printf("=================================\n");
    printf("Vocabulary          : %5ld bytes\n", voc_size);
    printf("Compression Tokens  : %5ld bytes\n", token_size);
    printf("object texts        : %5ld bytes\n", otx_size);
    printf("Object definitions  : %5ld bytes\n", obj_size);
    printf("Object weights      : %5ld bytes\n", objweight_size);
    printf("Object attributes   : %5ld bytes\n", objattr_size);
    printf("Object location     : %5ld bytes\n", objinitially_size);
    printf("System Mesages      : %5ld bytes\n", stx_size);
    printf("User Messages       : %5ld bytes\n", mtx_size);
    printf("Location texts      : %5ld bytes\n", ltx_size);
    printf("Connections         : %5ld bytes\n", con_size);
    printf("Processes           : %5ld bytes\n", pro_size);
    printf("Extern routines     : %5ld bytes\n", extern_size);
    printf("DDB Header & others : %5ld bytes\n", extern_size);
    printf("\n");

    /* PORT: drb.php:2104. */
    {
        char base_buf[24], end_buf[24];
        fmt_addr(base_buf, sizeof base_buf, base_address);
        fmt_addr(end_buf, sizeof end_buf, addr);
        printf("Total DDB size is %ld bytes.\n", ddb_size);
        printf("Database starts at %ld (%s)\n", base_address, base_buf);
        printf("Database ends at address %ld (%s)\n", addr, end_buf);
    }

    /* PORT: drb.php:2109-2114. */
    if (adv->xmessage_size) {
        if (adv->xmessage_padding == 0)
            printf("XMessages size is %ld bytes in files of %ldK.\n",
                   adv->xmessage_size, adv->xmessage_max_k);
        else
            printf("XMessages size is %ld bytes in files of 64K (%ldK padding) .\n",
                   adv->xmessage_size,
                   (long)((adv->xmessage_padding + 512) / 1024));
            /* note the space before the period - drb.php:2113, verbatim;
               the padding K is PHP round($paddingSize/1024, 0) - half-up
               rounding, (x+512)/1024 in integer C for positive x */
    }

    /* PORT: drb.php:2116. */
    if (text_savings > 0) {
        printf("Text compression savings: %ld bytes.\n", text_savings);
    }

    /* PORT: drb.php:2117-2127 - the two header prepends, in this order
       and with the verbose line on each side drb.php has it (BEFORE the
       C64 prepend, AFTER the +3 prepend). Both mutate `out` in place
       (finish.h); Task 7's PORT NOTE: DRB prepends by re-reading the
       file it already wrote and rewriting it, which this port skips by
       mutating the in-memory Str before the single fwrite below - only
       the resulting bytes are ported, not PHP's rewrite mechanism. */
    unprefixed_len = str_len(out);
    if (opts->prepend_c64) {
        if (opts->verbose) printf("Adding Commodore header\n");
        finish_prepend_c64(out, base_address);
    }
    if (opts->prepend_plus3) {
        finish_prepend_plus3(out);
        if (opts->verbose) printf("+3DOS header added\n");
    }

    /* PORT: drb.php:1791/2074 - opens for streaming writes far earlier in
       the PHP than here, since this port builds the whole DDB in memory
       first (Str, matching every Task 1-7 emitter) rather than streaming
       it; only the resulting bytes are ported, not PHP's incremental
       write timing. Moved after the prepends above (Task 7): the bytes
       written here must already carry any -ch/-3h header. */
    fp = fopen(output_path, "wb");
    if (fp == NULL) {
        diag_fatal(d, "Can't create output file");
        return diag_exit_code(d);
    }
    fwrite(str_bytes(out), 1, str_len(out), fp);
    fclose(fp);

    if (opts->srcmap_path != NULL) {
        long shift = (long)(str_len(out) - unprefixed_len) - base_address;
        if (!write_srcmap(opts->srcmap_path, &proc_addrs, shift,
                          (const uint8_t *)str_bytes(out), str_len(out))) {
            diag_fatal(d, "Can't create source map file %s", opts->srcmap_path);
            return diag_exit_code(d);
        }
        if (opts->verbose) printf("Source map written to %s.\n", opts->srcmap_path);
    }

    /* PORT: drb.php:2129-2131. */
    if (strcmp(target->name, "HTML") == 0) {
        if (!finish_write_jddb(d, output_path, str_bytes(out), str_len(out))) {
            return diag_exit_code(d);
        }
    }

    return diag_exit_code(d);
}
