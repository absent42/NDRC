/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_jsonexport.c - Copyright (C) 2026 Dan Gibson.

   jsonexport.c is the byte contract UJSONExport.pas defines. Each
   test hand-builds a TINY model directly through the front containers
   (no full DSF, no lexer) reachable via sintactic_init()'s singleton
   instance, drives sintactic_parse over a small hand-built token
   stream (the same pattern test_sintactic.c uses), then calls
   jsonexport_write and asserts the EXACT bytes of the relevant region
   - tabs, CRLF, comma placement, all included. Nothing here is
   compared loosely (strstr substring presence); every assertion is a
   byte-exact CHECK_MEM against a span located by an exact anchor. */
#include "test.h"
#include "arena.h"
#include "diag.h"
#include "str.h"
#include "../src/front/sintactic.h"
#include "../src/front/jsonexport.h"
#include "../src/front/lex_tokens.h"
#include "../src/front/constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- fixture (mirrors test_sintactic.c's Fx) ---- */

typedef struct {
    Arena *a;
    Diag *d;
    FILE *diagout;
    FrontOptions opts;
    char json_path[512];
} Fx;

static void scratch_path(char *buf, size_t bufsz, const char *filename)
{
    const char *dir = getenv("TMPDIR");
    if (dir == NULL) dir = getenv("TEMP");
    if (dir == NULL) dir = getenv("TMP");
    if (dir == NULL) dir = ".";
    snprintf(buf, bufsz, "%s/%s", dir, filename);
}

static Fx fx_open(const char *json_filename)
{
    Fx f;
    f.a = arena_new(0);
    f.d = diag_new(f.a);
    f.diagout = tmpfile();
    diag_set_stream(f.d, f.diagout);
    diag_set_source(f.d, "test.dsf");
    sintactic_init(f.a, f.d);
    memset(&f.opts, 0, sizeof f.opts);
    f.opts.target = "NEXTDAAD";
    f.opts.subtarget = "";
    f.opts.check_maluva = 1;
    scratch_path(f.json_path, sizeof f.json_path, json_filename);
    return f;
}

static void fx_close(Fx *f)
{
    fclose(f->diagout);
    remove(f->json_path);
    arena_free(f->a);
}

/* Reads the whole output file back in BINARY mode - text-mode CRLF
   translation would silently corrupt exactly the bytes under test. */
static size_t fx_read_json(Fx *f, char *buf, size_t n)
{
    FILE *jf = fopen(f->json_path, "rb");
    size_t got;
    CHECK(jf != NULL);
    if (jf == NULL) return 0;
    got = fread(buf, 1, n - 1, jf);
    buf[got] = '\0';
    fclose(jf);
    return got;
}

/* ---- hand-built token streams (subset of test_sintactic.c's) ---- */

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
    t->line = ++b->line;
    t->col = 7;
    t->next = NULL;
    if (b->tail) b->tail->next = t; else b->head = t;
    b->tail = t;
}

static void t(TB *b, int id, const char *text)
{
    tb_add(b, id, text, TOKEN_NO_VALUE);
}

static void n(TB *b, const char *text, long value)
{
    tb_add(b, T_NUMBER, text, value);
}

static void s(TB *b, const char *inner)
{
    char buf[512];
    snprintf(buf, sizeof buf, "\"%s\"", inner);
    tb_add(b, T_STRING, buf, TOKEN_NO_VALUE);
}

static void le(TB *b, long num)
{
    char buf[32];
    snprintf(buf, sizeof buf, "/%ld", num);
    tb_add(b, T_LIST_ENTRY, buf, num);
}

/* /CTL /VOC, no vocabulary words */
static void head_ctl_voc(TB *b)
{
    t(b, T_SECTION_CTL, "/CTL");
    t(b, T_SECTION_VOC, "/VOC");
}

/* /STX /MTX /OTX, all empty, THEN /LTX with one entry ("room"), THEN
   /CON (one location, no connections)/OBJ (empty)/PRO 0 (empty)/END -
   the smallest program that reaches a clean parse. */
