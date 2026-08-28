/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/str.c - Copyright (C) 2026 Dan Gibson. */
#include "str.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STR_INITIAL_CAP 64u

struct Str {
    unsigned char *data;
    size_t len;
    size_t cap;
    Arena *arena;
};

static void str_reserve(Str *s, size_t extra)
{
    size_t need;
    size_t cap;
    unsigned char *nd;

    /* Same reasoning as arena_alloc's guard: a wrapped size would
       under-reserve and the caller would then write past the buffer.
       Refuse rather than corrupt. */
    if (extra > SIZE_MAX - s->len - 1u) {
        fprintf(stderr,
                "ndrc: string append of %zu bytes is impossibly large\n",
                extra);
        abort();
    }

    need = s->len + extra + 1u;   /* +1 for the trailing NUL */
    if (need <= s->cap) return;

    cap = s->cap ? s->cap : STR_INITIAL_CAP;
    while (cap < need) {
        /* Doubling past this point would wrap; clamp instead, or the
           loop would spin forever once cap wrapped to zero. */
        if (cap > SIZE_MAX / 2u) {
            cap = need;
            break;
        }
        cap *= 2u;
    }

    /* The arena never frees, so growth copies into a fresh block. Doubling
       keeps the total copied linear in the final size. */
    nd = arena_alloc(s->arena, cap);
    if (s->len) memcpy(nd, s->data, s->len);
    arena_poison(s->arena, s->data, s->cap);
    s->data = nd;
    s->cap = cap;
}

Str *str_new(Arena *a)
{
    Str *s = arena_alloc(a, sizeof(*s));
    s->arena = a;
    s->data = arena_alloc(a, STR_INITIAL_CAP);
    s->cap = STR_INITIAL_CAP;
    s->len = 0;
    s->data[0] = '\0';
    return s;
}

Str *str_from(Arena *a, const char *t)
{
    Str *s = str_new(a);
    str_append(s, t);
    return s;
}

void str_push(Str *s, char c)
{
    str_reserve(s, 1u);
    s->data[s->len++] = (unsigned char)c;
    s->data[s->len] = '\0';
}

void str_append(Str *s, const char *t)
{
    str_append_n(s, t, strlen(t));
}

void str_append_n(Str *s, const void *t, size_t n)
{
    if (n == 0) return;
    str_reserve(s, n);
    memcpy(s->data + s->len, t, n);
    s->len += n;
    s->data[s->len] = '\0';
}

void str_appendf(Str *s, const char *fmt, ...)
{
    va_list ap, ap2;
    int n;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0) {
        va_end(ap2);
        fprintf(stderr, "ndrc: format error in str_appendf\n");
        abort();
    }

    str_reserve(s, (size_t)n);
    vsnprintf((char *)s->data + s->len, (size_t)n + 1u, fmt, ap2);
    va_end(ap2);
    s->len += (size_t)n;
    s->data[s->len] = '\0';
}

void str_push_u8(Str *s, unsigned v)
{
    str_push(s, (char)(unsigned char)(v & 0xFFu));
}

void str_push_u16le(Str *s, unsigned v)
{
    str_push_u8(s, v & 0xFFu);
    str_push_u8(s, (v >> 8) & 0xFFu);
}

void str_push_u16be(Str *s, unsigned v)
{
    str_push_u8(s, (v >> 8) & 0xFFu);
    str_push_u8(s, v & 0xFFu);
}

static void str_check_patch(const Str *s, size_t at)
{
    if (at > s->len || s->len - at < 2u) {
        fprintf(stderr,
                "ndrc: internal error, back-patch at %zu exceeds length %zu\n",
                at, s->len);
        abort();
    }
}

void str_set_u16le(Str *s, size_t at, unsigned v)
{
    str_check_patch(s, at);
    s->data[at] = (unsigned char)(v & 0xFFu);
    s->data[at + 1u] = (unsigned char)((v >> 8) & 0xFFu);
}

