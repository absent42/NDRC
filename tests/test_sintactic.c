/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_sintactic.c - Copyright (C) 2026 Dan Gibson.

   Task 6 suite: sintactic.c's driver/preprocessor/text-voc-ctl half,
   include.c (the Preparse stage) and ctlextern.c. Token streams are
   hand-built Token lists (no lexer involved) except the preparse and
   remap tests, which go through real files (the test_lex.c
   scratch_path pattern) because Preparse's only surface is a pair of
   paths. Every expected diagnostic text below is the USintactic.pas
   source literal (analysis part2-sintactic section 8's catalogue);
   texts whose Pascal literal itself ends in '.' keep that period here
   and gain a second one on the wire (defect 19.40's family). */
#include "test.h"
#include "arena.h"
#include "diag.h"
#include "../src/front/sintactic.h"
#include "../src/front/include.h"
#include "../src/front/ctlextern.h"
#include "../src/front/lex_tokens.h"
#include "../src/front/constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void scratch_path(char *buf, size_t bufsz, const char *filename)
{
    const char *dir = getenv("TMPDIR");
    if (dir == NULL) dir = getenv("TEMP");
    if (dir == NULL) dir = getenv("TMP");
    if (dir == NULL) dir = ".";
    snprintf(buf, bufsz, "%s/%s", dir, filename);
}

static void write_bytes(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f == NULL) return;
    fwrite(data, 1, len, f);
    fclose(f);
}

/* ---- fixture ---- */

typedef struct {
    Arena *a;
    Diag *d;
    FILE *out;
    FrontOptions opts;
} Fx;

static Fx fx_open(void)
{
    Fx f;
    f.a = arena_new(0);
    f.d = diag_new(f.a);
    f.out = tmpfile();
    diag_set_stream(f.d, f.out);
    diag_set_source(f.d, "test.dsf");
    include_reset();
    sintactic_init(f.a, f.d);
    memset(&f.opts, 0, sizeof f.opts);
    f.opts.target = "NEXTDAAD";
    f.opts.subtarget = "";
    f.opts.check_maluva = 1;
    return f;
}

static void fx_close(Fx *f)
{
    fclose(f->out);
    arena_free(f->a);
}

static void fx_read(Fx *f, char *buf, size_t n)
{
    size_t got;
    rewind(f->out);
    got = fread(buf, 1, n - 1, f->out);
    buf[got] = '\0';
}

/* ---- hand-built token streams ---- */

typedef struct {
    Arena *a;
    Token *head, *tail;
    int line;
} TB;

static TB tb_new(Arena *a)
{
    TB b;
    b.a = a;
    b.head = b.tail = NULL;
    b.line = 0;
    return b;
}

static void tb_add(TB *b, int id, const char *text, long value)
{
    Token *t = arena_alloc(b->a, sizeof(*t));
    t->id = id;
    t->text = arena_strdup(b->a, text);
    t->value = value;
    t->line = ++b->line; /* one line per token: distinct positions */
    t->col = 7;
    t->next = NULL;
    if (b->tail) b->tail->next = t; else b->head = t;
    b->tail = t;
}

/* id token with no numeric value (the lexer's MaxLongInt filler) */
static void t(TB *b, int id, const char *text)
{
    tb_add(b, id, text, TOKEN_NO_VALUE);
}

/* T_NUMBER with a real value, as lexer rule 38 produces */
static void n(TB *b, const char *text, long value)
{
    tb_add(b, T_NUMBER, text, value);
}

/* T_STRING with its two delimiter quotes, as rule 30 stores them */
static void s(TB *b, const char *inner)
{
    char buf[1040];
    snprintf(buf, sizeof buf, "\"%s\"", inner);
    tb_add(b, T_STRING, buf, TOKEN_NO_VALUE);
}

/* numbered list entry: text keeps the '/', value is real (rule 35) */
static void le(TB *b, long num)
{
    char buf[32];
    snprintf(buf, sizeof buf, "/%ld", num);
    tb_add(b, T_LIST_ENTRY, buf, num);
}

/* named list entry: slash STRIPPED from text, sentinel value (rule 36) */
static void le_name(TB *b, const char *name)
{
    tb_add(b, T_LIST_ENTRY, name, TOKEN_NO_VALUE);
}

/* /CTL /VOC */
static void head_ctl_voc(TB *b)
{
    t(b, T_SECTION_CTL, "/CTL");
    t(b, T_SECTION_VOC, "/VOC");
}

/* /STX /MTX /OTX /LTX */
static void sections_stx_to_ltx(TB *b)
{
    t(b, T_SECTION_STX, "/STX");
    t(b, T_SECTION_MTX, "/MTX");
    t(b, T_SECTION_OTX, "/OTX");
    t(b, T_SECTION_LTX, "/LTX");
}

/* /0 "room" /CON /0 /OBJ /PRO 0 /END - one location, empty world */
static void world_close(TB *b)
{
    le(b, 0);
    s(b, "room");
    t(b, T_SECTION_CON, "/CON");
    le(b, 0);
    t(b, T_SECTION_OBJ, "/OBJ");
    t(b, T_SECTION_PRO, "/PRO");
    n(b, "0", 0);
    t(b, T_SECTION_END, "/END");
}

/* the smallest program that reaches /END */
static void minimal_program(TB *b)
{
    head_ctl_voc(b);
    sections_stx_to_ltx(b);
    world_close(b);
}

/* first condact list of process p's entry e, for the #db/#dw checks */
static Vec_ProcessCondact *entry_condacts(long p, size_t e)
{
    const ProcessSlot *slot = processtable_get(sintactic_processes(), p);
    if (slot == NULL) return NULL;
    if (e >= vec_len_ProcessEntry(slot->entries)) return NULL;
    return vec_at_ProcessEntry(slot->entries, e)->condacts;
}

/* ---- driver / section order ---- */

TEST(minimal_full_program_parses)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    minimal_program(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(sintactic_messages()->ltx_count, 1);
    CHECK_STR(msglist_at(sintactic_messages()->ltx, 0)->text, "room");
    CHECK_INT(sintactic_last_process(), 0);
    CHECK_INT(diag_error_count(f.d), 0);
    fx_close(&f);
}

/* Defect 19.33: the raw token id is glued onto the message with no
   separator - /STX is 259. */
TEST(wrong_section_order_reports_voc_expected_with_glued_id)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_SECTION_CTL, "/CTL");
    t(&b, T_SECTION_STX, "/STX");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "/VOC expected259");
    fx_close(&f);
}

TEST(ctl_accepts_underscores_before_voc)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_SECTION_CTL, "/CTL");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_SECTION_VOC, "/VOC");
    sections_stx_to_ltx(&b);
    world_close(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    fx_close(&f);
}

TEST(missing_ctl_is_an_error)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_SECTION_VOC, "/VOC");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "/CTL expected");
    fx_close(&f);
}

/* /LTX arriving where /OTX's table runs (i.e. LTX before OTX) is
   rejected by the fixed dispatch: ParseMTX's list parser sees a
   non-terminator, non-entry token. */
TEST(ltx_before_otx_is_rejected)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    t(&b, T_SECTION_MTX, "/MTX");
    t(&b, T_SECTION_LTX, "/LTX");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "List entry number expected");
    fx_close(&f);
}

/* ---- #define / ExtractValue ---- */

TEST(define_number_used_as_voc_value)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    VocEntry e;
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "X");
    n(&b, "5", 5);
    head_ctl_voc(&b);
    t(&b, T_IDENTIFIER, "NORTH");
    t(&b, T_IDENTIFIER, "X");
    t(&b, T_IDENTIFIER, "verb");
    sections_stx_to_ltx(&b);
    world_close(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(voctree_lookup(sintactic_voctree(), f.a, "NORTH", VOC_VERB, &e), 1);
    CHECK_INT(e.value, 5);
    fx_close(&f);
}

TEST(define_duplicate_is_rejected)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "X");
    n(&b, "1", 1);
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "X");
    n(&b, "2", 2);
    minimal_program(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "\"X\" already defined");
    fx_close(&f);
}

TEST(define_requires_identifier)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_DEFINE, "#define");
    n(&b, "5", 5);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Identifier expected after #define");
    fx_close(&f);
}

TEST(define_underscore_value_is_invalid)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "X");
    t(&b, T_UNDERSCORE, "_");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Value for symbol \"X\" is not valid: \"_\"");
    fx_close(&f);
}

/* GetExpressionValue is live: a quoted operand is evaluated and the
   result stored (probe P1). */
TEST(define_expression_is_evaluated)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    long v = -1;
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "X");
    s(&b, "1+2");
    minimal_program(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(symbols_lookup(sintactic_symbols(), f.a, "X", &v), 1);
    CHECK_INT(v, 3);
    fx_close(&f);
}

/* Defect 19.32, probe P23 - truncate FIRST, strip SECOND. A 256-byte
   operand (250 spaces + "1+29" between two quotes) loses its closing
   quote to the ShortString assignment (USintactic.pas:124), so the
   Copy(2, len-2) at USintactic.pas:125 eats the final '9' instead: the
   expression that reaches the parser is "1+2" and the stored value is
   3, not 30 - silently wrong, not merely truncated. */
TEST(define_expression_shortstring_truncation_eats_last_char)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    char operand[256];
    long v = -1;
    memset(operand, ' ', 250);
    memcpy(operand + 250, "1+29", 4);
    operand[254] = '\0';
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "X");
    s(&b, operand);                    /* 254 + 2 quotes = 256 bytes */
    minimal_program(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(symbols_lookup(sintactic_symbols(), f.a, "X", &v), 1);
    CHECK_INT(v, 3);
    fx_close(&f);
}

/* Defect 19.56, probe P47: an expression that evaluates to exactly
   MAXLONGINT collides with the "no value" sentinel, so ExtractValue's
   first sentinel leg (USintactic.pas:145) misdiagnoses it - and it
   reports CurrentText, which still carries its quotes, hence the
   doubling. Reference (drf.exe, 2026-08-27, #define BAD
   "2147483647"): `1:24:g.DSF: ""2147483647"" is not a valid
   expression.`, exit 1. */
TEST(define_expression_maxlongint_result_hits_the_sentinel)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    char buf[512];
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "BAD");
    s(&b, "2147483647");
    minimal_program(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    fx_read(&f, buf, sizeof buf);
    CHECK_STR(buf,
              "3:7:test.dsf: \"\"2147483647\"\" is not a valid expression.\n");
    fx_close(&f);
}

/* 19.13's downstream consequence: the literal 2147483647 arrives
   carrying the sentinel and is rejected as an unknown value. */
TEST(define_maxlongint_literal_rejected)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "X");
    n(&b, "2147483647", 2147483647L);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Value for symbol \"X\" is not valid: \"2147483647\"");
    fx_close(&f);
}

/* ---- #ifdef / #ifndef / #else / #endif ---- */

/* helper: parse a stream whose /VOC uses symbol Y as NORTH's value */
static int parse_with_voc_y(Fx *f, TB *b)
{
    head_ctl_voc(b);
    t(b, T_IDENTIFIER, "NORTH");
    t(b, T_IDENTIFIER, "Y");
    t(b, T_IDENTIFIER, "verb");
    sections_stx_to_ltx(b);
    world_close(b);
    return sintactic_parse(f->a, f->d, b->head, &f->opts);
}

