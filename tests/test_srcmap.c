/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_srcmap.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "back/srcmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TMP_DSM "srcmap_test_out.dsm"

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    size_t n;
    if (!f) return NULL;
    buf = malloc(65536);
    n = fread(buf, 1, 65535, f);
    buf[n] = '\0';
    fclose(f);
    if (len) *len = n;
    return buf;
}

TEST(crc32_check_value)
{
    CHECK_INT(srcmap_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u);
    CHECK_INT(srcmap_crc32((const uint8_t *)"", 0), 0);
}

TEST(writer_v2_layout)
{
    static const uint8_t ddb[4] = { 1, 2, 3, 4 };
    char want[512];
    char *got;
    size_t len = 0;
    SrcMap *m = srcmap_new();
    int f = srcmap_file(m, "GAME.DSF");
    CHECK_INT(f, 0);
    srcmap_proc(m, 0, f, 3);
    srcmap_entry(m, 0x0425, f, 5);
    srcmap_cond(m, 0x03F8, 0x0425, f, 5, 17);
    srcmap_cond(m, 0x03FB, 0x0425, f, 6, 17);
    srcmap_sym(m, "fCounter", 100);
    CHECK_INT(srcmap_write(m, TMP_DSM, ddb, sizeof ddb), 0);
    srcmap_free(m);

    snprintf(want, sizeof want,
             "NDRC-DSM\t2\n"
             "ddb\t%08lX\t4\n"
             "file\t0\tGAME.DSF\n"
             "proc\t0\t0\t3\n"
             "entry\t0425\t0\t5\n"
             "cond\t03F8\t0425\t0\t5\t17\n"
             "cond\t03FB\t0425\t0\t6\t17\n"
             "sym\tfCounter\t100\n",
             (unsigned long)srcmap_crc32(ddb, sizeof ddb));
    got = slurp(TMP_DSM, &len);
    CHECK(got != NULL);
    if (got) {
        CHECK_INT(len, strlen(want));
        CHECK_STR(got, want);
        free(got);
    }
    remove(TMP_DSM);
}

TEST(file_index_dedup)
{
    SrcMap *m = srcmap_new();
    CHECK_INT(srcmap_file(m, "A.DSF"), 0);
    CHECK_INT(srcmap_file(m, "B.DSF"), 1);
    CHECK_INT(srcmap_file(m, "A.DSF"), 0);
    srcmap_free(m);
}

TEST(same_offset_two_entries)
{
    static const uint8_t ddb[1] = { 0 };
    char *got;
    SrcMap *m = srcmap_new();
    int f = srcmap_file(m, "G.DSF");
    srcmap_cond(m, 0x0100, 0x0200, f, 1, 1);
    srcmap_cond(m, 0x0100, 0x0300, f, 2, 1);
    CHECK_INT(srcmap_write(m, TMP_DSM, ddb, sizeof ddb), 0);
    srcmap_free(m);
    got = slurp(TMP_DSM, NULL);
    CHECK(got != NULL);
    if (got) {
        CHECK(strstr(got, "cond\t0100\t0200\t0\t1\t1\n") != NULL);
        CHECK(strstr(got, "cond\t0100\t0300\t0\t2\t1\n") != NULL);
        free(got);
    }
    remove(TMP_DSM);
}

TEST(write_failure_reported)
{
    static const uint8_t ddb[1] = { 0 };
    SrcMap *m = srcmap_new();
    CHECK(srcmap_write(m, "no_such_dir_xyz/out.dsm", ddb, sizeof ddb) != 0);
    srcmap_free(m);
}

static void check_rel(const char *dsm, const char *file, const char *want)
{
    char *got = srcmap_rel_path(dsm, file);
    CHECK_STR(got, want);
    free(got);
}

TEST(rel_path_from_dsm_dir)
{
    check_rel("/a/b/x.dsm", "/a/b/g.dsf", "g.dsf");
    check_rel("/a/b/x.dsm", "/a/c/g.dsf", "../c/g.dsf");
    check_rel("/a/b/./x.dsm", "/a/b/../b/inc/i.dsf", "inc/i.dsf");
    check_rel("/a/b/c/x.dsm", "/g.dsf", "../../../g.dsf");
    check_rel("out/x.dsm", "g.dsf", "../g.dsf");
    check_rel("x.dsm", "sub\\g.dsf", "sub/g.dsf");
#ifdef _WIN32
    check_rel("C:\\a\\x.dsm", "c:/a/G.DSF", "G.DSF");
    check_rel("C:/a/x.dsm", "D:\\b\\g.dsf", "D:/b/g.dsf");
#endif
}

int main(void)
{
    RUN(rel_path_from_dsm_dir);
    RUN(crc32_check_value);
    RUN(writer_v2_layout);
    RUN(file_index_dedup);
    RUN(same_offset_two_entries);
    RUN(write_failure_reported);
    return test_summary("srcmap");
}
