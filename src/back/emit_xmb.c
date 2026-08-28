/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/emit_xmb.c - XMessages (.XMB) file emitter.
   Copyright (C) 2026 Dan Gibson.

   PORT: drb.php:449-524 generateXMessages, plus its size-table helper
   getXMessageFileSizeByTarget (drb.php:419-446, already transcribed as
   the Target.xmessage_size_k column - targets.c). Writes one or more
   ".XMB" files into the CURRENT WORKING DIRECTORY, exactly as DRB does
   (relative fopen, no path handling).

   Arena note: this function's signature (emit.h) carries no Arena
   parameter of its own - it borrows the arena already backing
   adv->xmessages (vec_arena, vec.h), matching the
   project's arena-only allocation rule without widening the interface
   the brief specifies verbatim. adv->xmessages is always non-empty
   whenever this function runs (main.c's own `if (vec_len_Message(...))` guard,
   drb.php:1920), so that arena pointer is always valid here. */
#include "emit.h"

#include <stdio.h>
#include <string.h>

#define OFUSCATE_VALUE 0xFFu

/* PORT: drb.php:457/489 - the 2K container's file naming prefixes a
   zero ("0%d") for indices < 10, so the first 2K file is "00.XMB"
   while the first 64K (or 16K) file is "0.XMB". This is a DISPLAY/
   naming-only quirk: the numeric file index used for offset arithmetic
   (currentOffset + fileIndex * maxFileSize, drb.php:501) is unaffected -
   see emit_xmessages below, which keeps file_index as a plain long
   throughout and only feeds it through this formatting rule when
   building a filename. */
static void xmb_filename(char *buf, size_t bufsz, long file_index, long max_file_size)
{
    if (max_file_size == 2048 && file_index < 10) {
        snprintf(buf, bufsz, "0%ld.XMB", file_index);
    } else {
        snprintf(buf, bufsz, "%ld.XMB", file_index);
    }
}

/* PORT: drb.php:98-101 writeBlock - n literal zero bytes. */
static void write_zero_block(FILE *fh, long n)
{
    long i;
    for (i = 0; i < n; i++) fputc(0, fh);
}

int emit_xmessages(Diag *d, const Target *t, Adventure *adv)
{
    long current_offset = 0;
    long current_file = 0;
    long max_file_size_k = (long)t->xmessage_size_k;
    long max_file_size;
    char filename[32];
    FILE *fh;
    size_t n = vec_len_Message(adv->xmessages);
    size_t i;
    Arena *arena = vec_arena_Message(adv->xmessages);
    long *offsets;
    const char *subtarget = t->subtarget ? t->subtarget : "";
    int is_amiga = strcmp(t->name, "AMIGA") == 0;
    int is_plus3 = strcmp(subtarget, "PLUS3") == 0;
    int is_128k = strcmp(subtarget, "128K") == 0;
    int is_msx2 = strcmp(t->name, "MSX2") == 0;

    /* PORT: drb.php:453-454 - $GLOBALS['maxFileSizeForXMessages'] is set
       BEFORE the unsupported-target check below, so it is recorded even
       on the error path; ported the same way (harmless here since the
       caller halts on a 0 return either way, but matches call order). */
    adv->xmessage_max_k = max_file_size_k;

    /* PORT: drb.php:455. Note the spaces inside the brackets and that a
       bare target (no subtarget) renders with an empty subtarget:
       "[ NEXTDAAD  ]", two spaces before the closing bracket. */
    if (max_file_size_k == 0) {
        diag_fatal(d, "XMessages are not supported by target [ %s %s ]", t->name, subtarget);
        return 0;
    }
    max_file_size = max_file_size_k * 1024;   /* drb.php:456 */

    xmb_filename(filename, sizeof filename, current_file, max_file_size);
    fh = fopen(filename, "wb");
    /* PORT: drb.php:459 never checks fopen(); a failed handle just
       silently no-ops on every fputs() (verified live: fopenfail.php
       via php.exe, exit code 0). Failing loudly here instead is a
       deliberate, gate-invisible deviation, owner-ruled 2026-08-26. */
    if (fh == NULL) {
        diag_fatal(d, "Can't create output XMB file");
        return 0;
    }

    /* PORT: drb.php:461-471 - the +3/AMIGA 512-byte leading gap. */
    if (is_plus3 || is_amiga) {
        write_zero_block(fh, 512);
        current_offset += 512;
    }

    offsets = n ? arena_alloc(arena, n * sizeof(long)) : NULL;

    for (i = 0; i < n; i++) {
        const Message *m = vec_at_Message(adv->xmessages, i);
        size_t message_length = str_len(m->Text);
        const unsigned char *bytes = str_bytes(m->Text);
        /* PORT: drb.php:478 - PLUS3/128K should fit 512 bytes in the
           current file (see the +3 interpreter's page-swap scheme). */
        int should_fit_512 = is_plus3 || is_128k;
        size_t j;

        /* PORT: drb.php:479-499 - rolling to the next file (or, for
           MSX2/PLUS3/128K, padding the current file and continuing as
           one physical file addressed in maxFileSize-sized pages). */
        if (((long)message_length + current_offset + 1 > max_file_size) ||
            (should_fit_512 && (current_offset + 512 > max_file_size))) {
            if (!is_msx2 && !is_plus3 && !is_128k) {
                fclose(fh);
                current_file++;
                current_offset = 0;
                xmb_filename(filename, sizeof filename, current_file, max_file_size);
                fh = fopen(filename, "wb");
                /* PORT NOTE (memory-safety, not a defect port): the
                   roll-over sibling of the guard above - drb.php:491
                   never checks this fopen() either, same silent-no-op
                   consequence. See that guard's PORT NOTE. */
                if (fh == NULL) {
                    diag_fatal(d, "Can't create output XMB file");
                    return 0;
                }
            } else {
                write_zero_block(fh, max_file_size - current_offset);
                current_file++;
                current_offset = 0;
                adv->xmessage_padding = max_file_size;
            }
        }

        /* PORT: drb.php:501 - the NUMERIC file index, not the possibly
           string-formatted display name above. */
        offsets[i] = current_offset + current_file * max_file_size;

        for (j = 0; j < message_length; j++) {
            fputc((int)(bytes[j] ^ OFUSCATE_VALUE), fh);
            current_offset++;
        }
        /* PORT: drb.php:509 - mark of end of string, "\n"^0xFF = 0xF5. */
        fputc((int)(0x0Au ^ OFUSCATE_VALUE), fh);
        current_offset++;
    }

    /* PORT: drb.php:512-518 - the Amiga trailing 512-byte gap. */
    if (is_amiga) {
        write_zero_block(fh, 512);
        current_offset += 512;
    }

    fclose(fh);

    /* PORT: drb.php:521-522, the two 64K errors verbatim. Note these
       check only the LAST file's currentOffset, not a cumulative total
       across every file written - ported exactly as DRB has it. */
    if (is_amiga && current_offset > 65535 + 512) {
        diag_fatal(d, "XMessages data exceeds 64K including Amiga gap of 512 bytes");
        return 0;
    }
    if (!is_amiga && current_offset > 65535) {
        diag_fatal(d, "XMessages data exceeds 64K");
        return 0;
    }

    adv->xmessage_offsets = offsets;
    /* PORT: drb.php:523. */
    adv->xmessage_size = max_file_size * current_file + current_offset;

    return 1;
}
