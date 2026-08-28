/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/finish.c - -ch/-3h header prepends and HTML JDDB generation.
   Copyright (C) 2026 Dan Gibson.

   PORT: prependC64HeaderToDDB (drb.php:1496-1512), prependPlus3HeaderToDDB
   (drb.php:1447-1494) and generateJDDB (drb.php:1399-1445). See finish.h
   for the per-function PORT notes and finish.h/main.c for call order. */
#include "finish.h"

#include <stdio.h>
#include <string.h>

void finish_prepend_c64(Str *ddb, long base_address)
{
    Str *tmp = str_new(str_arena(ddb));
    unsigned base = (unsigned)base_address;

    /* drb.php:1500-1502 - two-byte little-endian base address, then the
       original DDB bytes. */
    str_push_u8(tmp, base & 0xFFu);
    str_push_u8(tmp, (base >> 8) & 0xFFu);
    str_append_n(tmp, str_bytes(ddb), str_len(ddb));

    str_assign(ddb, tmp);
}

void finish_prepend_plus3(Str *ddb)
{
    Str *tmp = str_new(str_arena(ddb));
    size_t orig_len = str_len(ddb);
    unsigned long file_size = (unsigned long)orig_len + 128uL; /* drb.php:1450 */
    unsigned checksum;
    size_t i;

    str_append_n(tmp, "PLUS3DOS", 8);           /* drb.php:1455-1462 */
    str_push_u8(tmp, 0x1A);                     /* drb.php:1463 soft EOF */
    str_push_u8(tmp, 0x01);                     /* drb.php:1464 issue */
    str_push_u8(tmp, 0x00);                     /* drb.php:1465 version */
    str_push_u8(tmp, file_size & 0xFFu);         /* drb.php:1466-1469 */
    str_push_u8(tmp, (file_size >> 8) & 0xFFu);
    str_push_u8(tmp, (file_size >> 16) & 0xFFu);
    str_push_u8(tmp, (file_size >> 24) & 0xFFu);
    str_push_u8(tmp, 0x03);                     /* drb.php:1470 - "Bytes:" */
    file_size -= 128uL;                          /* drb.php:1471 - original size back */
    str_push_u8(tmp, file_size & 0xFFu);         /* drb.php:1472-1473 */
    str_push_u8(tmp, (file_size >> 8) & 0xFFu);
    str_push_u8(tmp, 0x00);                     /* drb.php:1474-1475 load addr 0x8400 */
    str_push_u8(tmp, 0x84);
    while (str_len(tmp) < 127u) str_push_u8(tmp, 0); /* drb.php:1476 fillers */

    checksum = 0;
    for (i = 0; i < 127u; i++) checksum += str_bytes(tmp)[i]; /* drb.php:1477-1478 */
    str_push_u8(tmp, checksum & 0xFFu);          /* drb.php:1479 */

    str_append_n(tmp, str_bytes(ddb), orig_len); /* drb.php:1484-1489 */

    str_assign(ddb, tmp);
}

/* PORT: drb.php:1412/1431 - one array element: lowercase, unpadded hex,
   a trailing comma unless this is the very last element (the phantom
   post-EOF one, drb.php:1409-1419's feof/fgetc interaction - see finish.h),
   then an offset comment every 10th element (i%10==9, lowercase 4-digit
   zero-padded, drb.php:1415-1416). */
static void write_jddb_element(FILE *out_fh, unsigned val, int with_comma, long i)
{
    fprintf(out_fh, "0x%lx", (unsigned long)val);
    if (with_comma) fputc(',', out_fh);
    if (i % 10 == 9) fprintf(out_fh, "// 0x%04lx\n", (unsigned long)i);
}

/* PORT: drb.php:1408-1419/1427-1438 - the shared array-emission loop, over
   an in-memory buffer (the DDB main.c holds; never streamed through a
   FILE* in this port - see main.c's fwrite PORT NOTE). C's feof() has the
   same "still false right after the last successful read" timing as
   PHP's, so the phantom trailing "0x0" element (comma-less: feof is true
   by the time PHP's comma check runs) falls out of one extra iteration
   here too - measured against a live DRB run (task-7-report.md), not
   inferred from the PHP text alone. */