void str_set_u16be(Str *s, size_t at, unsigned v)
{
    str_check_patch(s, at);
    s->data[at] = (unsigned char)((v >> 8) & 0xFFu);
    s->data[at + 1u] = (unsigned char)(v & 0xFFu);
}

void str_set_u8(Str *s, size_t at, unsigned v)
{
    if (at >= s->len) {
        fprintf(stderr,
                "ndrc: internal error, str_set_u8 at %zu exceeds length %zu\n",
                at, s->len);
        abort();
    }
    s->data[at] = (unsigned char)(v & 0xFFu);
}

void str_push_u16(Str *s, unsigned v, int big_endian)
{
    if (big_endian)
        str_push_u16be(s, v);
    else
        str_push_u16le(s, v);
}

void str_set_u16(Str *s, size_t at, unsigned v, int big_endian)
{
    if (big_endian)
        str_set_u16be(s, at, v);
    else
        str_set_u16le(s, at, v);
}

const char *str_cstr(Str *s)
{
    return (const char *)s->data;
}

const unsigned char *str_bytes(const Str *s)
{
    return s->data;
}

size_t str_len(const Str *s)
{
    return s->len;
}

void str_clear(Str *s)
{
    s->len = 0;
    s->data[0] = '\0';
}

static int ascii_upper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

int str_ieq(const char *a, const char *b)
{
    size_t i = 0;
    for (;;) {
        int ca = ascii_upper((unsigned char)a[i]);
        int cb = ascii_upper((unsigned char)b[i]);
        if (ca != cb) return 0;
        if (ca == 0) return 1;
        i++;
    }
}

char *str_upper_ascii(Arena *a, const char *s)
{
    char *out = arena_strdup(a, s);
    size_t i;
    for (i = 0; out[i] != '\0'; i++) {
        out[i] = (char)ascii_upper((unsigned char)out[i]);
    }
    return out;
}

/* Pinned mapping. FPC's AnsiUpperCase here runs under Windows-1252
   (confirmed: this machine's registry ACP is 1252), so the fold
   covers the WHOLE lowercase-accented byte block
   0xE0-0xFF, not just the eight bytes DSF identifiers/labels can
   contain:

     - 0xE0-0xFE except 0xF7: folds by a flat -0x20, i.e. exactly the
       Latin-1 lower-to-upper row shift (a-grave..thorn -> A-grave..
       THORN).
     - 0xF7 (division sign) has no case: identity, the one gap in that
       row.
     - 0xFF (y-diaeresis) is the sole irregular case: folds to 0x9F,
       cp1252's only single-byte slot for Y-diaeresis (U+0178) - it is
       NOT in the 0xC0-0xDE row the -0x20 shift would otherwise land
       it in, because Latin-1/cp1252 has no uppercase Y-diaeresis there
       at all.

   Every byte outside 0xE0-0xFF (including the already-uppercase
   0xC0-0xDE row) passes through unchanged, matching AnsiUpperCase's
   idempotence on bytes that are not lowercase. */
static int latin1_upper(int c)
{
    unsigned char b = (unsigned char)c;

    if (b >= 'a' && b <= 'z') return b - 'a' + 'A';
    if (b == 0xFF) return 0x9F;
    if (b >= 0xE0 && b <= 0xFE && b != 0xF7) return b - 0x20;
    return b;
}

char *str_upper_latin1(Arena *a, const char *s)
{
    char *out = arena_strdup(a, s);
    size_t i;
    for (i = 0; out[i] != '\0'; i++) {
        out[i] = (char)(unsigned char)latin1_upper((unsigned char)out[i]);
    }
    return out;
}

Arena *str_arena(const Str *s)
{
    return s->arena;
}

void str_assign(Str *dst, const Str *src)
{
    /* dst's old buffer is abandoned exactly like a growth buffer:
       never handed out again, so poison it for the sanitize build. */
    arena_poison(dst->arena, dst->data, dst->cap);
    *dst = *src;
}
