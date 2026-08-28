/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/emit_hdr.h - DDB header emitter and offset patch-up.
   Copyright (C) 2026 Dan Gibson.

   PORT: drb.php's header write (drb.php:1834-1874) and the header patch
   pass (drb.php:2039-2073). See emit_hdr.c for detail. */
#ifndef NDRC_BACK_EMIT_HDR_H
#define NDRC_BACK_EMIT_HDR_H

#include "model.h"
#include "str.h"
#include "targets.h"

/* Number of address words patched at header offset 8 (drb.php:2042-2071),
   in exactly this order: compressed-text offset, process list offset,
   object lookup offset, location lookup offset, message lookup offset,
   sysmess lookup offset, connections lookup offset, vocabulary offset,
   initially-at offset, object names offset, object weight/attr offset,
   object extra-attr offset, file size. */
#define NDRC_HEADER_PATCH_WORDS 13

/* PORT: drb.php:1834-1874. Writes the 60-byte header: DAAD version byte
   (3 if adv->v3code else 2), machine/language byte (t->machine_id<<4,
   with bit 0 set when lang_bit is true - drb.php:1845 sets it for ES and
   PT only, so Phase 1a's EN-only caller always passes 0), submachine
   byte, five table-count bytes (object_data, locations, messages,
   sysmess, processes), 26 zero-filled spare/offset bytes, then 13 extvec
   words (all zero at this point - drb.php:1783-1784 zeroes extvec before
   any emission runs). Advances *addr by 60. */
void emit_header(Str *out, long *addr, const Target *t, const Adventure *adv, int lang_bit);

/* PORT: drb.php:2039-2073, the header patch-up pass. `offsets` holds the
   NDRC_HEADER_PATCH_WORDS values in the exact order documented above;
   each is back-patched at header offset 8 + 2*i via str_set_u16 in the
   target's byte order. Then extvec - which generateProcesses/USERPTR may
   have mutated since emit_header wrote its all-zero copy (drb.php:1117-
   1123, emit_proc.c) - is rewritten at offset 34 from adv->extvec's
   current values (drb.php:2072-2073). */
void emit_header_patch(Str *out, const Target *t, const Adventure *adv,
                        const long offsets[NDRC_HEADER_PATCH_WORDS]);

#endif /* NDRC_BACK_EMIT_HDR_H */
