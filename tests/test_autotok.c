/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_autotok.c - end-to-end tests for -auto-tokens / --tok.
   Runs the built ndrc binary from the repo root (how `make test`
   invokes test binaries). Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define NDRC "ndrc.exe"
#else
#define NDRC "./ndrc"
#include <sys/wait.h>
#endif

#define FIXTURE "tests/fixtures/AUTOTOK.DSF"
#define OBJSHARE "tests/fixtures/OBJSHARE.DSF"

static void scratch_path(char *buf, size_t bufsz, const char *filename)
{
    const char *dir = getenv("TMPDIR");
    if (dir == NULL) dir = getenv("TEMP");
    if (dir == NULL) dir = getenv("TMP");
    if (dir == NULL) dir = ".";
    snprintf(buf, bufsz, "%s/%s", dir, filename);
}

static int run_ndrc(const char *args, const char *logpath)
{
    char cmd[2048];
    int rc;

    snprintf(cmd, sizeof cmd, NDRC " %s > \"%s\" 2>&1", args, logpath);
    rc = system(cmd);
#ifndef _WIN32
    if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    return rc;
}

static long file_size(const char *p)
{
    FILE *f = fopen(p, "rb");
    long n;
    if (f == NULL) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static int files_equal(const char *pa, const char *pb)
{
    FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb");
    int ca, cb, eq = 1;

    if (fa == NULL || fb == NULL) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return 0;
    }
    do {
        ca = fgetc(fa);
        cb = fgetc(fb);
        if (ca != cb) { eq = 0; break; }
    } while (ca != EOF);
    fclose(fa);
    fclose(fb);
    return eq;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *fs = fopen(src, "rb"), *fd = fopen(dst, "wb");
    int c;

    if (fs == NULL || fd == NULL) {
        if (fs) fclose(fs);
        if (fd) fclose(fd);
        return 0;
    }
    while ((c = fgetc(fs)) != EOF) fputc(c, fd);
    fclose(fs);
    return fclose(fd) == 0;
}

static int log_contains(const char *logpath, const char *needle)
{
    FILE *f = fopen(logpath, "rb");
    char line[1024];
    int found = 0;

    if (f == NULL) return 0;
    while (fgets(line, sizeof line, f) != NULL)
        if (strstr(line, needle) != NULL) { found = 1; break; }
    fclose(f);
    return found;
}

/* 1-based line of the first match, -1 if absent. */
static int log_line_of(const char *logpath, const char *needle)
{
    FILE *f = fopen(logpath, "rb");
    char line[1024];
    int n = 0, at = -1;

    if (f == NULL) return -1;
    while (fgets(line, sizeof line, f) != NULL) {
        n++;
        if (strstr(line, needle) != NULL) { at = n; break; }
    }
    fclose(f);
    return at;
}

static void write_text(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (f != NULL) { fputs(content, f); fclose(f); }
}

/* Whole file into buf; NULL on failure or overflow. */
static unsigned char *read_ddb(const char *path, unsigned char *buf,
                               size_t bufsz, long *len)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL) return NULL;
    n = fread(buf, 1, bufsz, f);
    fclose(f);
    *len = (long)n;
    return n > 0 && n < bufsz ? buf : NULL;
}

static unsigned ddb_word(const unsigned char *ddb, long at)
{
    return (unsigned)ddb[at] | ((unsigned)ddb[at + 1] << 8);
}

/* Table kind: 0 sysmess, 1 messages, 2 locations, 3 objects. */
static int ddb_count(const unsigned char *ddb, int kind)
{
    return ddb[6 - kind];
}

static unsigned ddb_lookup_word(const unsigned char *ddb, int kind, int index)
{
    return ddb_word(ddb, ddb_word(ddb, 0x12 - 2 * kind) + 2L * index);
}

/* Decodes entry `index` of table `kind` via its lookup word, expanding
   tokens. Header: count at 6-kind, lookup pointer at 0x12-2*kind,
   tokens at 0x08; text bytes XOR 0xFF, 0x0A ends a text. */
