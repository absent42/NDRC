/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/huff.c - Copyright (C) 2026 Dan Gibson. */
#include "huff.h"

/* Insertion sort of symbol indices by key order: deterministic, n <= 511. */
static void sort_syms(unsigned *v, unsigned n, const unsigned long *counts, int desc)
{
    for (unsigned i = 1; i < n; i++) {
        unsigned x = v[i];
        unsigned j = i;
        while (j > 0) {
            unsigned y = v[j - 1];
            int after = desc ? (counts[y] < counts[x] || (counts[y] == counts[x] && y > x))
                             : (counts[y] > counts[x] || (counts[y] == counts[x] && y > x));
            if (!after)
                break;
            v[j] = y;
            j--;
        }
        v[j] = x;
    }
}

/* Two-queue Huffman tree (leaf wins weight ties), then the JPEG Annex K.3
   length-limit fix on the depth histogram; lengths are then handed out in
   (count desc, symbol asc) order so a more frequent symbol is never longer. */
int huff_lengths(const unsigned long *counts, unsigned nsym, unsigned char *lengths)
{
    unsigned leaf[HUFF_MAXSYM];
    unsigned long long w[2 * HUFF_MAXSYM];
    unsigned parent[2 * HUFF_MAXSYM];
    unsigned depth[2 * HUFF_MAXSYM];
    unsigned hist[2 * HUFF_MAXSYM];
    unsigned n = 0;

    if (nsym > HUFF_MAXSYM)
        return -1;
    for (unsigned i = 0; i < nsym; i++) {
        lengths[i] = 0;
        if (counts[i])
            leaf[n++] = i;
    }
    if (n == 0)
        return 0;
    if (n == 1) {
        lengths[leaf[0]] = 1;
        return 0;
    }

    /* nodes 0..n-1 are leaves in ascending order, n.. are internal */
    sort_syms(leaf, n, counts, 0);
    for (unsigned i = 0; i < n; i++)
        w[i] = counts[leaf[i]];
    unsigned li = 0, qi = n, next = n;
    while (next < 2 * n - 1) {
        unsigned pick[2];
        for (int k = 0; k < 2; k++) {
            if (li < n && (qi >= next || w[li] <= w[qi]))
                pick[k] = li++;
            else
                pick[k] = qi++;
        }
        w[next] = w[pick[0]] + w[pick[1]];
        parent[pick[0]] = parent[pick[1]] = next;
        next++;
    }
    unsigned root = 2 * n - 2;
    depth[root] = 0;
    for (unsigned i = root; i-- > 0;)
        depth[i] = depth[parent[i]] + 1;

    unsigned maxd = 0;
    for (unsigned d = 0; d < 2 * HUFF_MAXSYM; d++)
        hist[d] = 0;
    for (unsigned i = 0; i < n; i++) {
        hist[depth[i]]++;
        if (depth[i] > maxd)
            maxd = depth[i];
    }
    for (unsigned d = maxd; d > HUFF_MAXLEN; d--) {
        while (hist[d] > 0) {
            unsigned j = d - 2;
            while (hist[j] == 0)
                j--;
            hist[d] -= 2;
            hist[d - 1]++;
            hist[j + 1] += 2;
            hist[j]--;
        }
    }

    sort_syms(leaf, n, counts, 1);
    unsigned d = 1, k = 0;
    for (unsigned i = 0; i < n; i++) {
        while (k == hist[d]) {
            d++;
            k = 0;
        }
        lengths[leaf[i]] = (unsigned char)d;
        k++;
    }
    return 0;
}

void huff_codes(const unsigned char *lengths, unsigned nsym, unsigned short *codes)
{
    unsigned blc[HUFF_MAXLEN + 1] = {0};
    unsigned nextc[HUFF_MAXLEN + 1];
    unsigned code = 0;

    for (unsigned i = 0; i < nsym; i++)
        if (lengths[i])
            blc[lengths[i]]++;
    for (unsigned l = 1; l <= HUFF_MAXLEN; l++) {
        code = (code + blc[l - 1]) << 1;
        nextc[l] = code;
    }
    for (unsigned i = 0; i < nsym; i++)
        codes[i] = lengths[i] ? (unsigned short)nextc[lengths[i]]++ : 0;
}

void bw_init(BitWriter *w, Str *out)
{
    w->out = out;
    w->acc = 0;
    w->nbits = 0;
}

void bw_put(BitWriter *w, unsigned code, unsigned len)
{
    while (len-- > 0) {
        w->acc = (w->acc << 1) | ((code >> len) & 1u);
        if (++w->nbits == 8) {
            str_push_u8(w->out, w->acc);
            w->acc = 0;
            w->nbits = 0;
        }
    }
}

void bw_flush(BitWriter *w)
{
    if (w->nbits > 0)
        bw_put(w, 0, (unsigned)(8 - w->nbits));
}

/* Canonical decode one bit at a time: no tables beyond the length histogram. */
int huff_decode_one(const unsigned char *lengths, unsigned nsym,
                    const unsigned char *data, size_t len, size_t *pos)
{
    unsigned blc[HUFF_MAXLEN + 1] = {0};
    unsigned code = 0, first = 0, index = 0;
    size_t p = *pos;

    for (unsigned i = 0; i < nsym; i++)
        if (lengths[i])
            blc[lengths[i]]++;
    for (unsigned l = 1; l <= HUFF_MAXLEN; l++) {
        if (p >= len * 8)
            return -1;
        code |= (data[p >> 3] >> (7 - (p & 7))) & 1u;
        p++;
        if (code - first < blc[l]) {
            unsigned target = index + (code - first);
            for (unsigned i = 0; i < nsym; i++)
                if (lengths[i] == l && index++ == target) {
                    *pos = p;
                    return (int)i;
                }
        }
        index += blc[l];
        first = (first + blc[l]) << 1;
        code <<= 1;
    }
    return -1;
}

unsigned short crc16_ccitt(const unsigned char *p, size_t n)
{
    unsigned crc = 0xFFFF;
    while (n-- > 0) {
        crc ^= (unsigned)*p++ << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
        crc &= 0xFFFF;
    }
    return (unsigned short)crc;
}
