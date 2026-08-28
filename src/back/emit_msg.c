/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/emit_msg.c - message table emitter.
   Copyright (C) 2026 Dan Gibson.

   PORT: drb.php:526-568 generateMessages, restored to DRB's full
   parameter surface (dump_to_xmb, is_stx, and now xmb/xmb_addr).

   The XMB-write branch below (drb.php:534-535/544-548/556-559) is live:
   main.c opens 0.XMB (append, cursor = existing size) before the
   extern region whenever -X/dump_to_xmb is set, and
   threads that FILE* plus its running cursor through every
   emit_messages call site - see main.c's own PORT NOTE at the -X open.
   A dumped message's LOOKUP WORD holds its XMB offset (xmb_addr, not
   the DDB's *addr) - drb.php:534-536. The DDB-side pad (layout_pad,
   drb.php:534's addPaddingIfRequired call) still runs unconditionally
   per message even when that message's own bytes go to the XMB stream
   instead - ported exactly as DRB has it, a quirk of $currentAddress's
   parity being checked regardless of which handle receives the bytes. */
#include "emit.h"

#include "arena.h"

#define OFUSCATE_VALUE 0xFFu
#define LAST_DEFAULT_SYSMESS 62   /* drb.php:36 */

void emit_messages(Str *out, long *addr, const Target *t, Vec_Message *msgs,
                    int dump_to_xmb, int is_stx, FILE *xmb, long *xmb_addr)
{
    size_t n = vec_len_Message(msgs);
    long *offsets = n ? arena_alloc(str_arena(out), n * sizeof(long)) : NULL;
    size_t i;

    for (i = 0; i < n; i++) {
        const Message *m = vec_at_Message(msgs, i);
        /* PORT: drb.php:532. STX's default system messages always go to
           RAM even when dump_to_xmb is set. */
        int dump_this_to_xmb = dump_to_xmb && !(is_stx && (long)i <= LAST_DEFAULT_SYSMESS);
        const unsigned char *bytes;
        size_t len, j;

        /* PORT: drb.php:534 - addPaddingIfRequired runs before the
           per-message dump decision is acted on, against the DDB's own
           $currentAddress regardless of dump_this_to_xmb. */
        layout_pad(out, addr, t);

        if (dump_this_to_xmb) {
            /* PORT: drb.php:534-535 (offset), 545-550/556-560 (text +
               terminator to the XMB stream). */
            offsets[i] = *xmb_addr;

            bytes = str_bytes(m->Text);
            len = str_len(m->Text);
            for (j = 0; j < len; j++) {
                fputc((int)(bytes[j] ^ OFUSCATE_VALUE), xmb);
                (*xmb_addr)++;
            }
            fputc((int)(0x0Au ^ OFUSCATE_VALUE), xmb);   /* mark of end of string */
            (*xmb_addr)++;
        } else {
            offsets[i] = *addr;

            bytes = str_bytes(m->Text);
            len = str_len(m->Text);
            for (j = 0; j < len; j++) {
                str_push_u8(out, bytes[j] ^ OFUSCATE_VALUE);
                (*addr)++;
            }
            str_push_u8(out, 0x0Au ^ OFUSCATE_VALUE);   /* mark of end of string */
            (*addr)++;
        }
    }

    /* Write the messages table */
    layout_pad(out, addr, t);
    for (i = 0; i < n; i++) {
        str_push_u16(out, (unsigned)offsets[i], t->big_endian);
        *addr += 2;
    }
}