TEST(ifdef_defined_symbol_takes_block)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    VocEntry e;
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "X");
    n(&b, "1", 1);
    t(&b, T_IFDEF, "#ifdef");
    s(&b, "X");
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "Y");
    n(&b, "2", 2);
    t(&b, T_ENDIF, "#endif");
    CHECK_INT(parse_with_voc_y(&f, &b), 0);
    CHECK_INT(voctree_lookup(sintactic_voctree(), f.a, "NORTH", VOC_VERB, &e), 1);
    CHECK_INT(e.value, 2);
    fx_close(&f);
}

TEST(ifdef_undefined_symbol_skips_block)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_IFDEF, "#ifdef");
    s(&b, "NOPE");
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "Y");
    n(&b, "2", 2);
    t(&b, T_ENDIF, "#endif");
    /* Y never defined, so /VOC's use of it fails */
    CHECK_INT(parse_with_voc_y(&f, &b), 1);
    CHECK_STR(diag_last_error(f.d), "\"Y\" is not defined");
    fx_close(&f);
}

TEST(ifndef_negates)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    VocEntry e;
    t(&b, T_IFNDEF, "#ifndef");
    s(&b, "NOPE");
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "Y");
    n(&b, "3", 3);
    t(&b, T_ENDIF, "#endif");
    CHECK_INT(parse_with_voc_y(&f, &b), 0);
    CHECK_INT(voctree_lookup(sintactic_voctree(), f.a, "NORTH", VOC_VERB, &e), 1);
    CHECK_INT(e.value, 3);
    fx_close(&f);
}

TEST(else_of_false_ifdef_is_taken)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    VocEntry e;
    t(&b, T_IFDEF, "#ifdef");
    s(&b, "NOPE");
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "Y");
    n(&b, "1", 1);
    t(&b, T_ELSE, "#else");
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "Y");
    n(&b, "2", 2);
    t(&b, T_ENDIF, "#endif");
    CHECK_INT(parse_with_voc_y(&f, &b), 0);
    CHECK_INT(voctree_lookup(sintactic_voctree(), f.a, "NORTH", VOC_VERB, &e), 1);
    CHECK_INT(e.value, 2);
    fx_close(&f);
}

/* Discriminator: were the else-part executed, its duplicate #define Y
   would error - the pass proves the else part was skipped, and the
   value proves the true part ran. */
TEST(else_of_true_ifdef_is_skipped)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    VocEntry e;
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "X");
    n(&b, "1", 1);
    t(&b, T_IFDEF, "#ifdef");
    s(&b, "X");
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "Y");
    n(&b, "1", 1);
    t(&b, T_ELSE, "#else");
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "Y");
    n(&b, "2", 2);
    t(&b, T_ENDIF, "#endif");
    CHECK_INT(parse_with_voc_y(&f, &b), 0);
    CHECK_INT(voctree_lookup(sintactic_voctree(), f.a, "NORTH", VOC_VERB, &e), 1);
    CHECK_INT(e.value, 1);
    fx_close(&f);
}

/* SkipBlock's #endif REWIND: the skipped block contains a nested
   #ifdef/#endif pair. If the rewind were missing, the outer #endif
   would be consumed inside the skip and the parse would end with
   `1 #endif(s) missing.`. */
TEST(skipblock_rewind_keeps_nested_ifdefs_balanced)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_IFDEF, "#ifdef");
    s(&b, "NOPE");
    t(&b, T_IFDEF, "#ifdef");
    s(&b, "ALSO");
    t(&b, T_ENDIF, "#endif");
    t(&b, T_ENDIF, "#endif");
    minimal_program(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    fx_close(&f);
}

TEST(stray_endif_reported)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_ENDIF, "#endif");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "#endif without #ifdef/#ifndef");
    fx_close(&f);
}

TEST(stray_else_reported)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_ELSE, "#else");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "#else without #ifdef/#ifndef");
    fx_close(&f);
}

TEST(false_ifdef_without_endif_hits_eof)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_ctl_voc(&b);
    t(&b, T_IFDEF, "#ifdef");
    s(&b, "NOPE");
    t(&b, T_SECTION_STX, "/STX"); /* swallowed by the skip */
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Unexpected end of file. #ifdef/#ifndef couldn't find #endif");
    fx_close(&f);
}

/* The source literal ends in '.', so the wire shows two (19.40's
   family); diag_last_error returns the body as passed. */
TEST(unbalanced_ifdef_reported_after_end)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "X");
    n(&b, "1", 1);
    t(&b, T_IFDEF, "#ifdef");
    s(&b, "X");
    minimal_program(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "1 #endif(s) missing.");
    fx_close(&f);
}

TEST(ifdef_operand_must_be_string_with_betwween_typo)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_IFDEF, "#ifdef");
    t(&b, T_IDENTIFIER, "X");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Invalid #ifdef/#ifndef label, please include the label or "
              "expression in betwween quotes");
    fx_close(&f);
}

TEST(ifdef_at_eof_reported)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_IFDEF, "#ifdef");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Unexpected end of file just after #ifdef/#ifndef");
    fx_close(&f);
}

/* ---- #echo ---- */

TEST(echo_prints_unquoted_text_unconditionally)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    char buf[512];
    t(&b, T_ECHO, "#echo");
    s(&b, "hello there");
    minimal_program(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    fx_read(&f, buf, sizeof buf);
    CHECK(strstr(buf, "hello there\n") != NULL);
    fx_close(&f);
}

TEST(echo_requires_string)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_ECHO, "#echo");
    t(&b, T_IDENTIFIER, "bare");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Invalid string for #echo");
    fx_close(&f);
}

/* ---- #extern / #int / #sfx ---- */

/* Success line pinned live against drf.exe 2026-08-27:
   `#EXTERN' "ext.bin" processed.` - the apostrophe is defect 19.41. */
TEST(extern_composes_pipe_string_and_echoes)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    char path[512], want[600], buf[1024], expect[700];
    scratch_path(path, sizeof path, "t6_ext.bin");
    write_bytes(path, "X", 1);
    t(&b, T_EXTERN, "#extern");
    s(&b, path);
    t(&b, T_INT, "#int");
    s(&b, path);
    t(&b, T_SFX, "#sfx");
    s(&b, path);
    minimal_program(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT((long)ctlextern_count(sintactic_externs()), 3);
    snprintf(want, sizeof want, "%s|EXTERN", path);
    CHECK_STR(ctlextern_at(sintactic_externs(), 0), want);
    snprintf(want, sizeof want, "%s|INT", path);
    CHECK_STR(ctlextern_at(sintactic_externs(), 1), want);
    snprintf(want, sizeof want, "%s|SFX", path);
    CHECK_STR(ctlextern_at(sintactic_externs(), 2), want);
    fx_read(&f, buf, sizeof buf);
    snprintf(expect, sizeof expect, "#EXTERN' \"%s\" processed.\n", path);
    CHECK(strstr(buf, expect) != NULL);
    snprintf(expect, sizeof expect, "#SFX' \"%s\" processed.\n", path);
    CHECK(strstr(buf, expect) != NULL);
    remove(path);
    fx_close(&f);
}

TEST(extern_missing_file_reported)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_EXTERN, "#extern");
    s(&b, "no_such_t6.bin");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Extern file \"no_such_t6.bin\" not found");
    fx_close(&f);
}

TEST(extern_requires_quoted_string)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_EXTERN, "#extern");
    t(&b, T_IDENTIFIER, "bare");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Included extern file should be in between quotes");
    fx_close(&f);
}

/* Defect 19.26: getMaluvaFilename leaves the caller's FileName
   unchanged for any target but C64/CP4/PCW, so "MALUVA" on NEXTDAAD
   deterministically fails the existence check under its own name. */
TEST(extern_maluva_undefined_for_nextdaad_target)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_EXTERN, "#extern");
    s(&b, "MALUVA");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Extern file \"MALUVA\" not found");
    fx_close(&f);
}

/* ---- #classic / #debug ---- */

TEST(classic_and_debug_set_their_flags)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_CLASSIC, "#classic");
    t(&b, T_DEBUG, "#debug");
    minimal_program(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(sintactic_classic_mode(), 1);
    CHECK_INT(sintactic_debug_mode(), 1);
    fx_close(&f);
}

/* ---- /VOC ---- */

TEST(voc_word_truncates_to_five_chars)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    VocEntry e;
    head_ctl_voc(&b);
    t(&b, T_IDENTIFIER, "NORTHERN");
    n(&b, "2", 2);
    t(&b, T_IDENTIFIER, "verb");
    sections_stx_to_ltx(&b);
    world_close(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(voctree_lookup(sintactic_voctree(), f.a, "NORTH", VOC_VERB, &e), 1);
    CHECK_INT(e.value, 2);
    CHECK_INT((long)voctree_count(sintactic_voctree()), 1);
    fx_close(&f);
}

/* The duplicate pre-check reports the TRUNCATED word. */
TEST(voc_duplicate_word_reports_truncated_word)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_ctl_voc(&b);
    t(&b, T_IDENTIFIER, "NORTH");
    n(&b, "2", 2);
    t(&b, T_IDENTIFIER, "verb");
    t(&b, T_IDENTIFIER, "NORTHERN");
    n(&b, "3", 3);
    t(&b, T_IDENTIFIER, "noun");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Word \"NORTH\" already defined");
    fx_close(&f);
}

TEST(voc_invalid_type_keyword)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_ctl_voc(&b);
    t(&b, T_IDENTIFIER, "NORTH");
    n(&b, "2", 2);
    t(&b, T_IDENTIFIER, "blah");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "\"blah\" is not a valid vocabulary word type");
    fx_close(&f);
}

TEST(voc_value_must_be_number_or_identifier)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_ctl_voc(&b);
    t(&b, T_IDENTIFIER, "NORTH");
    s(&b, "2");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Number or Identifier expected");
    fx_close(&f);
}

TEST(voc_unknown_symbol_value)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_ctl_voc(&b);
    t(&b, T_IDENTIFIER, "NORTH");
    t(&b, T_IDENTIFIER, "FOO");
    t(&b, T_IDENTIFIER, "verb");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "\"FOO\" is not defined");
    fx_close(&f);
}

/* Defect 19.52's second cause surfaced through ParseNewWord's own
   error text: a user #define colliding with the auto _VOC_ symbol. */
TEST(voc_symbol_collision_reports_voc_message)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "_VOC_NORTH");
    n(&b, "1", 1);
    head_ctl_voc(&b);
    t(&b, T_IDENTIFIER, "NORTH");
    n(&b, "2", 2);
    t(&b, T_IDENTIFIER, "verb");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Vocabulary word already exists or \"_VOC_NORTH\" already defined");
    fx_close(&f);
}

/* ---- text tables ---- */