static int ddb_text(const unsigned char *ddb, long len, int kind, int index,
                    char *out, size_t outsz)
{
    long p = ddb_lookup_word(ddb, kind, index), t;
    size_t o = 0;
    int entry;

    if (index >= ddb_count(ddb, kind)) return 0;
    for (;;) {
        unsigned b;
        if (p >= len) return 0;
        b = ddb[p++] ^ 0xFFu;
        if (b == 0x0A) break;
        if (b < 128) {
            if (o + 1 >= outsz) return 0;
            out[o++] = (char)b;
            continue;
        }
        /* Token entry (b - 127); entry 0 is the first in the table. */
        t = ddb_word(ddb, 0x08);
        for (entry = 0; entry < (int)b - 127; entry++) {
            while (t < len && !(ddb[t] & 0x80)) t++;
            t++;
        }
        do {
            if (t >= len || o + 1 >= outsz) return 0;
            out[o++] = (char)(ddb[t] & 0x7F);
        } while (!(ddb[t++] & 0x80));
    }
    out[o] = '\0';
    return 1;
}

static int ddb_has_ref_byte(const unsigned char *ddb, long len, int kind,
                            int index)
{
    long p = ddb_lookup_word(ddb, kind, index);

    for (; p < len; p++) {
        unsigned b = ddb[p] ^ 0xFFu;
        if (b == 0x0A) return 0;
        if (b >= 128) return 1;
    }
    return 0;
}

TEST(auto_tokens_beats_builtin)
{
    char out0[512], out1[512], logp[512], args[1600];
    long s0, s1;

    scratch_path(out0, sizeof out0, "at_base.ddb");
    scratch_path(out1, sizeof out1, "at_auto.ddb");
    scratch_path(logp, sizeof logp, "at_log.txt");

    snprintf(args, sizeof args, "NEXTDAAD EN %s \"%s\"", FIXTURE, out0);
    CHECK_INT(run_ndrc(args, logp), 0);
    snprintf(args, sizeof args, "NEXTDAAD EN %s \"%s\" -auto-tokens",
             FIXTURE, out1);
    CHECK_INT(run_ndrc(args, logp), 0);

    s0 = file_size(out0);
    s1 = file_size(out1);
    CHECK(s0 > 0);
    CHECK(s1 > 0);
    /* Spec margin: selected table beats builtin by more than 3%. */
    CHECK(s1 * 100 <= s0 * 97);
}

TEST(tok_tee_roundtrips_to_identical_ddb)
{
    char dsf[512], tok[512], out1[512], out2[512], logp[512], args[1600];

    scratch_path(dsf, sizeof dsf, "at_rt.DSF");
    scratch_path(tok, sizeof tok, "at_rt.tok");
    scratch_path(out1, sizeof out1, "at_rt1.ddb");
    scratch_path(out2, sizeof out2, "at_rt2.ddb");
    scratch_path(logp, sizeof logp, "at_rt_log.txt");
    remove(tok);
    CHECK(copy_file(OBJSHARE, dsf));

    /* --tok (bare) writes <input>.tok beside the input. */
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" --tok",
             dsf, out1);
    CHECK_INT(run_ndrc(args, logp), 0);
    CHECK(file_size(tok) > 0);

    /* Phase 2: the tee records its encoder. */
    CHECK(log_contains(tok, "\"encoder\": \"optimal\""));

    /* No flag: the override lookup finds the tee and must reproduce
       the identical DDB. */
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\"", dsf, out2);
    CHECK_INT(run_ndrc(args, logp), 0);
    CHECK(files_equal(out1, out2));
    remove(tok);
}

TEST(auto_tokens_overrides_tok_with_notice)
{
    char dsf[512], tok[512], out1[512], out2[512], logp[512], args[1600];

    scratch_path(dsf, sizeof dsf, "at_ov.DSF");
    scratch_path(tok, sizeof tok, "at_ov.tok");
    scratch_path(out1, sizeof out1, "at_ov1.ddb");
    scratch_path(out2, sizeof out2, "at_ov2.ddb");
    scratch_path(logp, sizeof logp, "at_ov_log.txt");
    remove(tok);
    CHECK(copy_file(FIXTURE, dsf));

    /* Plant a .tok, then compile with -auto-tokens: the flag must win
       and say so. */
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" --tok",
             dsf, out1);
    CHECK_INT(run_ndrc(args, logp), 0);
    CHECK(file_size(tok) > 0);

    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -auto-tokens",
             dsf, out2);
    CHECK_INT(run_ndrc(args, logp), 0);
    CHECK(log_contains(logp, "Warning: -auto-tokens overrides tokens file"));
    /* Same input, same selector: byte-identical result either way. */
    CHECK(files_equal(out1, out2));
    remove(tok);
}