static void world_close_one_room(TB *b)
{
    t(b, T_SECTION_STX, "/STX");
    t(b, T_SECTION_MTX, "/MTX");
    t(b, T_SECTION_OTX, "/OTX");
    t(b, T_SECTION_LTX, "/LTX");
    le(b, 0);
    s(b, "room");
    t(b, T_SECTION_CON, "/CON");
    le(b, 0);
    t(b, T_SECTION_OBJ, "/OBJ");
    t(b, T_SECTION_PRO, "/PRO");
    n(b, "0", 0);
    t(b, T_SECTION_END, "/END");
}

/* ---- expected-block builder: mirrors the WriteLn(tabs(),text)
   idiom by construction (tab count is an explicit integer at each
   call, matching the indent levels traced by hand against
   UJSONExport.pas - see the task report), independent of jsonexport.c
   itself. ---- */

typedef struct { char buf[4096]; size_t len; } Exp;

static void exp_reset(Exp *e) { e->len = 0; }

static void exp_raw(Exp *e, const char *s)
{
    size_t n = strlen(s);
    memcpy(e->buf + e->len, s, n);
    e->len += n;
}

/* tabs + text + CRLF */
static void exp_line(Exp *e, int tabs, const char *text)
{
    int i;
    for (i = 0; i < tabs; i++) e->buf[e->len++] = '\t';
    exp_raw(e, text);
    exp_raw(e, "\r\n");
}

/* ---- T1: settings line + symbols reversed (idiom A, tabs, CRLF) ---- */

TEST(settings_line_exact_shape)
{
    Fx f = fx_open("t8_settings.json");
    TB b = tb_new(f.a);
    char buf[8192];
    Exp e;

    t(&b, T_CLASSIC, "#classic");
    head_ctl_voc(&b);
    world_close_one_room(&b);
    f.opts.v3 = 1;
    f.opts.check_maluva = 0; /* -check-maluva-disabled: forces maluva_used=1 */

    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(jsonexport_write(f.d, f.json_path, &f.opts), 0);
    fx_read_json(&f, buf, sizeof buf);

    exp_reset(&e);
    exp_line(&e, 0, "{");
    exp_line(&e, 0, "\"settings\":");
    exp_line(&e, 1, "[");
    exp_line(&e, 1,
        "{\"classic_mode\":1, \"debug_mode\":0, \"v3code\":1, "
        "\"maluva_used\":1}");
    exp_line(&e, 1, "],");
    e.buf[e.len] = '\0';

    CHECK_MEM(buf, e.buf, e.len);
    fx_close(&f);
}

TEST(symbols_array_reversed_idiom_a)
{
    Fx f = fx_open("t8_symbols.json");
    TB b = tb_new(f.a);
    char buf[8192];
    const char *anchor = "\"symbols\":\r\n";
    char *p;
    Exp e;

    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "AAA");
    n(&b, "111", 111);
    t(&b, T_DEFINE, "#define");
    t(&b, T_IDENTIFIER, "BBB");
    n(&b, "222", 222);
    head_ctl_voc(&b);
    world_close_one_room(&b);

    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(jsonexport_write(f.d, f.json_path, &f.opts), 0);
    fx_read_json(&f, buf, sizeof buf);

    /* Insertion order: AAA, BBB, then ParseOTX's LAST_OBJECT(-1)/
       NUM_OBJECTS(0) (OTX has zero entries), ParseLTX's
       LAST_LOCATION(0)/NUM_LOCATIONS(1) (one room), then ParseOBJ's
       NUM_CARRIED(0)/NUM_WORN(0) (OBJ has zero entries too, but the
       two symbols are still injected). JSON reverses it. */
    exp_reset(&e);
    exp_line(&e, 0, "\"symbols\":");
    exp_line(&e, 1, "[");
    exp_raw(&e, "\t\t{\"symbol\":\"NUM_WORN\", \"Value\":0},\n");
    exp_raw(&e, "\t\t{\"symbol\":\"NUM_CARRIED\", \"Value\":0},\n");
    exp_raw(&e, "\t\t{\"symbol\":\"NUM_LOCATIONS\", \"Value\":1},\n");
    exp_raw(&e, "\t\t{\"symbol\":\"LAST_LOCATION\", \"Value\":0},\n");
    exp_raw(&e, "\t\t{\"symbol\":\"NUM_OBJECTS\", \"Value\":0},\n");
    exp_raw(&e, "\t\t{\"symbol\":\"LAST_OBJECT\", \"Value\":-1},\n");
    exp_raw(&e, "\t\t{\"symbol\":\"BBB\", \"Value\":222},\n");
    exp_raw(&e, "\t\t{\"symbol\":\"AAA\", \"Value\":111}\r\n");
    exp_line(&e, 1, "],");
    e.buf[e.len] = '\0';

    p = strstr(buf, anchor);
    CHECK(p != NULL);
    if (p == NULL) { fx_close(&f); return; }
    CHECK_MEM(p, e.buf, e.len);
    fx_close(&f);
}

