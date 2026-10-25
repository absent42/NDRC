/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_srcpos.c - Copyright (C) 2026 Dan Gibson.

   srcpos.c: source positions recorded by the front end, compiled from
   real DSF files through preparse, lexer and parser. */
#include "test.h"
#include "arena.h"
#include "diag.h"
#include "../src/front/sintactic.h"
#include "../src/front/include.h"
#include "../src/front/srcpos.h"

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

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f == NULL) return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

typedef struct {
    Arena *a;
    Diag *d;
    FILE *out;
    char main_path[512];
    char temp_path[520];
    int rc;
} Run;

/* Preparse -> lex -> parse, as main.c's front half; COLS stands in
   for an injected builtin symbol. */
static void compile(Run *r, const char *name, const char *src)
{
    FrontOptions opts;
    Token *stream;

    r->a = arena_new(0);
    r->d = diag_new(r->a);
    r->out = tmpfile();
    diag_set_stream(r->d, r->out);
    scratch_path(r->main_path, sizeof r->main_path, name);
    snprintf(r->temp_path, sizeof r->temp_path, "%s.___", r->main_path);
    write_text(r->main_path, src);

    sintactic_init(r->a, r->d);
    r->rc = 1;
    if (!preparse(r->a, r->d, r->main_path, r->temp_path)) return;
    stream = lex_tokenize(r->a, r->d, r->temp_path);
    if (stream == NULL) return;
    symbols_add(sintactic_symbols(), r->a, r->d, "COLS", 42);

    memset(&opts, 0, sizeof opts);
    opts.target = "NEXTDAAD";
    opts.subtarget = "";
    opts.v3 = 1;
    opts.check_maluva = 1;
    r->rc = sintactic_parse(r->a, r->d, stream, &opts);
    if (r->rc == 0) r->rc = sintactic_fix_forward_labels();
    if (r->rc != 0) {
        char buf[1024];
        size_t got;
        rewind(r->out);
        got = fread(buf, 1, sizeof buf - 1, r->out);
        buf[got] = '\0';
        printf("compile diag: %s\n", buf);
    }
}

static void run_close(Run *r)
{
    remove(r->temp_path);
    remove(r->main_path);
    fclose(r->out);
    arena_free(r->a);
}

static void check_pos(SrcPos p, const char *file, int line, int col)
{
    CHECK_STR(p.file, file);
    CHECK_INT(p.line, line);
    CHECK_INT(p.col, col);
}

/* Lines 1-10; the caller's /PRO starts on line 11. */
#define HEAD_NO_VOC \
    "/CTL\n/VOC\n/STX\n/MTX\n/OTX\n/LTX\n/0 \"room\"\n/CON\n/0\n/OBJ\n"

TEST(cond_positions_main_file)
{
    static const char *src =
        HEAD_NO_VOC
        "/PRO 0\n"                          /* 11 */
        "> _       _     LET 100 7\n"       /* 12 */
        "                DONE\n"            /* 13 */
        "/END\n";
    Run r;
    compile(&r, "srcpos_main.dsf", src);
    CHECK_INT(r.rc, 0);

    CHECK_INT(srcpos_proc_count(), 1);
    check_pos(srcpos_proc(0), r.main_path, 11, 6);
    CHECK_INT(srcpos_entry_count(0), 1);
    check_pos(srcpos_entry(0, 0), r.main_path, 12, 3);
    CHECK_INT(srcpos_cond_count(0, 0), 2);
    check_pos(srcpos_cond(0, 0, 0), r.main_path, 12, 17);
    check_pos(srcpos_cond(0, 0, 1), r.main_path, 13, 17);
    run_close(&r);
}

TEST(cond_positions_include)
{
    char inc_path[512];
    char src[2048];
    Run r;

    scratch_path(inc_path, sizeof inc_path, "srcpos_inc.dsf");
    write_text(inc_path,
               "; included\n"                       /* inc 1 */
               "> _       _     SET     5\n"        /* inc 2 */
               "                  DONE\n");         /* inc 3 */
    snprintf(src, sizeof src,
             HEAD_NO_VOC
             "/PRO 0\n"                             /* 11 */
             "#include %s\n"                        /* 12 */
             "> _ _ CLS\n"                          /* 13 */
             "/END\n", inc_path);
    compile(&r, "srcpos_incmain.dsf", src);
    CHECK_INT(r.rc, 0);

    CHECK_INT(srcpos_entry_count(0), 2);
    check_pos(srcpos_entry(0, 0), inc_path, 2, 3);
    check_pos(srcpos_cond(0, 0, 0), inc_path, 2, 17);
    check_pos(srcpos_cond(0, 0, 1), inc_path, 3, 19);
    check_pos(srcpos_entry(0, 1), r.main_path, 13, 3);
    check_pos(srcpos_cond(0, 1, 0), r.main_path, 13, 7);
    run_close(&r);
    remove(inc_path);
}

/* Lines 1-12 with GET/DROP vocabulary. */
#define HEAD_VOC \
    "/CTL\n/VOC\nGET 20 verb\nDROP 21 verb\n/STX\n/MTX\n/OTX\n/LTX\n" \
    "/0 \"room\"\n/CON\n/0\n/OBJ\n"