TEST(stx_mtx_ltx_collect_messages_and_counts)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    le(&b, 0);
    s(&b, "sys zero");
    le(&b, 1);
    s(&b, "sys one");
    t(&b, T_SECTION_MTX, "/MTX");
    le(&b, 0);
    s(&b, "msg zero");
    t(&b, T_SECTION_OTX, "/OTX");
    t(&b, T_SECTION_LTX, "/LTX");
    world_close(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(sintactic_messages()->stx_count, 2);
    CHECK_INT(sintactic_messages()->mtx_count, 1);
    CHECK_INT(sintactic_messages()->otx_count, 0);
    CHECK_INT(sintactic_messages()->ltx_count, 1);
    CHECK_STR(msglist_at(sintactic_messages()->stx, 1)->text, "sys one");
    CHECK_STR(msglist_at(sintactic_messages()->mtx, 0)->text, "msg zero");
    fx_close(&f);
}

TEST(message_numbers_must_be_consecutive)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    le(&b, 1);
    s(&b, "skipped zero");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Message/Locations/Object numbers must be consecutive");
    fx_close(&f);
}

TEST(message_number_255_is_too_high)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    long i;
    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    for (i = 0; i <= 255; i++) {
        le(&b, i);
        s(&b, "m");
    }
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Message number too high. Maximum message number is 254");
    fx_close(&f);
}

TEST(named_list_entry_resolves_through_symbols)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "FIRST");
    n(&b, "0", 0);
    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    le_name(&b, "FIRST");
    s(&b, "named");
    t(&b, T_SECTION_MTX, "/MTX");
    t(&b, T_SECTION_OTX, "/OTX");
    t(&b, T_SECTION_LTX, "/LTX");
    world_close(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_STR(msglist_at(sintactic_messages()->stx, 0)->text, "named");
    fx_close(&f);
}

TEST(named_list_entry_unknown_symbol)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    le_name(&b, "WHAT");
    s(&b, "x");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Invalid or unknown symbol \"WHAT\"");
    fx_close(&f);
}

TEST(message_entry_requires_string)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    le(&b, 0);
    n(&b, "5", 5);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "String between quotes expected");
    fx_close(&f);
}

/* LAST_OBJECT/NUM_OBJECTS and LAST_LOCATION/NUM_LOCATIONS symbols
   appear after their sections. */
TEST(section_symbols_defined_after_otx_and_ltx)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    long v = -99;
    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    t(&b, T_SECTION_MTX, "/MTX");
    t(&b, T_SECTION_OTX, "/OTX");
    t(&b, T_SECTION_LTX, "/LTX");
    world_close(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(symbols_lookup(sintactic_symbols(), f.a, "NUM_OBJECTS", &v), 1);
    CHECK_INT(v, 0);
    CHECK_INT(symbols_lookup(sintactic_symbols(), f.a, "LAST_OBJECT", &v), 1);
    CHECK_INT(v, -1);
    CHECK_INT(symbols_lookup(sintactic_symbols(), f.a, "NUM_LOCATIONS", &v), 1);
    CHECK_INT(v, 1);
    CHECK_INT(symbols_lookup(sintactic_symbols(), f.a, "LAST_LOCATION", &v), 1);
    CHECK_INT(v, 0);
    fx_close(&f);
}

/* ---- /CON ---- */

/* two locations; helper up to /CON */
static void head_two_locations(TB *b)
{
    head_ctl_voc(b);
    t(b, T_IDENTIFIER, "NORTH");
    n(b, "2", 2);
    t(b, T_IDENTIFIER, "verb");
    t(b, T_SECTION_STX, "/STX");
    t(b, T_SECTION_MTX, "/MTX");
    t(b, T_SECTION_OTX, "/OTX");
    t(b, T_SECTION_LTX, "/LTX");
    le(b, 0);
    s(b, "room zero");
    le(b, 1);
    s(b, "room one");
    t(b, T_SECTION_CON, "/CON");
}

static void tail_from_obj(TB *b)
{
    t(b, T_SECTION_OBJ, "/OBJ");
    t(b, T_SECTION_PRO, "/PRO");
    n(b, "0", 0);
    t(b, T_SECTION_END, "/END");
}

TEST(connection_recorded_with_direction_value)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    const ConnectionRecord *c;
    head_two_locations(&b);
    le(&b, 0);
    t(&b, T_IDENTIFIER, "NORTH");
    n(&b, "1", 1);
    le(&b, 1);
    tail_from_obj(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT((long)connectionlist_count(sintactic_connections()), 1);
    c = connectionlist_at(sintactic_connections(), 0);
    CHECK_INT(c->from_loc, 0);
    CHECK_INT(c->to_loc, 1);
    CHECK_INT(c->direction, 2);
    fx_close(&f);
}

TEST(connection_exact_duplicate_rejected)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_two_locations(&b);
    le(&b, 0);
    t(&b, T_IDENTIFIER, "NORTH");
    n(&b, "1", 1);
    t(&b, T_IDENTIFIER, "NORTH");
    n(&b, "1", 1);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Connection already defined");
    fx_close(&f);
}

/* Defect 19.35: same direction to a DIFFERENT target is accepted. */
TEST(connection_same_direction_different_target_accepted)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_two_locations(&b);
    le(&b, 0);
    t(&b, T_IDENTIFIER, "NORTH");
    n(&b, "1", 1);
    t(&b, T_IDENTIFIER, "NORTH");
    n(&b, "0", 0);
    le(&b, 1);
    tail_from_obj(&b);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT((long)connectionlist_count(sintactic_connections()), 2);
    fx_close(&f);
}

/* Note the missing space after the colon - the reference's text. */
TEST(connection_direction_undefined)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_two_locations(&b);
    le(&b, 0);
    t(&b, T_IDENTIFIER, "EAST");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Direction is not defined:\"EAST\"");
    fx_close(&f);
}

TEST(connections_missing_for_second_location)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_two_locations(&b);
    le(&b, 0);
    t(&b, T_SECTION_OBJ, "/OBJ"); /* ends loc 0's block AND the section */
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Connections for location #1 missing");
    fx_close(&f);
}

TEST(connection_blocks_must_be_sequential)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_two_locations(&b);
    le(&b, 1);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Connections for location #0 expected but location #1 found");
    fx_close(&f);
}

TEST(connection_location_beyond_ltx_not_defined)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_ctl_voc(&b);
    sections_stx_to_ltx(&b);
    le(&b, 0);
    s(&b, "room");
    t(&b, T_SECTION_CON, "/CON");
    le(&b, 0);
    le(&b, 1); /* only 1 location defined */
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Location 1 is not defined");
    fx_close(&f);
}

/* ---- /OBJ ---- */

/* one location, one object description, NOUN+ADJECTIVE vocabulary */
static void head_one_object(TB *b)
{
    head_ctl_voc(b);
    t(b, T_IDENTIFIER, "DOOR");
    n(b, "50", 50);
    t(b, T_IDENTIFIER, "noun");
    t(b, T_IDENTIFIER, "RED");
    n(b, "60", 60);
    t(b, T_IDENTIFIER, "adjective");
    t(b, T_SECTION_STX, "/STX");
    t(b, T_SECTION_MTX, "/MTX");
    t(b, T_SECTION_OTX, "/OTX");
    le(b, 0);
    s(b, "an object");
    t(b, T_SECTION_LTX, "/LTX");
    le(b, 0);
    s(b, "room");
    t(b, T_SECTION_CON, "/CON");
    le(b, 0);
    t(b, T_SECTION_OBJ, "/OBJ");
}

/* 16 custom flag tokens: first column is bit 15 (MSB-first). */
static void add_flags(TB *b, const char *first, const char *rest15)
{
    int i;
    t(b, T_IDENTIFIER, first);
    for (i = 0; i < 15; i++) t(b, T_IDENTIFIER, rest15);
}

TEST(object_full_record_msb_first_flags)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    const ObjectRecord *o;
    head_one_object(&b);
    le(&b, 0);
    n(&b, "0", 0);   /* initially at location 0 */
    n(&b, "10", 10); /* weight */
    t(&b, T_IDENTIFIER, "Y"); /* container */
    t(&b, T_IDENTIFIER, "n"); /* wearable, case-insensitive */
    add_flags(&b, "Y", "N");  /* bit 15 set only */
    t(&b, T_IDENTIFIER, "DOOR");
    t(&b, T_IDENTIFIER, "RED");
    t(&b, T_SECTION_PRO, "/PRO");
    n(&b, "0", 0);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT((long)objectlist_count(sintactic_objects()), 1);
    o = objectlist_at(sintactic_objects(), 0);
    CHECK_INT(o->initially_at, 0);
    CHECK_INT(o->weight, 10);
    CHECK_INT(o->container, 1);
    CHECK_INT(o->wearable, 0);
    CHECK_INT(o->flags, 0x8000);
    CHECK_INT(o->noun, 50);
    CHECK_INT(o->adjective, 60);
    fx_close(&f);
}

/* underscore initial location = NOT_CREATED (252); CARRIED symbol
   injected before the parse (the drf.pas built-in path Task 9 wires)
   resolves to 254 and bumps NUM_CARRIED. */
TEST(object_underscore_and_carried_locations)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    long v = -1;
    CHECK_INT(symbols_add(sintactic_symbols(), f.a, f.d, "CARRIED",
                           LOC_CARRIED), 1);
    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    t(&b, T_SECTION_MTX, "/MTX");
    t(&b, T_SECTION_OTX, "/OTX");
    le(&b, 0);
    s(&b, "obj zero");
    le(&b, 1);
    s(&b, "obj one");
    t(&b, T_SECTION_LTX, "/LTX");
    le(&b, 0);
    s(&b, "room");
    t(&b, T_SECTION_CON, "/CON");
    le(&b, 0);
    t(&b, T_SECTION_OBJ, "/OBJ");
    le(&b, 0);
    t(&b, T_UNDERSCORE, "_");
    n(&b, "1", 1);
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    add_flags(&b, "N", "N");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    le(&b, 1);
    t(&b, T_IDENTIFIER, "CARRIED");
    n(&b, "1", 1);
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    add_flags(&b, "N", "N");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_SECTION_PRO, "/PRO");
    n(&b, "0", 0);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(objectlist_at(sintactic_objects(), 0)->initially_at,
              LOC_NOT_CREATED);
    CHECK_INT(objectlist_at(sintactic_objects(), 1)->initially_at,
              LOC_CARRIED);
    CHECK_INT(objectlist_at(sintactic_objects(), 0)->noun, NO_WORD);
    CHECK_INT(symbols_lookup(sintactic_symbols(), f.a, "NUM_CARRIED", &v), 1);
    CHECK_INT(v, 1);
    fx_close(&f);
}

TEST(object_definition_missing)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_one_object(&b);
    t(&b, T_SECTION_PRO, "/PRO"); /* zero definitions for one OTX entry */
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Definition for object #0 missing");
    fx_close(&f);
}

TEST(object_container_flag_must_be_ynu)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_one_object(&b);
    le(&b, 0);
    n(&b, "0", 0);
    n(&b, "10", 10);
    t(&b, T_IDENTIFIER, "Q");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "\"Y\", \"N\" or \"_\" expected at container flag");
    fx_close(&f);
}

/* No space before the number - the reference's own text. 255 (HERE)
   is not an accepted initial location. */
TEST(object_invalid_initial_location_text)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_one_object(&b);
    le(&b, 0);
    n(&b, "255", 255);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Invalid initial location255");
    fx_close(&f);
}

/* Space before the colon - the reference's own text. */
TEST(object_weight_over_63_rejected)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_one_object(&b);
    le(&b, 0);
    n(&b, "0", 0);
    n(&b, "64", 64);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Invalid weight :64");
    fx_close(&f);
}

