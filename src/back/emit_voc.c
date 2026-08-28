/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/emit_voc.c - vocabulary table emitter.
   Copyright (C) 2026 Dan Gibson.

   PORT: drb.php:651-713 generateVocabulary, including the UTF-8 195
   continuation-byte path and the dead "old conversions" branch. */
#include "emit.h"

#include <string.h>

#define OFUSCATE_VALUE 0xFFu

/* drb.php:248 daadToChr::conversions, the old Spanish-character table -
   PORT NOTE (analysis S12.1): dead twice over. drb.php:663's in_array
   check can never match (table entries are two-byte UTF-8, while
   $tempWord[$i] is one byte); even if it did, drb.php:665's assignment
   is never read and the char is dropped from output either way.
   old_conversion_index below reproduces both defects structurally
   (always -1 in practice). */
static const char *const old_conversions[16] = {
    "\xC2\xAA", "\xC2\xA1", "\xC2\xBF", "\xC2\xAB", "\xC2\xBB",
    "\xC3\xA1", "\xC3\xA9", "\xC3\xAD", "\xC3\xB3", "\xC3\xBA",
    "\xC3\xB1", "\xC3\x91", "\xC3\xA7", "\xC3\x87", "\xC3\xBC", "\xC3\x9C"
};

/* Mirrors PHP's in_array/array_search over old_conversions for a single
   raw byte c: returns the matching table index, or -1. Always -1 (see
   the PORT NOTE above), since every entry is two bytes long. */
static int old_conversion_index(unsigned char c)
{
    int k;
    for (k = 0; k < 16; k++) {
        const char *entry = old_conversions[k];
        if (entry[0] == (char)c && entry[1] == '\0') return k;
    }
    return -1;
}

/* PORT NOTE: drb.php:693's default case echoes a warning for a
   malformed UTF-8 continuation byte; emit_vocabulary takes no Diag*
   (see emit.h), so only the byte-consuming control flow is reproduced
   here, not the warning text - the one dropped side effect in this file. */
void emit_vocabulary(Str *out, long *addr, const Adventure *adv)
{
    size_t i, n = vec_len_VocWordEntry(adv->vocabulary);

    for (i = 0; i < n; i++) {
        const VocWordEntry *word = vec_at_VocWordEntry(adv->vocabulary, i);
        const char *temp = word->VocWord;
        size_t len = strlen(temp);
        char final_word[5];
        size_t produced = 0;   /* uncapped count of chars appended to $finalVocWord */
        size_t j;

        for (j = 0; j < len; j++) {
            unsigned char c = (unsigned char)temp[j];

            if (old_conversion_index(c) >= 0) {
                continue;   /* dead branch - see the PORT NOTE above */
            }
            if (c < 128) {
                if (produced < 5) final_word[produced] = (char)c;
                produced++;
            } else if (c == 195) {
                unsigned char c2;
                char mapped = 0;
                int has_mapped = 1;

                j++;
                /* temp is NUL-terminated, so temp[j] at j==len reads the
                   terminator (0) - matching PHP's out-of-range string
                   offset read, which returns '' (ord 0). */
                c2 = (unsigned char)temp[j];
                switch (c2) {
                    case 161: mapped = 21; break; /* a-acute */
                    case 169: mapped = 22; break; /* e-acute */
                    case 173: mapped = 23; break; /* i-acute */
                    case 179: mapped = 24; break; /* o-acute */
                    case 186: mapped = 25; break; /* u-acute */
                    case 129: mapped = 21; break; /* A-acute */
                    case 137: mapped = 22; break; /* drb.php comment says E-diaeresis; reproduced as-is */
                    case 141: mapped = 23; break; /* I-acute */
                    case 147: mapped = 24; break; /* O-acute */
                    case 154: mapped = 25; break; /* drb.php comment repeats "u" (see 186); reproduced as-is */
                    case 145: mapped = 27; break; /* N-tilde */
                    case 177: mapped = 27; break; /* n-tilde */
                    case 156: mapped = 31; break; /* U-diaeresis */
                    case 188: mapped = 31; break; /* u-diaeresis */
                    case 135: mapped = 29; break; /* C-cedilla */
                    case 167: mapped = 29; break; /* c-cedilla */
                    default:
                        has_mapped = 0;   /* drb.php:693 warning dropped - see file PORT NOTE */
                        break;
                }
                if (has_mapped) {
                    if (produced < 5) final_word[produced] = mapped;
                    produced++;
                }
            } else if (c > 128) {
                if (produced < 5) final_word[produced] = (char)c;
                produced++;
            }
            /* c == 128 exactly: matches none of PHP's four elseif arms
               (not <128, not ==195, not >128), so PHP silently drops
               it - no counter advance, nothing appended. Matched here
               by simply not having an arm for it. */
        }
        /* drb.php:699 str_pad(...,5) then substr(...,0,5): pad short
           words with spaces, truncate long ones to 5. */
        for (j = produced; j < 5; j++) final_word[j] = ' ';

        for (j = 0; j < 5; j++) {
            unsigned char ch = (unsigned char)final_word[j];
            if (ch >= 32 && ch < 128 && ch >= 'a' && ch <= 'z') {
                ch = (unsigned char)(ch - 'a' + 'A');
            }
            str_push_u8(out, ch ^ OFUSCATE_VALUE);
        }
        str_push_u8(out, (unsigned)word->Value);
        str_push_u8(out, (unsigned)word->VocType);

        *addr += 7;
    }
    str_push_u8(out, 0);   /* store 0 to mark end of vocabulary */
    (*addr)++;
}
