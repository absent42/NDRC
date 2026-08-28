/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/layout.c - Copyright (C) 2026 Dan Gibson.

   PORT: drb.php:288-297; forced flags routed via layout_set_forced
   (see layout.h for the S12.1 exit defect). */
#include "layout.h"

#include <stdio.h>
#include <stdlib.h>

static int g_forced_padding = 0;
static int g_forced_no_padding = 0;
static const Str *g_out = NULL;
static const char *g_output_path = NULL;

void layout_set_forced(int forced_padding, int forced_no_padding,
                       const Str *out, const char *output_path)
{
    g_forced_padding = forced_padding;
    g_forced_no_padding = forced_no_padding;
    g_out = out;
    g_output_path = output_path;
}

void layout_pad(Str *out, long *addr, const Target *t)
{
    /* drb.php:290's exit fires unconditionally, ahead of the padding
       test - measured: -np truncates at the FIRST layout_pad even on a
       non-padding platform with even addr. */
    if (g_forced_no_padding) {
        FILE *fp = fopen(g_output_path, "wb");
        if (fp != NULL) {
            fwrite(str_bytes(g_out), 1, str_len(g_out), fp);
            fclose(fp);
        }
        exit(0);   /* measured PHP exit; status */
    }
    if ((t->padding_platform || g_forced_padding) && (*addr % 2) == 1) {
        str_push_u8(out, 0);   /* writeZero: fill with one byte for padding */
        (*addr)++;
    }
}