TEST(synonym_entries_get_own_lines)
{
    static const char *src =
        HEAD_VOC
        ";\n;\n;\n;\n;\n;\n"                /* 13-18 */
        "/PRO 0\n"                          /* 19 */
        "> GET     _\n"                     /* 20 */
        "  > DROP    _   SET 5\n"           /* 21 */
        "                DONE\n"            /* 22 */
        "/END\n";
    Run r;
    compile(&r, "srcpos_syn.dsf", src);
    CHECK_INT(r.rc, 0);

    CHECK_INT(srcpos_entry_count(0), 2);
    check_pos(srcpos_entry(0, 0), r.main_path, 20, 3);
    check_pos(srcpos_entry(0, 1), r.main_path, 21, 5);
    CHECK_INT(srcpos_cond_count(0, 0), 2);
    CHECK_INT(srcpos_cond_count(0, 1), 2);
    check_pos(srcpos_cond(0, 0, 0), r.main_path, 21, 17);
    check_pos(srcpos_cond(0, 0, 1), r.main_path, 22, 17);
    check_pos(srcpos_cond(0, 1, 0), r.main_path, 21, 17);
    check_pos(srcpos_cond(0, 1, 1), r.main_path, 22, 17);
    run_close(&r);
}

TEST(defines_only_user_names)
{
    static const char *src =
        "#define fCounter 100\n"
        "#ifdef \"NOSUCH\"\n#define fSkipped 3\n#endif\n"
        "/CTL\n/VOC\nGET 20 verb\n/STX\n/MTX\n/OTX\n/LTX\n/0 \"room\"\n"
        "/CON\n/0\n/OBJ\n"
        "#define fLater \"fCounter+1\"\n"
        "/PRO 0\n> _ _ DONE\n/END\n";
    Run r;
    compile(&r, "srcpos_def.dsf", src);
    CHECK_INT(r.rc, 0);

    CHECK_INT(srcpos_define_count(), 2);
    CHECK_STR(srcpos_define_name(0), "fCounter");
    CHECK_INT(srcpos_define_value(0), 100);
    CHECK_STR(srcpos_define_name(1), "fLater");
    CHECK_INT(srcpos_define_value(1), 101);
    run_close(&r);
}

TEST(hex_rows_take_directive_line)
{
    static const char *src =
        HEAD_NO_VOC
        "/PRO 0\n"                          /* 11 */
        "> _ _ SET 5\n"                     /* 12 */
        "      #hex \"010203\"\n"           /* 13 */
        "      #dw 513\n"                   /* 14 */
        "      DONE\n"                      /* 15 */
        "/END\n";
    Run r;
    compile(&r, "srcpos_hex.dsf", src);
    CHECK_INT(r.rc, 0);

    CHECK_INT(srcpos_cond_count(0, 0), 7);
    check_pos(srcpos_cond(0, 0, 0), r.main_path, 12, 7);
    check_pos(srcpos_cond(0, 0, 1), r.main_path, 13, 7);
    check_pos(srcpos_cond(0, 0, 2), r.main_path, 13, 7);
    check_pos(srcpos_cond(0, 0, 3), r.main_path, 13, 7);
    check_pos(srcpos_cond(0, 0, 4), r.main_path, 14, 7);
    check_pos(srcpos_cond(0, 0, 5), r.main_path, 14, 7);
    check_pos(srcpos_cond(0, 0, 6), r.main_path, 15, 7);
    run_close(&r);
}

/* The order contract: counts follow the walk jsonexport.c makes
   (process 0..last, each slot's entries, each entry's condacts). */
TEST(counts_follow_json_walk)
{
    static const char *src =
        HEAD_VOC
        "/PRO 0\n"
        "> _ _ PROCESS 2\n"
        "$lbl\n"
        "> GET _\n> DROP _ SET 5\n  SKIP $lbl\n"
        "/PRO 2\n"
        "> _ _ #db 1\n  #userptr 3\n  DONE\n"
        "/PRO 0\n"
        "> _ _ CLS\n"
        "/END\n";
    ProcessTable *procs;
    long p, last;
    Run r;

    compile(&r, "srcpos_walk.dsf", src);
    CHECK_INT(r.rc, 0);

    procs = sintactic_processes();
    last = sintactic_last_process();
    CHECK_INT(last, 2);
    CHECK_INT(srcpos_proc_count(), last + 1);
    for (p = 0; p <= last; p++) {
        const ProcessSlot *slot = processtable_get(procs, p);
        size_t ne = vec_len_ProcessEntry(slot->entries), e;
        CHECK_INT(srcpos_entry_count((int)p), ne);
        for (e = 0; e < ne; e++) {
            const ProcessEntry *pe = vec_at_ProcessEntry(slot->entries, e);
            CHECK_INT(srcpos_cond_count((int)p, (int)e),
                      vec_len_ProcessCondact(pe->condacts));
        }
    }
    CHECK(srcpos_proc(1).file == NULL);
    CHECK_INT(srcpos_entry_count(1), 0);
    /* Process 0 is reopened: first /PRO keeps the position, entries
       append in parse order. */
    check_pos(srcpos_proc(0), r.main_path, 13, 6);
    CHECK_INT(srcpos_entry_count(0), 4);
    check_pos(srcpos_entry(0, 3), r.main_path, 24, 3);
    check_pos(srcpos_cond(0, 3, 0), r.main_path, 24, 7);
    run_close(&r);
}

int main(void)
{
    RUN(cond_positions_main_file);
    RUN(cond_positions_include);
    RUN(synonym_entries_get_own_lines);
    RUN(defines_only_user_names);
    RUN(hex_rows_take_directive_line);
    RUN(counts_follow_json_walk);
    return test_summary("srcpos");
}