/* ---- T2: vocabulary is IN-ORDER (ascending), not reversed ---- */

TEST(vocabulary_array_sorted_ascending)
{
    Fx f = fx_open("t8_voc.json");
    TB b = tb_new(f.a);
    char buf[8192];
    const char *anchor = "\"vocabulary\":\r\n";
    char *p;
    Exp e;

    t(&b, T_SECTION_CTL, "/CTL");
    t(&b, T_SECTION_VOC, "/VOC");
    /* inserted out of order: ZOO then APPLE */
    t(&b, T_IDENTIFIER, "ZOO");
    n(&b, "99", 99);
    t(&b, T_IDENTIFIER, "verb");
    t(&b, T_IDENTIFIER, "APPLE");
    n(&b, "1", 1);
    t(&b, T_IDENTIFIER, "verb");
    world_close_one_room(&b);

    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(jsonexport_write(f.d, f.json_path, &f.opts), 0);
    fx_read_json(&f, buf, sizeof buf);

    exp_reset(&e);
    exp_line(&e, 0, "\"vocabulary\":");
    exp_line(&e, 1, "[");
    exp_raw(&e, "\t\t{\"VocWord\":\"APPLE\", \"Value\":1,\"VocType\":0 },\n");
    exp_raw(&e, "\t\t{\"VocWord\":\"ZOO\", \"Value\":99,\"VocType\":0 }\r\n");
    exp_line(&e, 1, "],");
    e.buf[e.len] = '\0';

    p = strstr(buf, anchor);
    CHECK(p != NULL);
    if (p == NULL) { fx_close(&f); return; }
    CHECK_MEM(p, e.buf, e.len);
    fx_close(&f);
}

/* ---- T2b: empty /VOC - the idiom A underflow-guard shape, LIVE-PINNED
   (probe 2026-08-27, EMPTYVOC vector against drf.exe, D:\DRC branch
   nextdaad): the reference exits 0 and emits a BLANK LINE between "["
   and "]," (skipping the trim on an empty built string, then still
   writing the unconditional WriteLn's own CRLF) rather than aborting
   on SetLength's negative-length request - exactly what
   emit_vocabulary's guard produces. head_ctl_voc already leaves /VOC
   with zero words. ---- */

TEST(empty_vocabulary_emits_blank_line_guard)
{
    Fx f = fx_open("t8_emptyvoc.json");
    TB b = tb_new(f.a);
    char buf[8192];
    const char *anchor = "\"vocabulary\":\r\n";
    char *p;

    head_ctl_voc(&b); /* /CTL /VOC, no words at all */
    world_close_one_room(&b);

    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(jsonexport_write(f.d, f.json_path, &f.opts), 0);
    fx_read_json(&f, buf, sizeof buf);

    p = strstr(buf, anchor);
    CHECK(p != NULL);
    if (p == NULL) { fx_close(&f); return; }
    {
        static const char want[] = "\"vocabulary\":\r\n\t[\r\n\r\n\t],\r\n";
        CHECK_MEM(p, want, strlen(want));
    }
    fx_close(&f);
}

/* ---- T3: externs empty-array shape (idiom B, zero-entry edge) ---- */