/* ---- /PRO skeleton ---- */

/* head through /PRO 0 with one location and no objects */
static void head_pro0(TB *b)
{
    head_ctl_voc(b);
    sections_stx_to_ltx(b);
    le(b, 0);
    s(b, "room");
    t(b, T_SECTION_CON, "/CON");
    le(b, 0);
    t(b, T_SECTION_OBJ, "/OBJ");
    t(b, T_SECTION_PRO, "/PRO");
    n(b, "0", 0);
}

TEST(pro_number_expected)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_ctl_voc(&b);
    sections_stx_to_ltx(&b);
    le(&b, 0);
    s(&b, "room");
    t(&b, T_SECTION_CON, "/CON");
    le(&b, 0);
    t(&b, T_SECTION_OBJ, "/OBJ");
    t(&b, T_SECTION_PRO, "/PRO");
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Process number expected but \">\" found");
    fx_close(&f);
}

/* Two spaces between `expected` and `but` - the reference's own. */
TEST(entry_sign_expected_double_space)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    t(&b, T_IDENTIFIER, "NORTH");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Label or entry sign \">\" expected  but \"NORTH\" found");
    fx_close(&f);
}

TEST(label_recorded_at_entry_start)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    LabelData ld;
    head_pro0(&b);
    t(&b, T_LABEL, "$start");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(labels_find(sintactic_labels(), "$start", &ld), 1);
    CHECK_INT(ld.process, 0);
    CHECK_INT(ld.entry, 0);
    CHECK_INT(ld.is_forward, 0);
    fx_close(&f);
}

TEST(label_duplicate_rejected)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    t(&b, T_LABEL, "$dup");
    t(&b, T_LABEL, "$dup");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Label already defined ($dup) or too many labels");
    fx_close(&f);
}

/* Synonym headers: one condact list SHARED by every copy (analysis
   24 / defect 19.42's substrate). */
TEST(synonym_entries_share_one_condact_list)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    const ProcessSlot *slot;
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_DB, "#db");
    n(&b, "1", 1);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    slot = processtable_get(sintactic_processes(), 0);
    CHECK_INT((long)vec_len_ProcessEntry(slot->entries), 2);
    CHECK(vec_at_ProcessEntry(slot->entries, 0)->condacts ==
          vec_at_ProcessEntry(slot->entries, 1)->condacts);
    CHECK_INT(vec_at_ProcessEntry(slot->entries, 0)->verb, NO_WORD);
    fx_close(&f);
}

TEST(unknown_condact_reported)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_IDENTIFIER, "FROB");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Unknown condact: \"FROB\"");
    fx_close(&f);
}

/* Task 7 landed the condact branch: a minimal entry with a resolved
   condact + literal parameter and a zero-parameter terminator.
   (Replaces Task 6's real_condact_defers_to_task7 deferral gate.) */
TEST(condact_with_literal_param_queued)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_IDENTIFIER, "AT");
    n(&b, "0", 0);
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK(cl != NULL);
    CHECK_INT((long)vec_len_ProcessCondact(cl), 2);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, 0); /* AT */
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->num_params, 1);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].indirection, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->opcode, 22); /* DONE */
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->num_params, 0);
    fx_close(&f);
}

TEST(pro_redefinition_warns_and_concatenates)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    const ProcessSlot *slot;
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_DB, "#db");
    n(&b, "1", 1);
    t(&b, T_SECTION_PRO, "/PRO");
    n(&b, "0", 0);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_DB, "#db");
    n(&b, "2", 2);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(diag_warn_count(f.d), 1);
    CHECK_STR(diag_last(f.d),
              "Process #0 already defined, concatenating entries");
    slot = processtable_get(sintactic_processes(), 0);
    CHECK_INT((long)vec_len_ProcessEntry(slot->entries), 2);
    fx_close(&f);
}

/* ---- #userptr / #db / #dw / #hex / #incbin ---- */

TEST(userptr_queues_fake_condact)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    const ProcessCondact *c;
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_USERPTR, "#userptr");
    n(&b, "3", 3);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK(cl != NULL);
    CHECK_INT((long)vec_len_ProcessCondact(cl), 1);
    c = vec_at_ProcessCondact(cl, 0);
    CHECK_INT(c->opcode, FAKE_USERPTR_CONDACT_CODE);
    CHECK_INT(c->num_params, 1);
    CHECK_INT(c->params[0].value, 3);
    CHECK_INT(c->is_db, 0);
    fx_close(&f);
}

TEST(userptr_range_and_type_checks)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_USERPTR, "#userptr");
    n(&b, "12", 12);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "#userptr parameter should be 0-9");
    fx_close(&f);
}

TEST(userptr_requires_number)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_USERPTR, "#userptr");
    t(&b, T_IDENTIFIER, "X");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "#userptr parameter should be numeric");
    fx_close(&f);
}

/* THE 19.22 pin: `#define SYM 5` then `#db SYM` queues the UNTOUCHED
   sentinel 2147483647 as the byte, NOT 5 - the range check ran on
   ExtractValue's 5, but the queued value is CurrentIntVal, still the
   lexer's MaxLongInt filler for an identifier token (live-verified
   against drf.exe per the catalogue entry: `"Opcode":2147483647` in
   the JSON while the verbose line shows the correct value). */
TEST(db_symbol_operand_queues_sentinel_not_value)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    char buf[2048];
    diag_set_verbose(f.d, 1);
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "SYM");
    n(&b, "5", 5);
    head_ctl_voc(&b);
    sections_stx_to_ltx(&b);
    le(&b, 0);
    s(&b, "room");
    t(&b, T_SECTION_CON, "/CON");
    le(&b, 0);
    t(&b, T_SECTION_OBJ, "/OBJ");
    t(&b, T_SECTION_PRO, "/PRO");
    n(&b, "0", 0);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_DB, "#db");
    t(&b, T_IDENTIFIER, "SYM");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK(cl != NULL);
    CHECK_INT((long)vec_len_ProcessCondact(cl), 1);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, 2147483647L);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->is_db, 1);
    /* ...while the verbose confirmation shows the CORRECT value. */
    fx_read(&f, buf, sizeof buf);
    CHECK(strstr(buf, "#DB SYM(5) processed\n") != NULL);
    fx_close(&f);
}

TEST(db_literal_queues_its_value)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_DB, "#db");
    n(&b, "7", 7);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK(cl != NULL);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, 7);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->is_db, 1);
    fx_close(&f);
}

TEST(db_range_checked_on_extracted_value)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_DB, "#db");
    n(&b, "300", 300);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "DB value should be between 0 and 255");
    fx_close(&f);
}

/* ExtractValue halts internally on an unknown value (which is what
   makes the `#DB Unknown value` guard dead code, 19.22); the source
   literal's own trailing period gives a double period on the wire. */
TEST(db_unknown_symbol_uses_extract_value_text)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_DB, "#db");
    t(&b, T_IDENTIFIER, "FOO");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "\"FOO\" is not defined. Check DB/DW value.");
    fx_close(&f);
}

/* 19.22's #dw face: a symbol operand queues 0xFF,0xFF (the sentinel's
   low then high byte), not the symbol's value. */
TEST(dw_symbol_operand_queues_ff_ff)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "SYM");
    n(&b, "5", 5);
    head_ctl_voc(&b);
    sections_stx_to_ltx(&b);
    le(&b, 0);
    s(&b, "room");
    t(&b, T_SECTION_CON, "/CON");
    le(&b, 0);
    t(&b, T_SECTION_OBJ, "/OBJ");
    t(&b, T_SECTION_PRO, "/PRO");
    n(&b, "0", 0);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_DW, "#dw");
    t(&b, T_IDENTIFIER, "SYM");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK(cl != NULL);
    CHECK_INT((long)vec_len_ProcessCondact(cl), 2);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, 0xFF);
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->opcode, 0xFF);
    fx_close(&f);
}

TEST(dw_literal_queues_low_then_high)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_DW, "#dw");
    n(&b, "4660", 4660); /* 0x1234 */
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK(cl != NULL);
    CHECK_INT((long)vec_len_ProcessCondact(cl), 2);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, 0x34);
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->opcode, 0x12);
    fx_close(&f);
}

TEST(hex_queues_bytes_case_insensitive)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_HEX, "#hex");
    s(&b, "0AfF");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK(cl != NULL);
    CHECK_INT((long)vec_len_ProcessCondact(cl), 2);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, 0x0A);
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->opcode, 0xFF);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->is_db, 1);
    fx_close(&f);
}

/* The parity check runs on the FULL quoted text (delimiters
   included): "ABC" is 5 bytes with quotes - odd - and errors. */
TEST(hex_odd_length_rejected)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_HEX, "#hex");
    s(&b, "ABC");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Invalid hexadecimal string");
    fx_close(&f);
}

TEST(hex_requires_string)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_HEX, "#hex");
    n(&b, "10", 10);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "HEX parameter should in between quotes");
    fx_close(&f);
}

/* 19.34's guard: the reference crashes with an uncaught EConvertError
   on a non-hex digit; the port fatals (class 2) instead - PORT NOTE
   in sintactic.c. */
TEST(hex_invalid_digit_fatals)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_HEX, "#hex");
    s(&b, "ZZ");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 2);
    fx_close(&f);
}

TEST(incbin_queues_file_bytes)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    char path[512];
    static const unsigned char payload[3] = { 0x01, 0x00, 0xFE };
    scratch_path(path, sizeof path, "t6_incbin.bin");
    write_bytes(path, payload, sizeof payload);
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_INCBIN, "#incbin");
    s(&b, path);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK(cl != NULL);
    CHECK_INT((long)vec_len_ProcessCondact(cl), 3);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, 0x01);
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->opcode, 0x00);
    CHECK_INT(vec_at_ProcessCondact(cl, 2)->opcode, 0xFE);
    remove(path);
    fx_close(&f);
}

TEST(incbin_missing_file_reported)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_INCBIN, "#incbin");
    s(&b, "no_such_t6.bin");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Included file \"no_such_t6.bin\" not found");
    fx_close(&f);
}

/* 19.44's pinned guard: an entry STARTING with #db (the loop-control
   variable's uninitialised read in the reference) deterministically
   keeps parsing the entry in this port. */
TEST(entry_starting_with_db_keeps_parsing)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_DB, "#db");
    n(&b, "1", 1);
    t(&b, T_DB, "#db");
    n(&b, "2", 2);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK(cl != NULL);
    CHECK_INT((long)vec_len_ProcessCondact(cl), 2);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, 1);
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->opcode, 2);
    fx_close(&f);
}

/* ---- Task 7: condact parameters, labels, semantics ---- */

/* `> _ _` - an entry header with no words */
static void entry_uu(TB *b)
{
    t(b, T_PROCESS_ENTRY_SIGN, ">");
    t(b, T_UNDERSCORE, "_");
    t(b, T_UNDERSCORE, "_");
}

/* head through /PRO 0 with one location, no objects, and a /VOC
   carrying the words Task 7's parameter vectors need: a convertible
   noun (value <= 39), a non-convertible noun, a verb and a
   preposition. */
