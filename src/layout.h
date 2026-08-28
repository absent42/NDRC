/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/layout.h - DDB address-layout helpers.
   Copyright (C) 2026 Dan Gibson.

   Ported from drb.php addPaddingIfRequired (drb.php:288-297). */
#ifndef NDRC_LAYOUT_H
#define NDRC_LAYOUT_H

#include "str.h"
#include "targets.h"

/* Writes one zero byte when the target pads and *addr is odd. No-op on
   NEXTDAAD (not a padding platform); load-bearing on PC/ST/Amiga/HTML in
   Phase 1b. Call POSITIONS are part of the format - analysis S5.3.

   The padding condition is
   `(t->padding_platform || forced_padding)` - see layout_set_forced. */
void layout_pad(Str *out, long *addr, const Target *t);

/* PORT: drb.php:288-297's -np arm calls PHP exit; from WHEREVER the
   padding call happens. NDRC mirrors the global-state shape with a
   module setter carrying the write context, so layout_pad can
   reproduce the truncate-and-terminate defect (S12.1) - a direct
   exit() inside a library file, deliberately reproducing DRB's own
   exit-inside-helper defect and its partial on-disk write. Set once
   by main.c after option parsing, as soon as `out` exists. */
void layout_set_forced(int forced_padding, int forced_no_padding,
                       const Str *out, const char *output_path);

#endif /* NDRC_LAYOUT_H */