TEST(externs_empty_array_shape)
{
    Fx f = fx_open("t8_ext.json");
    TB b = tb_new(f.a);
    char buf[8192];
    const char *anchor = "\"externs\":\r\n";
    char *p;
    Exp e;

    head_ctl_voc(&b);
    world_close_one_room(&b);

    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(jsonexport_write(f.d, f.json_path, &f.opts), 0);
    fx_read_json(&f, buf, sizeof buf);

    /* IF Length(CTLExternList)>0 THEN FOR ... - with zero entries the
       loop body never executes, not even once: "[" is followed
       DIRECTLY by "]," with nothing between. */
    exp_reset(&e);
    exp_line(&e, 0, "\"externs\":");
    exp_line(&e, 1, "[");
    exp_line(&e, 1, "],");
    e.buf[e.len] = '\0';

    p = strstr(buf, anchor);
    CHECK(p != NULL);
    if (p == NULL) { fx_close(&f); return; }
    CHECK_MEM(p, e.buf, e.len);
    fx_close(&f);
}

/* ---- T4: object_data + connections, idiom B comma-per-sibling ---- */

TEST(object_and_connection_idiom_b_shape)
{
    Fx f = fx_open("t8_objcon.json");
    TB b = tb_new(f.a);
    char buf[8192];
    const char *anchor;
    char *p;
    Exp e;

    t(&b, T_SECTION_CTL, "/CTL");
    t(&b, T_SECTION_VOC, "/VOC");
    t(&b, T_IDENTIFIER, "DOOR");
    n(&b, "50", 50);
    t(&b, T_IDENTIFIER, "noun");
    /* direction words for /CON, defined before use (grammar requires
       an already-known VERB or NOUN vocabulary word) */
    t(&b, T_IDENTIFIER, "N");
    n(&b, "2", 2);
    t(&b, T_IDENTIFIER, "noun");
    t(&b, T_IDENTIFIER, "S");
    n(&b, "3", 3);
    t(&b, T_IDENTIFIER, "noun");
    t(&b, T_SECTION_STX, "/STX");
    t(&b, T_SECTION_MTX, "/MTX");
    t(&b, T_SECTION_OTX, "/OTX");
    le(&b, 0);
    s(&b, "an object");
    t(&b, T_SECTION_LTX, "/LTX");
    le(&b, 0);
    s(&b, "room zero");
    le(&b, 1);
    s(&b, "room one");
    t(&b, T_SECTION_CON, "/CON");
    /* two connections, so the FIRST needs a trailing comma and the
       SECOND does not */
    le(&b, 0);
    t(&b, T_IDENTIFIER, "N");
    n(&b, "1", 1);
    le(&b, 1);
    t(&b, T_IDENTIFIER, "S");
    n(&b, "0", 0);
    t(&b, T_SECTION_OBJ, "/OBJ");
    le(&b, 0);
    n(&b, "0", 0);   /* initially at location 0 */
    n(&b, "10", 10); /* weight */
    t(&b, T_IDENTIFIER, "Y"); /* container */
    t(&b, T_IDENTIFIER, "n"); /* wearable */
    { int i; t(&b, T_IDENTIFIER, "Y"); for (i = 0; i < 15; i++) t(&b, T_IDENTIFIER, "N"); }
    t(&b, T_IDENTIFIER, "DOOR");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_SECTION_PRO, "/PRO");
    n(&b, "0", 0);
    t(&b, T_SECTION_END, "/END");

    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(jsonexport_write(f.d, f.json_path, &f.opts), 0);
    fx_read_json(&f, buf, sizeof buf);

    exp_reset(&e);
    exp_line(&e, 0, "\"object_data\":");
    exp_line(&e, 1, "[");
    exp_line(&e, 1, "{");
    exp_line(&e, 2, "\"Value\":0,");
    exp_line(&e, 2, "\"Noun\":50,");
    exp_line(&e, 2, "\"Adjective\":255,");
    exp_line(&e, 2, "\"Container\":1,");
    exp_line(&e, 2, "\"Wearable\":0,");
    exp_line(&e, 2, "\"Flags\":32768,");
    exp_line(&e, 2, "\"Weight\":10,");
    exp_line(&e, 2, "\"InitialyAt\":0");
    exp_line(&e, 1, "}");
    exp_line(&e, 1, "],");
    e.buf[e.len] = '\0';

    anchor = "\"object_data\":\r\n";
    p = strstr(buf, anchor);
    CHECK(p != NULL);
    if (p != NULL) CHECK_MEM(p, e.buf, e.len);

    exp_reset(&e);
    exp_line(&e, 0, "\"connections\":");
    exp_line(&e, 1, "[");
    exp_line(&e, 1, "{");
    exp_line(&e, 2, "\"FromLoc\":0,");
    exp_line(&e, 2, "\"ToLoc\":1,");
    exp_line(&e, 2, "\"Direction\":2");
    exp_line(&e, 1, "},"); /* first of two: trailing comma */
    exp_line(&e, 1, "{");
    exp_line(&e, 2, "\"FromLoc\":1,");
    exp_line(&e, 2, "\"ToLoc\":0,");
    exp_line(&e, 2, "\"Direction\":3");
    exp_line(&e, 1, "}"); /* last: no comma */
    exp_line(&e, 1, "],");
    e.buf[e.len] = '\0';

    anchor = "\"connections\":\r\n";
    p = strstr(buf, anchor);
    CHECK(p != NULL);
    if (p != NULL) CHECK_MEM(p, e.buf, e.len);

    fx_close(&f);
}

