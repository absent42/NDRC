/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/emit_ext.c - embedded externs emitter.
   Copyright (C) 2026 Dan Gibson.

   PORT: generateExterns, drb.php:105-129. Reads each entry's file from
   the CURRENT WORKING DIRECTORY (matching DRB's own relative fopen - no
   path handling, the same convention emit_xmb.c's XMB files use) and
   appends its bytes to `out` verbatim.

   model.h pre-splits each entry's FilePath ("path|TYPE") into
   file_path/file_type at JSON-parse time (model.c's
   split_extern_file_path), rather than here as drb.php:109-113 does it
   inline - this file works from the already-split ExternEntry fields
   and never re-parses a combined string of its own.

   ORDER FACT: see emit.h (emit_externs). */
#include "emit.h"

#include <stdio.h>
#include <string.h>

/* PORT: prettyFormat, drb.php:272-278 - same '0x%04lX' as main.c's
   fmt_addr (main.c:143-146); duplicated because LIB_SRC excludes
   main.c from this link set. */
static void fmt_extern_addr(char *buf, size_t n, long value)
{
    snprintf(buf, n, "0x%04lX", (unsigned long)value);
}

int emit_externs(Str *out, long *addr, Diag *d, Adventure *adv, const Target *t)
{
    size_t n = vec_len_ExternEntry(adv->externs);
    size_t i;

    (void)t;   /* drb.php's generateExterns takes no target parameter - see emit.h */

    for (i = 0; i < n; i++) {
        const ExternEntry *e = vec_at_ExternEntry(adv->externs, i);
        FILE *fh;
        long size;
        unsigned char *buf;
        char addr_buf[24];

        /* PORT: drb.php:114 file_exists + drb.php:115 fopen, folded into
           one fopen("rb") check - matching main.c's own file_exists()
           helper, which uses the identical fopen-then-fclose test. */
        fh = fopen(e->file_path, "rb");
        if (fh == NULL) {
            diag_fatal(d, "File not found: %s", e->file_path);
            return 0;
        }
        fseek(fh, 0, SEEK_END);
        size = ftell(fh);
        rewind(fh);
        buf = size > 0 ? arena_alloc(str_arena(out), (size_t)size) : NULL;
        if (size > 0) {
            size_t got = fread(buf, 1, (size_t)size, fh);
            /* PORT: drb.php:116 never checks fread()'s return; a PHP
               short read just returns fewer bytes, but a C short read
               here would emit uninitialised arena bytes verbatim - so
               fail loudly, owner-ruled 2026-08-26. */
            if (got != (size_t)size) {
                fclose(fh);
                diag_fatal(d, "Can't read file: %s", e->file_path);
                return 0;
            }
        }
        fclose(fh);

        /* PORT: drb.php:118 - appended BEFORE the type switch below, see
           the file header ORDER FACT. */
        str_append_n(out, buf, (size_t)size);

        /* PORT: drb.php:119-125. */
        if (strcmp(e->file_type, "EXTERN") == 0) {
            adv->extvec[0] = *addr;
        } else if (strcmp(e->file_type, "SFX") == 0) {
            adv->extvec[1] = *addr;
        } else if (strcmp(e->file_type, "INT") == 0) {
            adv->extvec[2] = *addr;
        } else {
            diag_fatal(d, "Invalid file type '%s' for file %s", e->file_type, e->file_path);
            return 0;
        }

        /* PORT: drb.php:126 - unconditional echo, diag_note's shape. */
        fmt_extern_addr(addr_buf, sizeof addr_buf, *addr);
        diag_note(d, "%s %s loaded at %s", e->file_type, e->file_path, addr_buf);

        /* PORT: drb.php:127. */
        *addr += size;
    }

    return 1;
}
