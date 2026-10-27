/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/huff.h - canonical length-limited Huffman, MSB-first bit writer, CRC-16.
   Copyright (C) 2026 Dan Gibson. */
#ifndef NDRC_HUFF_H
#define NDRC_HUFF_H

#include <stddef.h>
#include "str.h"

#define HUFF_MAXLEN 15
#define HUFF_MAXSYM 511

/* counts[nsym] -> lengths[nsym] (0 for unused), canonical, max HUFF_MAXLEN;
   nsym 256 or 511. Returns 0 on success, -1 if nsym > HUFF_MAXSYM. */
int huff_lengths(const unsigned long *counts, unsigned nsym, unsigned char *lengths);

/* codes[i] = canonical code of lengths[i] bits, right-aligned; shorter
   codes first, ties by symbol number. Unused symbols get 0. */
void huff_codes(const unsigned char *lengths, unsigned nsym, unsigned short *codes);

typedef struct { Str *out; unsigned acc; int nbits; } BitWriter;
void bw_init(BitWriter *w, Str *out);
void bw_put(BitWriter *w, unsigned code, unsigned len);   /* MSB first */
void bw_flush(BitWriter *w);                               /* zero-pad to a byte */

/* Test/self-check decoder: reads one symbol at bit position *pos (bits
   from byte 0 MSB first), returns symbol or -1 on overrun past len bytes. */
int huff_decode_one(const unsigned char *lengths, unsigned nsym,
                    const unsigned char *data, size_t len, size_t *pos);

unsigned short crc16_ccitt(const unsigned char *p, size_t n);  /* poly 0x1021 init 0xFFFF */

#endif /* NDRC_HUFF_H */
