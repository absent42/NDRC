/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_finish.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "arena.h"
#include "back/finish.h"
#include "diag.h"
#include "str.h"
#include "targets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* generateJDDB's C test doubles need a real path on disk -
   finish_write_jddb fopens it directly, so tmpfile() does not apply.
   Scratch files route through TMPDIR/TEMP/TMP, not the CWD, to avoid
   cruft in the repo root on a crashed run. */
static void scratch_path(char *buf, size_t bufsz, const char *filename)
{
    const char *dir = getenv("TMPDIR");
    if (dir == NULL) dir = getenv("TEMP");
    if (dir == NULL) dir = getenv("TMP");
    if (dir == NULL) dir = ".";
    snprintf(buf, bufsz, "%s/%s", dir, filename);
}

/* PORT: drb.php:1447-1494 prependPlus3HeaderToDDB. Expected checksum
   computed by hand from the header layout (drb.php:1454-1479) for a
   100-byte payload of 0xAA (fileSize = 100 + 128 = 228 = 0xE4): sum
   "PLUS3DOS" (605) + 0x1A,0x01,0x00 (27) + fileSize dword E4 00 00 00
   (228) + 0x03 (3) + size word 64 00 (100) + load addr word 00 84
   (132) + 107 zero filler bytes (0) = 1095; 1095 & 0xFF = 0x47. */
TEST(plus3_header_layout_and_checksum)
{
    Arena *a = arena_new(0);
    Str *ddb = str_new(a);
    const unsigned char *b;
    size_t i;

    for (i = 0; i < 100; i++) str_push_u8(ddb, 0xAA);
    finish_prepend_plus3(ddb);

    CHECK_INT(str_len(ddb), 228);
    b = str_bytes(ddb);
    CHECK_MEM(b, "PLUS3DOS", 8);
    CHECK_INT(b[8], 0x1A);
    CHECK_INT(b[9], 0x01);
    CHECK_INT(b[10], 0x00);
    CHECK_INT(b[11], 0xE4); CHECK_INT(b[12], 0x00);
    CHECK_INT(b[13], 0x00); CHECK_INT(b[14], 0x00);
    CHECK_INT(b[15], 0x03);
    CHECK_INT(b[16], 0x64); CHECK_INT(b[17], 0x00);
    CHECK_INT(b[18], 0x00); CHECK_INT(b[19], 0x84);
    for (i = 20; i < 127; i++) CHECK_INT(b[i], 0);
    CHECK_INT(b[127], 0x47);
    for (i = 0; i < 100; i++) CHECK_INT(b[128 + i], 0xAA);

    arena_free(a);
}

/* PORT: drb.php:1496-1512 prependC64HeaderToDDB / drb.php:1296 (C64) and
   drb.php:1295 (CP4) base addresses. */
TEST(c64_header_base_address_c64)
{
    Arena *a = arena_new(0);
    Str *ddb = str_new(a);
    const Target *t = target_lookup("C64", NULL);
    const unsigned char *b;

    CHECK(t != NULL);
    str_push_u8(ddb, 0x11); str_push_u8(ddb, 0x22); str_push_u8(ddb, 0x33);
    finish_prepend_c64(ddb, t->base_address);

    b = str_bytes(ddb);
    CHECK_INT(str_len(ddb), 5);
    CHECK_INT(b[0], 0x80); CHECK_INT(b[1], 0x38); /* 0x3880 LE */
    CHECK_INT(b[2], 0x11); CHECK_INT(b[3], 0x22); CHECK_INT(b[4], 0x33);

    arena_free(a);
}

TEST(c64_header_base_address_cp4)
{
    Arena *a = arena_new(0);
    Str *ddb = str_new(a);
    const Target *t = target_lookup("CP4", NULL);
    const unsigned char *b;

    CHECK(t != NULL);
    str_push_u8(ddb, 0x11); str_push_u8(ddb, 0x22); str_push_u8(ddb, 0x33);
    finish_prepend_c64(ddb, t->base_address);

    b = str_bytes(ddb);
    CHECK_INT(str_len(ddb), 5);
    CHECK_INT(b[0], 0x80); CHECK_INT(b[1], 0x70); /* 0x7080 LE */

    arena_free(a);
}

static FILE *scratch_open(void)
{
    return tmpfile();
}