/* ---- T5: accented char through ConvertChars; the 19.2 backslash
   defect reproduced byte-for-byte (invalid JSON, on purpose) ---- */

TEST(accented_char_and_backslash_defect)
{
    Fx f = fx_open("t8_text.json");
    TB b = tb_new(f.a);
    char buf[8192];
    const char *anchor;
    char *p;
    Exp e;

    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    t(&b, T_SECTION_MTX, "/MTX");
    le(&b, 0);
    s(&b, "path C:\\games"); /* ONE raw backslash byte in the source text */
    t(&b, T_SECTION_OTX, "/OTX");
    t(&b, T_SECTION_LTX, "/LTX");
    le(&b, 0);
    s(&b, "sal\xF3n"); /* raw Latin-1 0xF3 = o-acute */
    t(&b, T_SECTION_CON, "/CON");
    le(&b, 0);
    t(&b, T_SECTION_OBJ, "/OBJ");
    t(&b, T_SECTION_PRO, "/PRO");
    n(&b, "0", 0);
    t(&b, T_SECTION_END, "/END");

    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(jsonexport_write(f.d, f.json_path, &f.opts), 0);
    fx_read_json(&f, buf, sizeof buf);

    /* messages[0].Text: the backslash passes through UNCHANGED - no
       escaping applied by either FixDoubleQuotes or ConvertChars
       (analysis 16.3/19.2) - the byte sequence \g (backslash then the
       letter g) is not a legal JSON escape: this Text value, taken in
       isolation, is NOT valid JSON, exactly reproducing the defect. */
    exp_reset(&e);
    exp_line(&e, 0, "\"messages\":");
    exp_line(&e, 1, "[");
    exp_line(&e, 1, "{");
    exp_line(&e, 2, "\"Value\":0,");
    exp_line(&e, 2, "\"Text\":\"path C:\\games\"");
    exp_line(&e, 1, "}");
    exp_line(&e, 1, "],");
    e.buf[e.len] = '\0';

    anchor = "\"messages\":\r\n";
    p = strstr(buf, anchor);
    CHECK(p != NULL);
    if (p != NULL) CHECK_MEM(p, e.buf, e.len);

    /* locations[0].Text: 0xF3 (o-acute) -> the DIRECT unicode-escape
       \u0018 (analysis 16.2's table: byte 243 -> hex 18). */
    exp_reset(&e);
    exp_line(&e, 0, "\"locations\":");
    exp_line(&e, 1, "[");
    exp_line(&e, 1, "{");
    exp_line(&e, 2, "\"Value\":0,");
    exp_line(&e, 2, "\"Text\":\"sal\\u0018n\"");
    exp_line(&e, 1, "}");
    exp_line(&e, 1, "],");
    e.buf[e.len] = '\0';

    anchor = "\"locations\":\r\n";
    p = strstr(buf, anchor);
    CHECK(p != NULL);
    if (p != NULL) CHECK_MEM(p, e.buf, e.len);

    fx_close(&f);
}