TEST(marker_engages_optimal_encoder)
{
    /* Same token table, marker on vs off. Tokens ordered so the
       sequential encoder provably loses: "th" (7468) before "the"
       (746865) makes every "the" cost 2 bytes sequentially but 1
       optimally, and the fixture's prose is rich in "the". */
    char dsf[512], tok[512], out_seq[512], out_opt[512], logp[512];
    char args[1600];

    scratch_path(dsf, sizeof dsf, "at_mk.DSF");
    scratch_path(tok, sizeof tok, "at_mk.tok");
    scratch_path(out_seq, sizeof out_seq, "at_mk_seq.ddb");
    scratch_path(out_opt, sizeof out_opt, "at_mk_opt.ddb");
    scratch_path(logp, sizeof logp, "at_mk_log.txt");
    remove(tok);
    CHECK(copy_file(FIXTURE, dsf));

    write_text(tok, "{\"compression\": \"advanced\", "
                    "\"tokens\": [\"00\",\"7468\",\"746865\"]}");
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\"", dsf, out_seq);
    CHECK_INT(run_ndrc(args, logp), 0);

    write_text(tok, "{\"compression\": \"advanced\", "
                    "\"encoder\": \"optimal\", "
                    "\"tokens\": [\"00\",\"7468\",\"746865\"]}");
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\"", dsf, out_opt);
    CHECK_INT(run_ndrc(args, logp), 0);

    CHECK(file_size(out_seq) > 0);
    CHECK(file_size(out_opt) > 0);
    CHECK(file_size(out_opt) < file_size(out_seq));
    remove(tok);
}

static unsigned char ddb_ctl[65536], ddb_auto[65536];

TEST(objects_compressed_and_duplicates_shared)
{
    char out0[512], out1[512], logp[512], args[1600];
    char t0[512], t1[512];
    long n0 = 0, n1 = 0;
    int kind, i;

    scratch_path(out0, sizeof out0, "os_ctl.ddb");
    scratch_path(out1, sizeof out1, "os_auto.ddb");
    scratch_path(logp, sizeof logp, "os_log.txt");

    snprintf(args, sizeof args, "NEXTDAAD EN %s \"%s\"", OBJSHARE, out0);
    CHECK_INT(run_ndrc(args, logp), 0);
    snprintf(args, sizeof args, "NEXTDAAD EN %s \"%s\" -auto-tokens",
             OBJSHARE, out1);
    CHECK_INT(run_ndrc(args, logp), 0);
    CHECK(read_ddb(out0, ddb_ctl, sizeof ddb_ctl, &n0) != NULL);
    CHECK(read_ddb(out1, ddb_auto, sizeof ddb_auto, &n1) != NULL);
    if (n0 == 0 || n1 == 0) return;

    CHECK(ddb_has_ref_byte(ddb_auto, n1, 3, 0));
    CHECK_INT(ddb_lookup_word(ddb_auto, 3, 0), ddb_lookup_word(ddb_auto, 3, 3));
    CHECK_INT(ddb_lookup_word(ddb_auto, 1, 7), ddb_lookup_word(ddb_auto, 1, 8));
    CHECK_INT(ddb_lookup_word(ddb_auto, 1, 9), ddb_lookup_word(ddb_auto, 1, 10));
    CHECK_INT(ddb_lookup_word(ddb_auto, 2, 0), ddb_lookup_word(ddb_auto, 2, 2));
    CHECK_INT(ddb_lookup_word(ddb_auto, 0, 63), ddb_lookup_word(ddb_auto, 0, 64));

    for (kind = 0; kind < 4; kind++) {
        CHECK_INT(ddb_count(ddb_auto, kind), ddb_count(ddb_ctl, kind));
        for (i = 0; i < ddb_count(ddb_ctl, kind); i++) {
            t0[0] = t1[0] = '\0';
            CHECK(ddb_text(ddb_ctl, n0, kind, i, t0, sizeof t0));
            CHECK(ddb_text(ddb_auto, n1, kind, i, t1, sizeof t1));
            CHECK_STR(t1, t0);
        }
    }
}