static void head_pro0_vocab(TB *b)
{
    head_ctl_voc(b);
    t(b, T_IDENTIFIER, "GO");
    n(b, "20", 20);
    t(b, T_IDENTIFIER, "noun"); /* convertible: 20 <= 39 */
    t(b, T_IDENTIFIER, "DOOR");
    n(b, "50", 50);
    t(b, T_IDENTIFIER, "noun");
    t(b, T_IDENTIFIER, "LOOK");
    n(b, "30", 30);
    t(b, T_IDENTIFIER, "verb");
    t(b, T_IDENTIFIER, "WITH");
    n(b, "40", 40);
    t(b, T_IDENTIFIER, "preposition");
    sections_stx_to_ltx(b);
    le(b, 0);
    s(b, "room");
    t(b, T_SECTION_CON, "/CON");
    le(b, 0);
    t(b, T_SECTION_OBJ, "/OBJ");
    t(b, T_SECTION_PRO, "/PRO");
    n(b, "0", 0);
}

TEST(debug_condact_queues_opcode_220)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "DEBUG");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK(cl != NULL);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, FAKE_DEBUG_CONDACT_CODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->num_params, 0);
    fx_close(&f);
}

/* ---- parameter resolution precedence (analysis 24.4) ---- */

TEST(param_symbol_resolves_with_fold)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    CHECK_INT(symbols_add(sintactic_symbols(), f.a, f.d, "FLAGX", 5), 1);
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "ZERO");
    t(&b, T_IDENTIFIER, "flagx"); /* folded lookup */
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, 11); /* ZERO */
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 5);
    fx_close(&f);
}

/* For the six word-condacts, vocabulary shadows symbols. */
TEST(word_condact_vocab_shadows_symbol)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    CHECK_INT(symbols_add(sintactic_symbols(), f.a, f.d, "DOOR", 7), 1);
    head_pro0_vocab(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "NOUN2");
    t(&b, T_IDENTIFIER, "DOOR");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, NOUN2_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 50);
    fx_close(&f);
}

/* For every other condact, symbols shadow vocabulary. */
TEST(plain_condact_symbol_shadows_vocab)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    CHECK_INT(symbols_add(sintactic_symbols(), f.a, f.d, "DOOR", 7), 1);
    head_pro0_vocab(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "ZERO");
    t(&b, T_IDENTIFIER, "DOOR");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 7);
    fx_close(&f);
}

/* Neither typed vocab nor symbol: the untyped VOC_ANY retry. */
TEST(param_untyped_vocab_fallback)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0_vocab(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "ZERO");
    t(&b, T_IDENTIFIER, "LOOK"); /* verb 30, no symbol */
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 30);
    fx_close(&f);
}

TEST(param_underscore_is_no_word)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "SYNONYM");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, SYNONYM_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, NO_WORD);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[1].value, NO_WORD);
    fx_close(&f);
}

TEST(param_unresolved_reports_number_and_name)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "ZERO");
    t(&b, T_IDENTIFIER, "BLAH");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Invalid parameter #1: \"BLAH\" for condact ZERO");
    fx_close(&f);
}

TEST(param_over_255_rejected)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "AT");
    n(&b, "300", 300);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Invalid parameter value \"300\" for condact AT");
    fx_close(&f);
}

TEST(param_bad_token_invalid_condact_parameter)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "AT");
    t(&b, T_LABEL, "$notskip"); /* T_LABEL on a non-SKIP condact */
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Invalid condact parameter");
    fx_close(&f);
}

/* SYNONYM's verb slot: typed lookup falls back to a CONVERTIBLE noun
   (value <= 39). */
TEST(synonym_verb_slot_accepts_convertible_noun)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0_vocab(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "SYNONYM");
    t(&b, T_IDENTIFIER, "GO"); /* noun 20 - convertible */
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 20);
    fx_close(&f);
}

/* A NON-convertible noun is discarded by the TYPED fallback but still
   resolves through the final UNTYPED retry (VOC_ANY), and the
   semantic vocabularyVerb arm's own noun retry has no convertibility
   test either - so it compiles, value = the noun's number. */
TEST(synonym_nonconvertible_noun_resolves_via_untyped_retry)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0_vocab(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "SYNONYM");
    t(&b, T_IDENTIFIER, "DOOR"); /* noun 50 - NOT convertible */
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 50);
    fx_close(&f);
}

/* ---- indirection ---- */

TEST(indirection_on_first_param)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "EQ");
    t(&b, T_INDIRECT, "@");
    n(&b, "5", 5);
    n(&b, "3", 3);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 5);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].indirection, 1);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[1].value, 3);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[1].indirection, 0);
    fx_close(&f);
}

TEST(indirection_on_second_param_rejected_without_v3)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "EQ");
    n(&b, "5", 5);
    t(&b, T_INDIRECT, "@");
    n(&b, "3", 3);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Indirection is not allowed in this parameter");
    fx_close(&f);
}

TEST(v3_allows_indirection_on_second_param)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    f.opts.v3 = 1;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "EQ");
    n(&b, "5", 5);
    t(&b, T_INDIRECT, "@");
    n(&b, "3", 3);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[1].indirection, 1);
    fx_close(&f);
}

/* An indirected parameter skips the semantic check. */
TEST(indirection_skips_semantic_check)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b); /* ltx_count = 1 */
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "AT");
    t(&b, T_INDIRECT, "@");
    n(&b, "5", 5); /* location 5 does not exist - but indirected */
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(diag_warn_count(f.d), 0);
    fx_close(&f);
}

/* ---- inline messages / auto-numbering ---- */

TEST(inline_message_auto_numbers_and_dedups)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "MESSAGE");
    s(&b, "hi");
    t(&b, T_IDENTIFIER, "MESSAGE");
    s(&b, "bye");
    t(&b, T_IDENTIFIER, "MESSAGE");
    s(&b, "hi"); /* dedup: same id as the first */
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT((long)vec_len_ProcessCondact(cl), 3);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, MESSAGE_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->params[0].value, 1);
    CHECK_INT(vec_at_ProcessCondact(cl, 2)->params[0].value, 0);
    CHECK_INT((long)msglist_count(sintactic_messages()->mtx), 2);
    /* the frozen SECTION count is untouched by /PRO inserts */
    CHECK_INT(sintactic_messages()->mtx_count, 0);
    fx_close(&f);
}

TEST(inline_sysmess_goes_to_stx)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "SYSMESS");
    s(&b, "boo");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, SYSMESS_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 0);
    CHECK_STR(msglist_at(sintactic_messages()->stx, 0)->text, "boo");
    fx_close(&f);
}

TEST(xmessage_appends_hash_n_and_becomes_xmes)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "XMESSAGE");
    s(&b, "hi");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, XMES_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 0);
    CHECK_STR(msglist_at(sintactic_messages()->xtx, 0)->text, "hi#n");
    fx_close(&f);
}

TEST(xmes_stores_text_without_append)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "XMES");
    s(&b, "hi");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, XMES_OPCODE);
    CHECK_STR(msglist_at(sintactic_messages()->xtx, 0)->text, "hi");
    fx_close(&f);
}

/* The 511 check runs BEFORE the #n append: 512 is rejected, 511
   passes and stores 513 characters. Source text's own period gives
   the double period on the wire (19.40 family). */
TEST(extended_message_length_checks)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    char big[513];
    memset(big, 'a', 512);
    big[512] = '\0';
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "XMES");
    s(&b, big);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Extended messages can be only up to 511 characters long. "
              "Your message is 512 long.");
    fx_close(&f);
}

TEST(xmessage_511_chars_stores_513)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    char big[512];
    memset(big, 'a', 511);
    big[511] = '\0';
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "XMESSAGE");
    s(&b, big);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT((long)strlen(msglist_at(sintactic_messages()->xtx, 0)->text),
              513);
    fx_close(&f);
}

TEST(force_x_messages_promotes_mes_and_message)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    f.opts.force_x_messages = 1;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "MES");
    s(&b, "one");
    t(&b, T_IDENTIFIER, "MESSAGE");
    s(&b, "two");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, XMES_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->opcode, XMES_OPCODE);
    /* MES stays bare; MESSAGE went via XMESSAGE and gained #n */
    CHECK_STR(msglist_at(sintactic_messages()->xtx, 0)->text, "one");
    CHECK_STR(msglist_at(sintactic_messages()->xtx, 1)->text, "two#n");
    CHECK_INT((long)msglist_count(sintactic_messages()->mtx), 0);
    fx_close(&f);
}

TEST(force_normal_messages_demotes_xmes_and_xmessage)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    f.opts.force_normal_messages = 1;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "XMES");
    s(&b, "one");
    t(&b, T_IDENTIFIER, "XMESSAGE");
    s(&b, "two");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, MES_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->opcode, MESSAGE_OPCODE);
    CHECK_STR(msglist_at(sintactic_messages()->mtx, 0)->text, "one");
    CHECK_STR(msglist_at(sintactic_messages()->mtx, 1)->text, "two");
    CHECK_INT((long)msglist_count(sintactic_messages()->xtx), 0);
    fx_close(&f);
}

TEST(xplay_and_xdata_go_to_other_tx)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "XPLAY");
    s(&b, "tune");
    t(&b, T_IDENTIFIER, "XDATA");
    s(&b, "blob");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, XPLAY_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->opcode, XDATA_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->params[0].value, 1);
    CHECK_STR(msglist_at(sintactic_messages()->other_tx, 0)->text, "tune");
    CHECK_STR(msglist_at(sintactic_messages()->other_tx, 1)->text, "blob");
    fx_close(&f);
}

/* An XMES-family NUMERIC parameter over 255 passes the range check
   (only Value < 0 is rejected) but the reference then RANGE-FAULTS at
   the SemanticCheck call - {$R+} traps the Longint->Byte conversion
   (live: `ERangeError: Range check error`, exit 217, no JSON) -
   identically under -semantic-warnings, since the trap fires before
   any downgrade. PORT-NOTEd fatal guard (abort-parity, 19.36/19.34
   precedent). */
TEST(xmes_numeric_param_over_255_fatals_with_semantics)
{
    int sw;
    for (sw = 0; sw < 2; sw++) {
        Fx f = fx_open();
        TB b = tb_new(f.a);
        f.opts.semantic_warnings = sw;
        head_pro0(&b);
        entry_uu(&b);
        t(&b, T_IDENTIFIER, "XMES");
        n(&b, "300", 300);
        t(&b, T_SECTION_END, "/END");
        CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 2);
        CHECK(strstr(diag_last_error(f.d), "ERangeError") != NULL);
        fx_close(&f);
    }
}

/* -no-semantic: the SemanticCheck call never happens, the reference
   compiles clean and the raw value reaches the output untruncated
   (live JSON: "Param1":300) - reproduced. */
TEST(xmes_numeric_param_over_255_allowed_no_semantic)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    f.opts.no_semantic = 1;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "XMES");
    n(&b, "300", 300);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 300);
    fx_close(&f);
}

/* head with a FULL /MTX (255 entries) plus `extra_stx`/`extra_ltx`
   fully-populated tables, for the overflow-cascade decision vectors. */