/* ---- T6: ASCII7 folds the same accented byte to plain 'o', proving
   ConvertAscii7Chars (the -7 table) is the one actually wired when
   opts.ascii7 is set - a genuinely different table, not just a flag
   that changes nothing (analysis 16.4). ---- */

TEST(ascii7_option_selects_the_other_table)
{
    Fx f = fx_open("t8_ascii7.json");
    TB b = tb_new(f.a);
    char buf[8192];
    const char *anchor;
    char *p;
    Exp e;

    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    t(&b, T_SECTION_MTX, "/MTX");
    t(&b, T_SECTION_OTX, "/OTX");
    t(&b, T_SECTION_LTX, "/LTX");
    le(&b, 0);
    s(&b, "sal\xF3n");
    t(&b, T_SECTION_CON, "/CON");
    le(&b, 0);
    t(&b, T_SECTION_OBJ, "/OBJ");
    t(&b, T_SECTION_PRO, "/PRO");
    n(&b, "0", 0);
    t(&b, T_SECTION_END, "/END");
    f.opts.ascii7 = 1;

    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(jsonexport_write(f.d, f.json_path, &f.opts), 0);
    fx_read_json(&f, buf, sizeof buf);

    exp_reset(&e);
    exp_line(&e, 0, "\"locations\":");
    exp_line(&e, 1, "[");
    exp_line(&e, 1, "{");
    exp_line(&e, 2, "\"Value\":0,");
    exp_line(&e, 2, "\"Text\":\"salon\""); /* o-acute folds to plain 'o' */
    exp_line(&e, 1, "}");
    exp_line(&e, 1, "],");
    e.buf[e.len] = '\0';

    anchor = "\"locations\":\r\n";
    p = strstr(buf, anchor);
    CHECK(p != NULL);
    if (p != NULL) CHECK_MEM(p, e.buf, e.len);
    fx_close(&f);
}

/* ---- T7: the two-pass condact shape (all IndirectionN, THEN all
   ParamN, THEN NumParams last - never interleaved, analysis 16.6) ---- */

TEST(condact_two_pass_indirection_then_params_then_count)
{
    Fx f = fx_open("t8_condact.json");
    TB b = tb_new(f.a);
    char buf[16384];
    const char *anchor;
    char *p;
    Exp e;

    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    t(&b, T_SECTION_MTX, "/MTX");
    t(&b, T_SECTION_OTX, "/OTX");
    t(&b, T_SECTION_LTX, "/LTX");
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
    t(&b, T_IDENTIFIER, "EQ");    /* opcode 13, 2 params: flagno, value */
    t(&b, T_INDIRECT, "@");
    n(&b, "5", 5);
    n(&b, "9", 9);
    t(&b, T_SECTION_END, "/END");

    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(jsonexport_write(f.d, f.json_path, &f.opts), 0);
    fx_read_json(&f, buf, sizeof buf);

    /* exp starts at the anchor text, mid-line, already past the
       condact object's 6-tab indentation; every subsequent line
       carries that same 6-tab prefix in full. */
    exp_reset(&e);
    exp_line(&e, 0, "\"Opcode\":13,");
    exp_line(&e, 6, "\"Condact\":\"EQ\",");
    exp_line(&e, 6, "\"Indirection1\":1,");
    exp_line(&e, 6, "\"Indirection2\":0,");
    exp_line(&e, 6, "\"Param1\":5,");
    exp_line(&e, 6, "\"Param2\":9,");
    exp_line(&e, 6, "\"NumParams\":2");
    e.buf[e.len] = '\0';

    anchor = "\"Opcode\":13,";
    p = strstr(buf, anchor);
    CHECK(p != NULL);
    if (p != NULL) CHECK_MEM(p, e.buf, e.len);
    fx_close(&f);
}

/* ---- T8: the leaked PENDINGSKIP node (opcode 141) - a forward SKIP
   left un-fixed (sintactic_fix_forward_labels deliberately NOT
   called) serialises faithfully, no filtering (Task 7's carry). ---- */