static void write_jddb_array(FILE *out_fh, const unsigned char *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        write_jddb_element(out_fh, data[i], 1, (long)i);
    }
    write_jddb_element(out_fh, 0, 0, (long)len);
}

/* PORT: drb.php:1427-1438 - identical loop, but streamed straight off a
   FILE* (0.XMB) rather than an in-memory buffer, since nothing upstream
   of this port loads XMB files into memory. A literal transliteration of
   the PHP's own fgetc()/feof() shape, which C's fgetc()/feof() time the
   same way (see write_jddb_array), so it reduces to the same element
   sequence without needing the file's length up front. */
static void write_jddb_array_from_file(FILE *out_fh, FILE *in_fh)
{
    long i = 0;
    int c;
    for (;;) {
        c = fgetc(in_fh);
        if (c == EOF) {
            write_jddb_element(out_fh, 0, 0, i);
            break;
        }
        write_jddb_element(out_fh, (unsigned char)c, 1, i);
        i++;
    }
}

#define FINISH_PATH_MAX 4096

static int lowercase_into(const char *s, char *out, size_t outsz)
{
    size_t i, n = strlen(s);
    if (n + 1 > outsz) return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[n] = '\0';
    return 1;
}

/* PORT: drb.php:1402-1403 - str_replace replaces EVERY '.ddb'
   occurrence post-lowercase; a path with none comes back unchanged.
   Bounded stack buffer - no Arena in this signature. */
static int derive_jddb_path(const char *output_path, char *out, size_t outsz)
{
    char lower[FINISH_PATH_MAX];
    size_t i, j, n;

    if (!lowercase_into(output_path, lower, sizeof lower)) return 0;
    n = strlen(lower);
    for (i = 0, j = 0; i < n; ) {
        if (i + 4 <= n && memcmp(lower + i, ".ddb", 4) == 0) {
            if (j + 5 >= outsz) return 0;
            memcpy(out + j, ".jddb", 5);
            j += 5;
            i += 4;
        } else {
            if (j + 1 >= outsz) return 0;
            out[j++] = lower[i];
            i++;
        }
    }
    out[j] = '\0';
    return 1;
}

int finish_write_jddb(Diag *d, const char *output_path,
                      const unsigned char *ddb, size_t len)
{
    char jddb_path[FINISH_PATH_MAX];
    FILE *out_fh;
    FILE *xmb_fh;

    /* drb.php:1401 - unconditional, before anything else in the
       function, including before the (never-checked) fopen below. */
    diag_note(d, "Converting DDB to JDDB");

    if (!derive_jddb_path(output_path, jddb_path, sizeof jddb_path)) {
        diag_fatal(d, "Can't create output file");
        return 0;
    }

    /* PORT NOTE: drb.php:1405 never checks this fopen(); a failure there
       would carry on writing through a null handle (a PHP warning per
       call, not a fatal). Failing loudly here instead is a deliberate,
       gate-invisible deviation (finish.h). */
    out_fh = fopen(jddb_path, "wb");
    if (out_fh == NULL) {
        diag_fatal(d, "Can't create output file");
        return 0;
    }

    fputs("var DDBDATA = [\n", out_fh);   /* drb.php:1407 */
    write_jddb_array(out_fh, ddb, len);    /* drb.php:1409-1419 */
    fputs("\n];", out_fh);                 /* drb.php:1420 */

    /* drb.php:1423-1441 - dormant for every HTML fixture this phase (none
       carries XMESSAGEs), but ported: emit_xmessages (emit.h) writes
       "0.XMB" into the current working directory when it runs, and this
       mirrors DRB's own file_exists() check the same way. */
    xmb_fh = fopen("0.XMB", "rb");
    if (xmb_fh != NULL) {
        fputs("var XMBDATA = [\n", out_fh);
        write_jddb_array_from_file(out_fh, xmb_fh);
        fputs("\n];", out_fh);
        fclose(xmb_fh);
    }

    fclose(out_fh);
    return 1;
}