static void head_pro0_full_tables(TB *b, int fill_stx, int fill_ltx)
{
    char id[32], txt[32];
    long i;
    long nltx = fill_ltx ? 255 : 1;
    head_ctl_voc(b);
    t(b, T_SECTION_STX, "/STX");
    if (fill_stx) {
        for (i = 0; i < 255; i++) {
            snprintf(id, sizeof id, "%ld", i);
            snprintf(txt, sizeof txt, "s%ld", i);
            le(b, i);
            s(b, txt);
        }
    }
    t(b, T_SECTION_MTX, "/MTX");
    for (i = 0; i < 255; i++) {
        snprintf(txt, sizeof txt, "m%ld", i);
        le(b, i);
        s(b, txt);
    }
    t(b, T_SECTION_OTX, "/OTX");
    t(b, T_SECTION_LTX, "/LTX");
    for (i = 0; i < nltx; i++) {
        snprintf(txt, sizeof txt, "l%ld", i);
        le(b, i);
        s(b, txt);
    }
    t(b, T_SECTION_CON, "/CON");
    for (i = 0; i < nltx; i++) le(b, i);
    t(b, T_SECTION_OBJ, "/OBJ");
    t(b, T_SECTION_PRO, "/PRO");
    n(b, "0", 0);
    (void)id;
}

/* Defect 19.51's decision site: MTX full, non-classic - MESSAGE
   retries into STX with a literal two-byte "\n" appended and the
   condact becomes SYSMESS. */
TEST(message_overflow_cascades_to_sysmess)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0_full_tables(&b, 0, 0);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "MESSAGE");
    s(&b, "fresh");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, SYSMESS_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 0);
    CHECK_STR(msglist_at(sintactic_messages()->stx, 0)->text, "fresh\\n");
    fx_close(&f);
}

TEST(message_overflow_classic_mode_hard_error)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    t(&b, T_CLASSIC, "#classic");
    head_pro0_full_tables(&b, 0, 0);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "MESSAGE");
    s(&b, "fresh");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Too many messages, max messages per message table is 255");
    fx_close(&f);
}

/* Both MTX and STX full: the text (with its "\n") falls through to
   LTX and the condact becomes DESC - 19.51's silent rewrite. */
TEST(message_overflow_both_full_becomes_desc)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0_full_tables(&b, 1, 0); /* LTX has 1 entry - room left */
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "MESSAGE");
    s(&b, "fresh");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, DESC_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 1);
    CHECK_STR(msglist_at(sintactic_messages()->ltx, 1)->text, "fresh\\n");
    fx_close(&f);
}

/* All three tables full: the LTX fall-through fails too and the
   non-classic capacity error fires (765 = 3 * 255). */
TEST(message_overflow_all_tables_full_errors)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0_full_tables(&b, 1, 1);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "MESSAGE");
    s(&b, "fresh");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Too many messages, total messages in MTX, STX and LTX "
              "tables, plus \"MESSAGE\" strings is 765");
    fx_close(&f);
}

/* Analysis 25's closing note: the semantic counters are the frozen
   SECTION counts - a numeric reference to an auto-inserted inline
   message is a semantic error even though the message exists. */
TEST(mesno_semantic_ignores_auto_inserted_messages)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "MESSAGE");
    s(&b, "auto"); /* inserted into MTX at id 0 */
    t(&b, T_IDENTIFIER, "MESSAGE");
    n(&b, "0", 0); /* numeric reference to it */
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Message 0 does not exist");
    fx_close(&f);
}

/* ---- SKIP labels / PENDINGSKIP / FixForwardLabels ---- */

TEST(backward_label_skip_resolves_with_wrap)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    t(&b, T_LABEL, "$back"); /* entry 0 */
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "SKIP");
    t(&b, T_LABEL, "$back"); /* displacement 0-0-1 = -1, wraps to 255 */
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(sintactic_fix_forward_labels(), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, SKIP_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 255);
    fx_close(&f);
}

/* Defect 19.37: a literal numeric SKIP is merely wrapped - SKIP -200
   silently becomes SKIP 56. */
TEST(skip_literal_negative_wraps)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "SKIP");
    n(&b, "-200", -200);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 56);
    fx_close(&f);
}

TEST(forward_label_pendingskip_then_fixed)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    char buf[4096];
    diag_set_verbose(f.d, 1);
    head_pro0(&b);
    entry_uu(&b); /* entry 0 */
    t(&b, T_IDENTIFIER, "SKIP");
    t(&b, T_LABEL, "$fwd");
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_LABEL, "$fwd"); /* defined at entry 1 */
    entry_uu(&b);           /* entry 1 */
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, PENDINGSKIP_OPCODE);
    CHECK_INT(sintactic_fix_forward_labels(), 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, SKIP_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 0); /* 1-0-1 */
    fx_read(&f, buf, sizeof buf);
    CHECK(strstr(buf, "Forward declaration of label $fwd created.\n") != NULL);
    fx_close(&f);
}

TEST(forward_label_never_defined_reported)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "SKIP");
    t(&b, T_LABEL, "$never");
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(sintactic_fix_forward_labels(), 1);
    CHECK_STR(diag_last_error(f.d),
              "Label $never was referenced but then not defined");
    fx_close(&f);
}

TEST(backward_label_wrong_process_reported)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    t(&b, T_LABEL, "$lab");
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_SECTION_PRO, "/PRO");
    n(&b, "1", 1);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "SKIP");
    t(&b, T_LABEL, "$lab");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Label \"$lab\" is not in this process");
    fx_close(&f);
}

TEST(forward_label_other_process_reported)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "SKIP");
    t(&b, T_LABEL, "$x");
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_SECTION_PRO, "/PRO");
    n(&b, "1", 1);
    t(&b, T_LABEL, "$x"); /* defined in process 1 */
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(sintactic_fix_forward_labels(), 1);
    CHECK_STR(diag_last_error(f.d),
              "Label $x was referenced in one process but defined in a "
              "different process");
    fx_close(&f);
}

/* 129 entries back: displacement -130 < -128. */
TEST(backward_label_too_far_reported)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    int i;
    head_pro0(&b);
    t(&b, T_LABEL, "$far"); /* entry 0 */
    for (i = 0; i < 129; i++) {
        entry_uu(&b);
        t(&b, T_IDENTIFIER, "DONE");
    }
    entry_uu(&b); /* entry 129 */
    t(&b, T_IDENTIFIER, "SKIP");
    t(&b, T_LABEL, "$far");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Label \"$far\" is too far from SKIP call, maximum 128 "
              "entries far allowed");
    fx_close(&f);
}

/* Forward: Entry - EntryNo > 128 - "trys" is the reference's own. */
TEST(forward_label_too_far_reported)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    int i;
    head_pro0(&b);
    entry_uu(&b); /* entry 0 */
    t(&b, T_IDENTIFIER, "SKIP");
    t(&b, T_LABEL, "$fwd");
    t(&b, T_IDENTIFIER, "DONE");
    for (i = 0; i < 129; i++) { /* entries 1..129 */
        entry_uu(&b);
        t(&b, T_IDENTIFIER, "DONE");
    }
    t(&b, T_LABEL, "$fwd"); /* entry 130 */
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(sintactic_fix_forward_labels(), 1);
    CHECK_STR(diag_last_error(f.d),
              "SKIP using label $fwd trys to jump forward too much, "
              "maximum 128 entries jumped allowed");
    fx_close(&f);
}

/* Defect 19.43: only the FIRST PENDINGSKIP per entry is fixed; the
   second placeholder opcode (141) leaks into the output unchanged. */
TEST(only_first_pendingskip_fixed)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    entry_uu(&b); /* entry 0 */
    t(&b, T_IDENTIFIER, "SKIP");
    t(&b, T_LABEL, "$a");
    t(&b, T_IDENTIFIER, "SKIP");
    t(&b, T_LABEL, "$b");
    t(&b, T_LABEL, "$a"); /* condact-list label leg: entry 1 */
    t(&b, T_LABEL, "$b"); /* entries leg: entry 1 */
    entry_uu(&b);          /* entry 1 */
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(sintactic_fix_forward_labels(), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, SKIP_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->opcode, PENDINGSKIP_OPCODE);
    fx_close(&f);
}

/* THE 19.24 vector: a label after a 2-synonym entry's condacts is
   recorded at CurrentEntry+1 = 1 - the synonym's duplicate copy - not
   the real next entry (2). */
TEST(label_after_synonym_entry_records_wrong_entry)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    LabelData ld;
    head_pro0(&b);
    entry_uu(&b); /* synonym copies: entries 0 and 1 */
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_LABEL, "$lab");
    entry_uu(&b); /* the real next entry - number 2 */
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(labels_find(sintactic_labels(), "$lab", &ld), 1);
    CHECK_INT(ld.entry, 1); /* WRONG (should be 2) - reproduced */
    fx_close(&f);
}

/* Defect 19.42: synonym copies share ONE condact list, so the shared
   PENDINGSKIP is fixed ONCE with the FIRST copy's entry number - the
   second copy's displacement is off by one. */
TEST(synonym_shared_pendingskip_fixed_once)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    const ProcessSlot *slot;
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    entry_uu(&b); /* entries 0 and 1, shared list */
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "SKIP");
    t(&b, T_LABEL, "$t");
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_LABEL, "$t"); /* recorded at entry 1 (19.24) */
    entry_uu(&b);          /* entry 2 */
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(sintactic_fix_forward_labels(), 0);
    slot = processtable_get(sintactic_processes(), 0);
    CHECK(vec_at_ProcessEntry(slot->entries, 0)->condacts ==
          vec_at_ProcessEntry(slot->entries, 1)->condacts);
    cl = entry_condacts(0, 0);
    /* fixed once, with entry 0's displacement: 1 - 0 - 1 = 0; the
       second copy would need -1 (i.e. 255) - skewed, reproduced */
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, SKIP_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 0);
    fx_close(&f);
}

/* A #db-emitted byte equal to 141 (PENDINGSKIP's opcode) matches the
   fix pass's opcode-only test. With no labels defined, the reference
   reads the zeroed LabelList[0] global and deterministically patches
   the node to SKIP with displacement 0 - entryno - 1; reproduced via
   the zeroed-record guard (PORT NOTE in sintactic.c). */
TEST(db_141_byte_caught_by_fix_pass)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_DB, "#db");
    n(&b, "141", 141);
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(sintactic_fix_forward_labels(), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, SKIP_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, -1);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->is_db, 1);
    fx_close(&f);
}

/* ---- X-condact gates ---- */

/* Defect 19.27: without -replace-xcondacts, every X-condact compiles
   untouched. */
TEST(xpicture_without_switch_compiles_untouched)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "XPICTURE");
    n(&b, "3", 3);
    t(&b, T_IDENTIFIER, "XSAVE");
    n(&b, "0", 0);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->opcode, XPICTURE_OPCODE);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 3);
    CHECK_INT(vec_at_ProcessCondact(cl, 1)->opcode, XSAVE_OPCODE);
    CHECK_INT(sintactic_maluva_used(), 0);
    fx_close(&f);
}

/* Source text's own trailing period - double period on the wire. */
TEST(xpicture_replace_switch_wrong_target)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    f.opts.replace_xcondacts = 1;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "XPICTURE");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "XPICTURE cannot be used in this target [NEXTDAAD].");
    fx_close(&f);
}