TEST(leaked_pendingskip_node_serializes_faithfully)
{
    Fx f = fx_open("t8_pendingskip.json");
    TB b = tb_new(f.a);
    char buf[16384];
    const char *anchor;
    char *p;
    Exp e;

    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    t(&b, T_SECTION_MTX, "/MTX");
    t(&b, T_SECTION_OTX, "/OTX");
    t(&b, T_SECTION_LTX, "/LTX");
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
    t(&b, T_IDENTIFIER, "SKIP");
    t(&b, T_LABEL, "$fwd");
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_LABEL, "$fwd"); /* defined later - forward reference */
    t(&b, T_PROCESS_ENTRY_SIGN, ">");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_UNDERSCORE, "_");
    t(&b, T_IDENTIFIER, "DONE");
    t(&b, T_SECTION_END, "/END");

    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    /* Deliberately skip sintactic_fix_forward_labels(): the raw
       PENDINGSKIP placeholder must reach the exporter untouched. */
    CHECK_INT(jsonexport_write(f.d, f.json_path, &f.opts), 0);
    fx_read_json(&f, buf, sizeof buf);

    /* Leaked node: Opcode 141, table name PENDINGSKIP (UCondacts.pas:
       169, NumParams:1/Type1:value), the un-fixed forward-label slot
       id as its one param - value not asserted here (Task 7's own
       concern), just that it flows through untouched, and NumParams
       reads back as 1 (AddProcessCondact used the FINAL opcode's
       GetNumParams(141)=1, analysis 16's carry). */
    exp_reset(&e);
    exp_line(&e, 0, "\"Opcode\":141,");
    exp_line(&e, 6, "\"Condact\":\"PENDINGSKIP\",");
    exp_line(&e, 6, "\"Indirection1\":0,");
    e.buf[e.len] = '\0';

    anchor = "\"Opcode\":141,";
    p = strstr(buf, anchor);
    CHECK(p != NULL);
    if (p != NULL) CHECK_MEM(p, e.buf, e.len);
    CHECK(strstr(buf, "\"NumParams\":1") != NULL);
    fx_close(&f);
}

/* ---- T9: FixDoubleQuotes - a bare quote is escaped ---- */

TEST(double_quote_in_text_is_escaped)
{
    Fx f = fx_open("t8_quote.json");
    TB b = tb_new(f.a);
    char buf[8192];
    const char *anchor;
    char *p;
    Exp e;

    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    t(&b, T_SECTION_MTX, "/MTX");
    le(&b, 0);
    s(&b, "He said \"hi\""); /* source text: He said "hi" (plain quotes, no backslashes) */
    t(&b, T_SECTION_OTX, "/OTX");
    t(&b, T_SECTION_LTX, "/LTX");
    le(&b, 0);
    s(&b, "room");
    t(&b, T_SECTION_CON, "/CON");
    le(&b, 0);
    t(&b, T_SECTION_OBJ, "/OBJ");
    t(&b, T_SECTION_PRO, "/PRO");
    n(&b, "0", 0);
    t(&b, T_SECTION_END, "/END");

    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(jsonexport_write(f.d, f.json_path, &f.opts), 0);
    fx_read_json(&f, buf, sizeof buf);

    /* FixDoubleQuotes step 1: every " -> \" (both quotes escaped).
       Step 2 (collapsing \\") does not fire here - no pre-existing
       backslash-quote pair in the source. */
    exp_reset(&e);
    exp_line(&e, 0, "\"messages\":");
    exp_line(&e, 1, "[");
    exp_line(&e, 1, "{");
    exp_line(&e, 2, "\"Value\":0,");
    exp_line(&e, 2, "\"Text\":\"He said \\\"hi\\\"\"");
    exp_line(&e, 1, "}");
    exp_line(&e, 1, "],");
    e.buf[e.len] = '\0';

    anchor = "\"messages\":\r\n";
    p = strstr(buf, anchor);
    CHECK(p != NULL);
    if (p != NULL) CHECK_MEM(p, e.buf, e.len);
    fx_close(&f);
}