static void scratch_read(FILE *f, char *buf, size_t n)
{
    size_t got;
    rewind(f);
    got = fread(buf, 1, n - 1, f);
    buf[got] = '\0';
}

static char *slurp_file(const char *path, size_t *out_len)
{
    static char buf[65536];
    FILE *f = fopen(path, "rb");
    size_t n;
    CHECK(f != NULL);
    if (f == NULL) { *out_len = 0; return buf; }
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    *out_len = n;
    return buf;
}

/* PORT: drb.php:1399-1445 generateJDDB. Format measured directly from
   a live DRB run on BLANK_EN/HTML: header "var DDBDATA = [\n",
   lowercase unpadded hex elements separated by commas, a
   trailing comma-less phantom "0x0" element after the real bytes
   (drb.php:1409-1419's post-EOF fgetc/feof interaction), footer "\n];"
   with no final newline. A 3-byte payload has too few elements to ever
   reach the i%10==9 offset-comment case. */
TEST(jddb_three_byte_payload_matches_measured_format)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *note_f = scratch_open();
    unsigned char payload[3] = { 0x61, 0x62, 0x63 };
    char ddb_path[512], jddb_path[512];
    char notebuf[256];
    size_t len;
    char *content;
    int rc;

    scratch_path(ddb_path, sizeof ddb_path, "test_finish_abc.ddb");
    scratch_path(jddb_path, sizeof jddb_path, "test_finish_abc.jddb");

    diag_set_stream(d, note_f);
    remove(jddb_path);

    rc = finish_write_jddb(d, ddb_path, payload, sizeof payload);
    CHECK_INT(rc, 1);

    scratch_read(note_f, notebuf, sizeof notebuf);
    CHECK_STR(notebuf, "Converting DDB to JDDB\n");

    content = slurp_file(jddb_path, &len);
    CHECK_STR(content, "var DDBDATA = [\n0x61,0x62,0x63,0x0\n];");

    remove(jddb_path);
    fclose(note_f);
    arena_free(a);
}

/* PORT: drb.php:1402-1403. "GAME.DDB" lowercases to "game.ddb", and its
   ".ddb" substring becomes ".jddb": "game.jddb". */
TEST(jddb_name_lowercased_and_ddb_replaced)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *note_f = scratch_open();
    FILE *check_f;
    unsigned char payload[1] = { 0x00 };
    char ddb_path[512], jddb_path[512];
    int rc;

    scratch_path(ddb_path, sizeof ddb_path, "GAME.DDB");
    scratch_path(jddb_path, sizeof jddb_path, "game.jddb");

    diag_set_stream(d, note_f);
    remove(jddb_path);

    rc = finish_write_jddb(d, ddb_path, payload, sizeof payload);
    CHECK_INT(rc, 1);
    check_f = fopen(jddb_path, "rb");
    CHECK(check_f != NULL);
    if (check_f != NULL) fclose(check_f);

    remove(jddb_path);
    fclose(note_f);
    arena_free(a);
}

/* PORT: drb.php:1402-1403's quirk (task-7-brief.md Step 3(d)): an output
   name with no ".ddb" substring after lowercasing passes through
   unchanged, so the "JDDB" is written back over the SAME name. */
TEST(jddb_name_unchanged_when_no_ddb_substring)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *note_f = scratch_open();
    FILE *check_f;
    unsigned char payload[1] = { 0x00 };
    char bin_path[512];
    int rc;

    scratch_path(bin_path, sizeof bin_path, "test_finish_game.bin");

    diag_set_stream(d, note_f);
    remove(bin_path);

    rc = finish_write_jddb(d, bin_path, payload, sizeof payload);
    CHECK_INT(rc, 1);
    check_f = fopen(bin_path, "rb");
    CHECK(check_f != NULL);
    if (check_f != NULL) fclose(check_f);

    remove(bin_path);
    fclose(note_f);
    arena_free(a);
}

int main(void)
{
    RUN(plus3_header_layout_and_checksum);
    RUN(c64_header_base_address_c64);
    RUN(c64_header_base_address_cp4);
    RUN(jddb_three_byte_payload_matches_measured_format);
    RUN(jddb_name_lowercased_and_ddb_replaced);
    RUN(jddb_name_unchanged_when_no_ddb_substring);
    return test_summary("finish");
}