TEST(xpicture_replace_switch_msx_sets_maluva)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    f.opts.replace_xcondacts = 1;
    f.opts.target = "MSX";
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "XPICTURE");
    n(&b, "3", 3);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(sintactic_maluva_used(), 1);
    fx_close(&f);
}

TEST(xsave_xload_xbeep_deprecated_with_switch)
{
    static const struct { const char *name; const char *msg; } cases[3] = {
        {"XSAVE", "XSAVE has been deprecated, use SAVE instead."},
        {"XLOAD", "XLOAD has been deprecated, use LOAD instead."},
        {"XBEEP", "XBEEP has been deprecated, use BEEP instead."},
    };
    int i;
    for (i = 0; i < 3; i++) {
        Fx f = fx_open();
        TB b = tb_new(f.a);
        f.opts.replace_xcondacts = 1;
        head_pro0(&b);
        entry_uu(&b);
        t(&b, T_IDENTIFIER, cases[i].name);
        CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
        CHECK_STR(diag_last_error(f.d), cases[i].msg);
        fx_close(&f);
    }
}

/* ---- semantic wiring (analysis 25) - one vector per arm the Task 5
   carry named: objno, sysno, mesno, vocabularyPrep, vocabularyAdverb,
   vocabularyAdjective (locno rides along) ---- */

TEST(semantic_locno_arm_wired)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b); /* ltx_count = 1 */
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "AT");
    n(&b, "5", 5);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Location 5 does not exist");
    fx_close(&f);
}

TEST(semantic_objno_arm_wired)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b); /* otx_count = 0 */
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "PRESENT");
    n(&b, "3", 3);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Object 3 does not exist");
    fx_close(&f);
}

TEST(semantic_sysno_arm_wired)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b); /* stx_count = 0 */
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "SYSMESS");
    n(&b, "4", 4);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "System message 4 does not exist");
    fx_close(&f);
}

TEST(semantic_mesno_arm_wired)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    head_pro0(&b); /* mtx_count = 0 */
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "MES");
    n(&b, "7", 7);
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d), "Message 7 does not exist");
    fx_close(&f);
}

/* The word-condact arms re-derive from the SOURCE TEXT: a parameter
   resolved numerically through a SYMBOL passes resolution but fails
   the semantic vocabulary check. */
TEST(semantic_prep_arm_wired)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    CHECK_INT(symbols_add(sintactic_symbols(), f.a, f.d, "W", 5), 1);
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "PREP");
    t(&b, T_IDENTIFIER, "W");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Word not defined in vocabulary or it has an unexpected "
              "word type : W");
    fx_close(&f);
}

TEST(semantic_adverb_arm_wired)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    CHECK_INT(symbols_add(sintactic_symbols(), f.a, f.d, "W", 5), 1);
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "ADVERB");
    t(&b, T_IDENTIFIER, "W");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Word not defined in vocabulary or it has an unexpected "
              "word type : W");
    fx_close(&f);
}

TEST(semantic_adject_arm_wired)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    CHECK_INT(symbols_add(sintactic_symbols(), f.a, f.d, "W", 5), 1);
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "ADJECT1");
    t(&b, T_IDENTIFIER, "W");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 1);
    CHECK_STR(diag_last_error(f.d),
              "Word not defined in vocabulary or it has an unexpected "
              "word type : W");
    fx_close(&f);
}

/* -semantic-warnings: same text as a Warning, parse continues and the
   condact is queued with the out-of-range value. */
TEST(semantic_warnings_downgrades_to_warning)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    Vec_ProcessCondact *cl;
    f.opts.semantic_warnings = 1;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "AT");
    n(&b, "5", 5);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(diag_warn_count(f.d), 1);
    CHECK_STR(diag_last(f.d), "Location 5 does not exist");
    cl = entry_condacts(0, 0);
    CHECK_INT(vec_at_ProcessCondact(cl, 0)->params[0].value, 5);
    fx_close(&f);
}

/* -no-semantic: the check is never called at all. */
TEST(no_semantic_skips_check_entirely)
{
    Fx f = fx_open();
    TB b = tb_new(f.a);
    f.opts.no_semantic = 1;
    head_pro0(&b);
    entry_uu(&b);
    t(&b, T_IDENTIFIER, "AT");
    n(&b, "5", 5);
    t(&b, T_SECTION_END, "/END");
    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(diag_warn_count(f.d), 0);
    fx_close(&f);
}

/* ---- preparse (include.c) ---- */

typedef struct {
    char input[512], inc[512], temp[512];
} PPaths;

static PPaths pp_paths(void)
{
    PPaths p;
    scratch_path(p.input, sizeof p.input, "t6_main.dsf");
    scratch_path(p.inc, sizeof p.inc, "t6_inc.dsf");
    scratch_path(p.temp, sizeof p.temp, "t6_main.___");
    return p;
}

static void pp_cleanup(PPaths *p)
{
    remove(p->input);
    remove(p->inc);
    remove(p->temp);
}

static size_t read_all(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "rb");
    size_t got = 0;
    CHECK(f != NULL);
    if (f) {
        got = fread(buf, 1, n - 1, f);
        fclose(f);
    }
    buf[got] = '\0';
    return got;
}

/* A no-include source is still preparsed: copied line by line with a
   WriteLn (CRLF) per line - which is what guarantees the temp file's
   last line ends in a newline even when the input's does not, masking
   defect 19.46's lexer edge in ordinary use. The map is identity. */
TEST(preparse_copies_and_maps_without_includes)
{
    Fx f = fx_open();
    PPaths p = pp_paths();
    char buf[256];
    IncludeData inc;
    write_bytes(p.input, "AAA\r\nBBB", 8); /* no trailing newline */
    CHECK_INT(preparse(f.a, f.d, p.input, p.temp), 1);
    read_all(p.temp, buf, sizeof buf);
    CHECK_STR(buf, "AAA\r\nBBB\r\n");
    CHECK_INT(include_get_data(1, &inc), 1);
    CHECK_STR(inc.original_filename, p.input);
    CHECK_INT(inc.original_line, 1);
    CHECK_INT(include_get_data(2, &inc), 1);
    CHECK_INT(inc.original_line, 2);
    CHECK_INT(include_get_data(3, &inc), 0); /* bounds guard */
    CHECK_INT(include_get_data(0, &inc), 0); /* GetIncludeData(0) guard */
    pp_cleanup(&p);
    fx_close(&f);
}

TEST(preparse_splices_include_and_maps_lines)
{
    Fx f = fx_open();
    PPaths p = pp_paths();
    char buf[512], src[600];
    IncludeData inc;
    write_bytes(p.inc, "I1\nI2\n", 6); /* LF endings collapse too */
    snprintf(src, sizeof src, "L1\r\n#include %s\r\nL3\r\n", p.inc);
    write_bytes(p.input, src, strlen(src));
    CHECK_INT(preparse(f.a, f.d, p.input, p.temp), 1);
    read_all(p.temp, buf, sizeof buf);
    CHECK_STR(buf, "L1\r\nI1\r\nI2\r\nL3\r\n");
    CHECK_INT(include_get_data(1, &inc), 1);
    CHECK_STR(inc.original_filename, p.input);
    CHECK_INT(inc.original_line, 1);
    CHECK_INT(include_get_data(2, &inc), 1);
    CHECK_STR(inc.original_filename, p.inc);
    CHECK_INT(inc.original_line, 1);
    CHECK_INT(include_get_data(3, &inc), 1);
    CHECK_STR(inc.original_filename, p.inc);
    CHECK_INT(inc.original_line, 2);
    CHECK_INT(include_get_data(4, &inc), 1);
    CHECK_STR(inc.original_filename, p.input);
    CHECK_INT(inc.original_line, 3);
    pp_cleanup(&p);
    fx_close(&f);
}

/* PreparseError shape live-verified against drf.exe 2026-08-27:
   `1:0: Include file "missing_file.inc" not found.` - line, fixed 0
   column, NO filename, exit class 2. */
TEST(preparse_missing_include_exact_shape)
{
    Fx f = fx_open();
    PPaths p = pp_paths();
    char buf[512];
    write_bytes(p.input, "A\r\n#include nope.inc\r\n", 22);
    CHECK_INT(preparse(f.a, f.d, p.input, p.temp), 0);
    CHECK_INT(diag_exit_code(f.d), 2);
    fx_read(&f, buf, sizeof buf);
    CHECK_STR(buf, "2:0: Include file \"nope.inc\" not found.\n");
    pp_cleanup(&p);
    fx_close(&f);
}

/* Nested include: reported at the INCLUDE-local line number. */
TEST(preparse_nested_include_rejected_at_local_line)
{
    Fx f = fx_open();
    PPaths p = pp_paths();
    char buf[512], src[600];
    write_bytes(p.inc, "ok\r\n#include another.inc\r\n", 26);
    snprintf(src, sizeof src, "#include %s\r\n", p.inc);
    write_bytes(p.input, src, strlen(src));
    CHECK_INT(preparse(f.a, f.d, p.input, p.temp), 0);
    CHECK_INT(diag_exit_code(f.d), 2);
    fx_read(&f, buf, sizeof buf);
    CHECK_STR(buf, "2:0: Nested includes are not allowed.\n");
    pp_cleanup(&p);
    fx_close(&f);
}

/* Character 9 - whatever it is - is discarded as the separator; the
   name is truncated at ';' and trimmed. Quotes are NOT stripped, so a
   quoted name fails carrying its quotes (analysis 14). */
TEST(preparse_filename_extraction_rules)
{
    Fx f = fx_open();
    PPaths p = pp_paths();
    char buf[512], src[700];
    write_bytes(p.inc, "INC\r\n", 5);
    /* char 9 is 'Z', then spaces around the name, then a comment */
    snprintf(src, sizeof src, "#includeZ  %s  ; trailing comment\r\n", p.inc);
    write_bytes(p.input, src, strlen(src));
    CHECK_INT(preparse(f.a, f.d, p.input, p.temp), 1);
    read_all(p.temp, buf, sizeof buf);
    CHECK_STR(buf, "INC\r\n");
    pp_cleanup(&p);
    fx_close(&f);
}

TEST(preparse_quoted_include_name_fails_with_quotes)
{
    Fx f = fx_open();
    PPaths p = pp_paths();
    char buf[512];
    write_bytes(p.input, "#include \"q.inc\"\r\n", 18);
    CHECK_INT(preparse(f.a, f.d, p.input, p.temp), 0);
    fx_read(&f, buf, sizeof buf);
    CHECK_STR(buf, "1:0: Include file \"\"q.inc\"\" not found.\n");
    pp_cleanup(&p);
    fx_close(&f);
}

TEST(preparse_verbose_echoes_including)
{
    Fx f = fx_open();
    PPaths p = pp_paths();
    char buf[512], src[600], want[600];
    diag_set_verbose(f.d, 1);
    write_bytes(p.inc, "INC\r\n", 5);
    snprintf(src, sizeof src, "#include %s\r\n", p.inc);
    write_bytes(p.input, src, strlen(src));
    CHECK_INT(preparse(f.a, f.d, p.input, p.temp), 1);
    fx_read(&f, buf, sizeof buf);
    snprintf(want, sizeof want, "Including %s...\n", p.inc);
    CHECK(strstr(buf, want) != NULL);
    pp_cleanup(&p);
    fx_close(&f);
}