/* ---- T10: FixDoubleQuotes' second replace collapses a doubled
   escape - text already containing a literal backslash immediately
   before a quote must NOT end up with two backslashes on the wire
   (UJSONExport.pas:38-43's second AnsiReplaceStr call). ---- */

TEST(backslash_before_quote_does_not_double)
{
    Fx f = fx_open("t8_bsquote.json");
    TB b = tb_new(f.a);
    char buf[8192];
    const char *anchor;
    char *p;
    Exp e;

    head_ctl_voc(&b);
    t(&b, T_SECTION_STX, "/STX");
    t(&b, T_SECTION_MTX, "/MTX");
    le(&b, 0);
    /* source text: path\"end - ONE backslash immediately before the
       quote, both real bytes */
    s(&b, "path\\\"end");
    t(&b, T_SECTION_OTX, "/OTX");
    t(&b, T_SECTION_LTX, "/LTX");
    le(&b, 0);
    s(&b, "room");
    t(&b, T_SECTION_CON, "/CON");
    le(&b, 0);
    t(&b, T_SECTION_OBJ, "/OBJ");
    t(&b, T_SECTION_PRO, "/PRO");
    n(&b, "0", 0);
    t(&b, T_SECTION_END, "/END");

    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(jsonexport_write(f.d, f.json_path, &f.opts), 0);
    fx_read_json(&f, buf, sizeof buf);

    /* Step 1 turns the source's backslash-quote into backslash-
       backslash-quote (only the quote itself gets a NEW backslash
       prefixed onto it); step 2 collapses that back down to a single
       backslash-quote pair - the tamper-check below (removing step 2)
       would produce "path\\\"end" instead. */
    exp_reset(&e);
    exp_line(&e, 0, "\"messages\":");
    exp_line(&e, 1, "[");
    exp_line(&e, 1, "{");
    exp_line(&e, 2, "\"Value\":0,");
    exp_line(&e, 2, "\"Text\":\"path\\\"end\"");
    exp_line(&e, 1, "}");
    exp_line(&e, 1, "],");
    e.buf[e.len] = '\0';

    anchor = "\"messages\":\r\n";
    p = strstr(buf, anchor);
    CHECK(p != NULL);
    if (p != NULL) CHECK_MEM(p, e.buf, e.len);
    fx_close(&f);
}

/* ---- T11: jsonexport_render must produce bytes IDENTICAL to what
   jsonexport_write fwrites for the same parsed state - the render
   path is a byte-exact substitute for the file path, not a second
   implementation. ---- */

TEST(render_matches_write_bytes)
{
    Fx f = fx_open("t8_render.json");
    TB b = tb_new(f.a);
    char buf[8192];
    size_t got;
    const unsigned char *data;
    size_t len;
    Arena *ra;

    head_ctl_voc(&b);
    world_close_one_room(&b);

    CHECK_INT(sintactic_parse(f.a, f.d, b.head, &f.opts), 0);
    CHECK_INT(jsonexport_write(f.d, f.json_path, &f.opts), 0);
    got = fx_read_json(&f, buf, sizeof buf);

    ra = arena_new(0);
    CHECK_INT(jsonexport_render(ra, f.d, &f.opts, &data, &len), 0);
    CHECK_INT((int)len, (int)got);
    if (len == got) CHECK_MEM(data, buf, len);
    arena_free(ra);

    fx_close(&f);
}

int main(void)
{
    RUN(settings_line_exact_shape);
    RUN(symbols_array_reversed_idiom_a);
    RUN(vocabulary_array_sorted_ascending);
    RUN(empty_vocabulary_emits_blank_line_guard);
    RUN(externs_empty_array_shape);
    RUN(object_and_connection_idiom_b_shape);
    RUN(accented_char_and_backslash_defect);
    RUN(ascii7_option_selects_the_other_table);
    RUN(condact_two_pass_indirection_then_params_then_count);
    RUN(leaked_pendingskip_node_serializes_faithfully);
    RUN(double_quote_in_text_is_escaped);
    RUN(backslash_before_quote_does_not_double);
    RUN(render_matches_write_bytes);
    return test_summary("jsonexport");
}