TEST(flag_off_rows_untouched)
{
    char out0[512], logp[512], args[1600];
    long n0 = 0;
    int kind, i, j;

    scratch_path(out0, sizeof out0, "os_off.ddb");
    scratch_path(logp, sizeof logp, "os_off_log.txt");

    snprintf(args, sizeof args, "NEXTDAAD EN %s \"%s\"", OBJSHARE, out0);
    CHECK_INT(run_ndrc(args, logp), 0);
    CHECK(read_ddb(out0, ddb_ctl, sizeof ddb_ctl, &n0) != NULL);
    if (n0 == 0) return;

    for (i = 0; i < ddb_count(ddb_ctl, 3); i++)
        CHECK(!ddb_has_ref_byte(ddb_ctl, n0, 3, i));
    for (kind = 0; kind < 4; kind++)
        for (i = 0; i < ddb_count(ddb_ctl, kind); i++)
            for (j = i + 1; j < ddb_count(ddb_ctl, kind); j++)
                CHECK(ddb_lookup_word(ddb_ctl, kind, i) !=
                      ddb_lookup_word(ddb_ctl, kind, j));
}

TEST(verbose_reports_objects_and_shares)
{
    char out1[512], logp[512], args[1600];

    scratch_path(out1, sizeof out1, "os_v.ddb");
    scratch_path(logp, sizeof logp, "os_v_log.txt");

    snprintf(args, sizeof args, "NEXTDAAD EN %s \"%s\" -auto-tokens -v",
             OBJSHARE, out1);
    CHECK_INT(run_ndrc(args, logp), 0);
    CHECK(log_contains(logp, "Object texts compressed."));
    CHECK(log_contains(logp, "Shared 1 duplicate texts in objects ("));
}

TEST(tee_written_only_after_file_check)
{
    /* Positive path: the tee follows a passing file check. The failing
       path is unit-tested in test_tokselect.c. */
    char dsf[512], tok[512], out1[512], logp[512], args[1600];

    scratch_path(dsf, sizeof dsf, "fc_tee.DSF");
    scratch_path(tok, sizeof tok, "fc_tee.tok");
    scratch_path(out1, sizeof out1, "fc_tee.ddb");
    scratch_path(logp, sizeof logp, "fc_tee_log.txt");
    remove(tok);
    remove(out1);
    CHECK(copy_file(OBJSHARE, dsf));

    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" --tok -v",
             dsf, out1);
    CHECK_INT(run_ndrc(args, logp), 0);
    CHECK(file_size(tok) > 0);
    CHECK(file_size(out1) > 0);
    CHECK(!log_contains(logp, "self-check failed"));
    /* The check runs once the location lookup is out. */
    CHECK(log_line_of(logp, "Locations         ") > 0);
    CHECK(log_line_of(logp, "Tokens written to") >
          log_line_of(logp, "Locations         "));
    remove(tok);
}

/* The container warning line holds the plain text, no reference byte. */
static int container_warning_ok(const char *logpath, const char *text)
{
    FILE *f = fopen(logpath, "rb");
    char line[1024];
    const unsigned char *c;
    int ok = 0;

    if (f == NULL) return 0;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strstr(line, "is a container.") == NULL) continue;
        for (c = (const unsigned char *)line; *c != '\0'; c++)
            if (*c >= 0x80) { fclose(f); return 0; }
        if (strstr(line, text) != NULL) ok = 1;
    }
    fclose(f);
    return ok;
}

TEST(container_warning_prints_source_text)
{
    char out0[512], logp[512], args[1600];
    const char *want = "Warning: object #0 (a brass lantern of the old kind) "
                       "is a container.";

    scratch_path(out0, sizeof out0, "cw.ddb");
    scratch_path(logp, sizeof logp, "cw_log.txt");

    snprintf(args, sizeof args, "NEXTDAAD EN %s \"%s\"", OBJSHARE, out0);
    CHECK_INT(run_ndrc(args, logp), 0);
    CHECK(log_contains(logp, "has a description."));
    CHECK(container_warning_ok(logp, want));

    snprintf(args, sizeof args, "NEXTDAAD EN %s \"%s\" -auto-tokens",
             OBJSHARE, out0);
    CHECK_INT(run_ndrc(args, logp), 0);
    CHECK(log_contains(logp, "has a description."));
    CHECK(container_warning_ok(logp, want));
}

int main(void)
{
    RUN(auto_tokens_beats_builtin);
    RUN(tok_tee_roundtrips_to_identical_ddb);
    RUN(auto_tokens_overrides_tok_with_notice);
    RUN(marker_engages_optimal_encoder);
    RUN(objects_compressed_and_duplicates_shared);
    RUN(flag_off_rows_untouched);
    RUN(verbose_reports_objects_and_shares);
    RUN(tee_written_only_after_file_check);
    RUN(container_warning_prints_source_text);
    return test_summary("autotok");
}
