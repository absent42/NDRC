/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/finish.h - -ch/-3h header prepends and HTML JDDB generation.
   Copyright (C) 2026 Dan Gibson.

   PORT: prependC64HeaderToDDB (drb.php:1496-1512), prependPlus3HeaderToDDB
   (drb.php:1447-1494) and generateJDDB (drb.php:1399-1445). Call order and
   verbose-line placement are drb.php:2117-2132: C64 prepend (verbose line
   BEFORE), +3 prepend (verbose line AFTER), then the HTML JDDB when
   target == HTML. See main.c's tail for the wiring. */
#ifndef NDRC_BACK_FINISH_H
#define NDRC_BACK_FINISH_H

#include <stddef.h>
#include "diag.h"
#include "str.h"

/* PORT: prependC64HeaderToDDB drb.php:1496-1512. Prepends the two-byte
   little-endian base address for C64/CP4. PORT NOTE (drb.php:1500,
   1284-1303, 1287): getBaseAddressByTarget checks forcedBaseAddress
   FIRST, ahead of its per-target switch, so -b= short-circuits the
   switch entirely - the base prepended here is whatever main.c already
   resolved (drb.php:1823's same short-circuit), passed in directly
   since this function has no adventure state of its own to re-derive
   it from. */
void finish_prepend_c64(Str *ddb, long base_address);

/* PORT: prependPlus3HeaderToDDB drb.php:1447-1494. Prepends the 128-byte
   +3DOS header: "PLUS3DOS", 0x1A, issue 0x01, version 0x00, 4-byte LE
   total file size (original + 128), byte 0x03, 2-byte LE original size,
   2-byte LE load address 0x8400, zero fill up to byte 126, then a
   checksum byte at 127 (sum of bytes 0..126 & 0xFF). */
void finish_prepend_plus3(Str *ddb);

/* PORT: generateJDDB drb.php:1399-1445. Writes the lowercased output
   name with EVERY ".ddb" substring replaced by ".jddb" (drb.php:1402-
   1403's str_replace has no single-shot mode); unchanged if none match.
   Emits "var DDBDATA = [\n", one "0xNN" element per input byte (an
   offset comment "// 0xNNNN\n" after every 10th, i%10==9), then "\n];",
   plus a same-format "var XMBDATA = [...]" block when "0.XMB" exists
   (dormant this phase). PORT NOTE drb.php:1409-1419: the PHP's
   while(!feof()) loop reads one fgetc() past the last real byte,
   emitting one extra trailing "0x0" phantom element (comma-less) after
   the real data - ported as measured. Prints "Converting DDB to JDDB"
   unconditionally (drb.php:1401). Returns 0 after diag_fatal on a
   failed fopen - the PHP never checks its own; failing loudly is a
   deliberate, gate-invisible deviation. */
int finish_write_jddb(Diag *d, const char *output_path,
                      const unsigned char *ddb, size_t len);

#endif /* NDRC_BACK_FINISH_H */