/* ---- diag source remapping through the include map ---- */

/* A parse error INSIDE an included region reports the include file's
   name and ITS line number - SyntaxError's GetIncludeData threading
   (USintactic.pas:34-40) through preparse's map. */
TEST(parse_error_in_include_reports_original_file_and_line)
{
    Fx f = fx_open();
    PPaths p = pp_paths();
    char buf[1024], src[600], want[700];
    Token *stream;
    /* inc line 2's 'blah' is an invalid vocabulary type; 'blah' ends
       at column 12 of "NORTH 2 blah" (stored colno = last char). */
    write_bytes(p.inc, "/VOC\r\nNORTH 2 blah\r\n", 20);
    snprintf(src, sizeof src, "/CTL\r\n#include %s\r\n", p.inc);
    write_bytes(p.input, src, strlen(src));
    CHECK_INT(preparse(f.a, f.d, p.input, p.temp), 1);
    stream = lex_tokenize(f.a, f.d, p.temp);
    CHECK(stream != NULL);
    CHECK_INT(sintactic_parse(f.a, f.d, stream, &f.opts), 1);
    fx_read(&f, buf, sizeof buf);
    snprintf(want, sizeof want,
             "2:12:%s: \"blah\" is not a valid vocabulary word type.\n",
             p.inc);
    CHECK(strstr(buf, want) != NULL);
    pp_cleanup(&p);
    fx_close(&f);
}

/* A LEXER error (rule 43's catch-all, LexerError's shape) inside an
   included region is remapped the same way - lex.c's reconciliation
   with the include map. Column is the RAW yycolno (offending char's
   column + 1). */
TEST(lexer_error_in_include_reports_original_file_and_line)
{
    Fx f = fx_open();
    PPaths p = pp_paths();
    char buf[1024], src[600], want[700];
    Token *stream;
    write_bytes(p.inc, "%\r\n", 3);
    snprintf(src, sizeof src, "/CTL\r\n#include %s\r\n", p.inc);
    write_bytes(p.input, src, strlen(src));
    CHECK_INT(preparse(f.a, f.d, p.input, p.temp), 1);
    stream = lex_tokenize(f.a, f.d, p.temp);
    CHECK(stream == NULL);
    CHECK_INT(diag_exit_code(f.d), 1);
    fx_read(&f, buf, sizeof buf);
    snprintf(want, sizeof want,
             "1:2:%s: Unexpected character or string: \"%%\".\n", p.inc);
    CHECK(strstr(buf, want) != NULL);
    pp_cleanup(&p);
    fx_close(&f);
}

/* ---- ctlextern unit ---- */

TEST(ctlextern_composes_and_iterates)
{
    Arena *a = arena_new(0);
    CTLExternList *l = ctlextern_new(a);
    CHECK_INT((long)ctlextern_count(l), 0);
    ctlextern_add(l, a, "FILE.BIN", "EXTERN");
    ctlextern_add(l, a, "S.BIN", "SFX");
    CHECK_INT((long)ctlextern_count(l), 2);
    CHECK_STR(ctlextern_at(l, 0), "FILE.BIN|EXTERN");
    CHECK_STR(ctlextern_at(l, 1), "S.BIN|SFX");
    CHECK(ctlextern_at(l, 2) == NULL);
    arena_free(a);
}

int main(void)
{
    RUN(minimal_full_program_parses);
    RUN(wrong_section_order_reports_voc_expected_with_glued_id);
    RUN(ctl_accepts_underscores_before_voc);
    RUN(missing_ctl_is_an_error);
    RUN(ltx_before_otx_is_rejected);
    RUN(define_number_used_as_voc_value);
    RUN(define_duplicate_is_rejected);
    RUN(define_requires_identifier);
    RUN(define_underscore_value_is_invalid);
    RUN(define_expression_is_evaluated);
    RUN(define_expression_shortstring_truncation_eats_last_char);
    RUN(define_expression_maxlongint_result_hits_the_sentinel);
    RUN(define_maxlongint_literal_rejected);
    RUN(ifdef_defined_symbol_takes_block);
    RUN(ifdef_undefined_symbol_skips_block);
    RUN(ifndef_negates);
    RUN(else_of_false_ifdef_is_taken);
    RUN(else_of_true_ifdef_is_skipped);
    RUN(skipblock_rewind_keeps_nested_ifdefs_balanced);
    RUN(stray_endif_reported);
    RUN(stray_else_reported);
    RUN(false_ifdef_without_endif_hits_eof);
    RUN(unbalanced_ifdef_reported_after_end);
    RUN(ifdef_operand_must_be_string_with_betwween_typo);
    RUN(ifdef_at_eof_reported);
    RUN(echo_prints_unquoted_text_unconditionally);
    RUN(echo_requires_string);
    RUN(extern_composes_pipe_string_and_echoes);
    RUN(extern_missing_file_reported);
    RUN(extern_requires_quoted_string);
    RUN(extern_maluva_undefined_for_nextdaad_target);
    RUN(classic_and_debug_set_their_flags);
    RUN(voc_word_truncates_to_five_chars);
    RUN(voc_duplicate_word_reports_truncated_word);
    RUN(voc_invalid_type_keyword);
    RUN(voc_value_must_be_number_or_identifier);
    RUN(voc_unknown_symbol_value);
    RUN(voc_symbol_collision_reports_voc_message);
    RUN(stx_mtx_ltx_collect_messages_and_counts);
    RUN(message_numbers_must_be_consecutive);
    RUN(message_number_255_is_too_high);
    RUN(named_list_entry_resolves_through_symbols);
    RUN(named_list_entry_unknown_symbol);
    RUN(message_entry_requires_string);
    RUN(section_symbols_defined_after_otx_and_ltx);
    RUN(connection_recorded_with_direction_value);
    RUN(connection_exact_duplicate_rejected);
    RUN(connection_same_direction_different_target_accepted);
    RUN(connection_direction_undefined);
    RUN(connections_missing_for_second_location);
    RUN(connection_blocks_must_be_sequential);
    RUN(connection_location_beyond_ltx_not_defined);
    RUN(object_full_record_msb_first_flags);
    RUN(object_underscore_and_carried_locations);
    RUN(object_definition_missing);
    RUN(object_container_flag_must_be_ynu);
    RUN(object_invalid_initial_location_text);
    RUN(object_weight_over_63_rejected);
    RUN(pro_number_expected);
    RUN(entry_sign_expected_double_space);
    RUN(label_recorded_at_entry_start);
    RUN(label_duplicate_rejected);
    RUN(synonym_entries_share_one_condact_list);
    RUN(unknown_condact_reported);
    RUN(condact_with_literal_param_queued);
    RUN(pro_redefinition_warns_and_concatenates);
    RUN(userptr_queues_fake_condact);
    RUN(userptr_range_and_type_checks);
    RUN(userptr_requires_number);
    RUN(db_symbol_operand_queues_sentinel_not_value);
    RUN(db_literal_queues_its_value);
    RUN(db_range_checked_on_extracted_value);
    RUN(db_unknown_symbol_uses_extract_value_text);
    RUN(dw_symbol_operand_queues_ff_ff);
    RUN(dw_literal_queues_low_then_high);
    RUN(hex_queues_bytes_case_insensitive);
    RUN(hex_odd_length_rejected);
    RUN(hex_requires_string);
    RUN(hex_invalid_digit_fatals);
    RUN(incbin_queues_file_bytes);
    RUN(incbin_missing_file_reported);
    RUN(entry_starting_with_db_keeps_parsing);
    RUN(debug_condact_queues_opcode_220);
    RUN(param_symbol_resolves_with_fold);
    RUN(word_condact_vocab_shadows_symbol);
    RUN(plain_condact_symbol_shadows_vocab);
    RUN(param_untyped_vocab_fallback);
    RUN(param_underscore_is_no_word);
    RUN(param_unresolved_reports_number_and_name);
    RUN(param_over_255_rejected);
    RUN(param_bad_token_invalid_condact_parameter);
    RUN(synonym_verb_slot_accepts_convertible_noun);
    RUN(synonym_nonconvertible_noun_resolves_via_untyped_retry);
    RUN(indirection_on_first_param);
    RUN(indirection_on_second_param_rejected_without_v3);
    RUN(v3_allows_indirection_on_second_param);
    RUN(indirection_skips_semantic_check);
    RUN(inline_message_auto_numbers_and_dedups);
    RUN(inline_sysmess_goes_to_stx);
    RUN(xmessage_appends_hash_n_and_becomes_xmes);
    RUN(xmes_stores_text_without_append);
    RUN(extended_message_length_checks);
    RUN(xmessage_511_chars_stores_513);
    RUN(force_x_messages_promotes_mes_and_message);
    RUN(force_normal_messages_demotes_xmes_and_xmessage);
    RUN(xplay_and_xdata_go_to_other_tx);
    RUN(xmes_numeric_param_over_255_fatals_with_semantics);
    RUN(xmes_numeric_param_over_255_allowed_no_semantic);
    RUN(message_overflow_cascades_to_sysmess);
    RUN(message_overflow_classic_mode_hard_error);
    RUN(message_overflow_both_full_becomes_desc);
    RUN(message_overflow_all_tables_full_errors);
    RUN(mesno_semantic_ignores_auto_inserted_messages);
    RUN(backward_label_skip_resolves_with_wrap);
    RUN(skip_literal_negative_wraps);
    RUN(forward_label_pendingskip_then_fixed);
    RUN(forward_label_never_defined_reported);
    RUN(backward_label_wrong_process_reported);
    RUN(forward_label_other_process_reported);
    RUN(backward_label_too_far_reported);
    RUN(forward_label_too_far_reported);
    RUN(only_first_pendingskip_fixed);
    RUN(label_after_synonym_entry_records_wrong_entry);
    RUN(synonym_shared_pendingskip_fixed_once);
    RUN(db_141_byte_caught_by_fix_pass);
    RUN(xpicture_without_switch_compiles_untouched);
    RUN(xpicture_replace_switch_wrong_target);
    RUN(xpicture_replace_switch_msx_sets_maluva);
    RUN(xsave_xload_xbeep_deprecated_with_switch);
    RUN(semantic_locno_arm_wired);
    RUN(semantic_objno_arm_wired);
    RUN(semantic_sysno_arm_wired);
    RUN(semantic_mesno_arm_wired);
    RUN(semantic_prep_arm_wired);
    RUN(semantic_adverb_arm_wired);
    RUN(semantic_adject_arm_wired);
    RUN(semantic_warnings_downgrades_to_warning);
    RUN(no_semantic_skips_check_entirely);
    RUN(preparse_copies_and_maps_without_includes);
    RUN(preparse_splices_include_and_maps_lines);
    RUN(preparse_missing_include_exact_shape);
    RUN(preparse_nested_include_rejected_at_local_line);
    RUN(preparse_filename_extraction_rules);
    RUN(preparse_quoted_include_name_fails_with_quotes);
    RUN(preparse_verbose_echoes_including);
    RUN(parse_error_in_include_reports_original_file_and_line);
    RUN(lexer_error_in_include_reports_original_file_and_line);
    RUN(ctlextern_composes_and_iterates);
    return test_summary("sintactic");
}
