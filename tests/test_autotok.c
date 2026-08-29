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
    CHECK(copy_file(FIXTURE, dsf));

    /* --tok (bare) writes <input>.tok beside the input. */
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" --tok",
             dsf, out1);
    CHECK_INT(run_ndrc(args, logp), 0);
    CHECK(file_size(tok) > 0);

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

int main(void)
{
    RUN(auto_tokens_beats_builtin);
    RUN(tok_tee_roundtrips_to_identical_ddb);
    RUN(auto_tokens_overrides_tok_with_notice);
    return test_summary("autotok");
}
