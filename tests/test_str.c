/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_str.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "arena.h"
#include "str.h"

#include <string.h>

TEST(str_starts_empty)
{
    Arena *a = arena_new(0);
    Str *s = str_new(a);
    CHECK_INT(str_len(s), 0);
    CHECK_STR(str_cstr(s), "");
    arena_free(a);
}

TEST(str_append_and_push_accumulate)
{
    Arena *a = arena_new(0);
    Str *s = str_new(a);
    str_append(s, "MES");
    str_push(s, 'S');
    str_append(s, "AGE");
    CHECK_STR(str_cstr(s), "MESSAGE");
    CHECK_INT(str_len(s), 7);
    arena_free(a);
}

TEST(str_grows_past_initial_capacity)
{
    Arena *a = arena_new(0);
    Str *s = str_new(a);
    int i;
    for (i = 0; i < 5000; i++) str_push(s, 'a');
    CHECK_INT(str_len(s), 5000);
    CHECK_INT(str_cstr(s)[4999], 'a');
    CHECK_INT(str_cstr(s)[5000], '\0');
    arena_free(a);
}

TEST(str_is_binary_safe)
{
    Arena *a = arena_new(0);
    Str *s = str_new(a);
    str_append_n(s, "AB\0CD", 5);
    CHECK_INT(str_len(s), 5);
    CHECK_INT(str_bytes(s)[2], 0);
    CHECK_INT(str_bytes(s)[3], 'C');
    arena_free(a);
}

TEST(str_appendf_formats)
{
    Arena *a = arena_new(0);
    Str *s = str_new(a);
    str_appendf(s, "at %d (0x%04X)", 2038, 2038);
    CHECK_STR(str_cstr(s), "at 2038 (0x07F6)");
    arena_free(a);
}

TEST(str_appendf_handles_long_output)
{
    Arena *a = arena_new(0);
    Str *s = str_new(a);
    char big[900];
    memset(big, 'q', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    str_appendf(s, "%s", big);
    CHECK_INT(str_len(s), 899);
    CHECK_STR(str_cstr(s), big);
    arena_free(a);
}

TEST(str_pushes_little_and_big_endian_words)
{
    Arena *a = arena_new(0);
    Str *s = str_new(a);
    str_push_u8(s, 0x0C);
    str_push_u16le(s, 0x07F6);
    str_push_u16be(s, 0x07F6);
    CHECK_INT(str_len(s), 5);
    CHECK_INT(str_bytes(s)[0], 0x0C);
    CHECK_INT(str_bytes(s)[1], 0xF6);
    CHECK_INT(str_bytes(s)[2], 0x07);
    CHECK_INT(str_bytes(s)[3], 0x07);
    CHECK_INT(str_bytes(s)[4], 0xF6);
    arena_free(a);
}

TEST(str_push_truncates_to_byte_and_word)
{
    Arena *a = arena_new(0);
    Str *s = str_new(a);
    str_push_u8(s, 0x1FF);
    str_push_u16le(s, 0x11234);
    CHECK_INT(str_bytes(s)[0], 0xFF);
    CHECK_INT(str_bytes(s)[1], 0x34);
    CHECK_INT(str_bytes(s)[2], 0x12);
    arena_free(a);
}

TEST(str_set_backpatches_in_place)
{
    Arena *a = arena_new(0);
    Str *s = str_new(a);
    str_push_u16le(s, 0);
    str_push_u16be(s, 0);
    str_set_u16le(s, 0, 0xBEEF);
    str_set_u16be(s, 2, 0xBEEF);
    CHECK_INT(str_bytes(s)[0], 0xEF);
    CHECK_INT(str_bytes(s)[1], 0xBE);
    CHECK_INT(str_bytes(s)[2], 0xBE);
    CHECK_INT(str_bytes(s)[3], 0xEF);
    CHECK_INT(str_len(s), 4);
    arena_free(a);
}

TEST(str_clear_resets_length_but_keeps_usable)
{
    Arena *a = arena_new(0);
    Str *s = str_from(a, "DISCARD");
    str_clear(s);
    CHECK_INT(str_len(s), 0);
    CHECK_STR(str_cstr(s), "");
    str_append(s, "REUSED");
    CHECK_STR(str_cstr(s), "REUSED");
    arena_free(a);
}

TEST(str_ieq_is_ascii_case_insensitive)
{
    CHECK(str_ieq("NEXTDAAD", "nextdaad"));
    CHECK(str_ieq("", ""));
    CHECK(!str_ieq("ZX", "ZX81"));
    CHECK(!str_ieq("ZX81", "ZX"));
    /* Latin-1 high bytes must not be case-folded: DAAD source is
       ISO-8859-1 and folding them would collide distinct characters. */
    CHECK(!str_ieq("\xE1", "\xC1"));
}

TEST(str_upper_ascii_leaves_high_bytes_alone)
{
    Arena *a = arena_new(0);
    char *u = str_upper_ascii(a, "fTurns\xE1");
    CHECK_STR(u, "FTURNS\xE1");
    arena_free(a);
}

TEST(str_set_u8_patches_one_byte)
{
    Arena *a = arena_new(0);
    Str *s = str_new(a);
    str_push_u8(s, 0x00); str_push_u8(s, 0xAA);
    str_set_u8(s, 0, 0x5F);
    CHECK_INT(str_bytes(s)[0], 0x5F);
    CHECK_INT(str_bytes(s)[1], 0xAA);
    CHECK_INT(str_len(s), 2);
    arena_free(a);
}

TEST(str_u16_dispatch_matches_explicit_forms)
{
    Arena *a = arena_new(0);
    Str *s = str_new(a);
    str_push_u16(s, 0x07F6, 0);   /* little */
    str_push_u16(s, 0x07F6, 1);   /* big */
    CHECK_INT(str_bytes(s)[0], 0xF6); CHECK_INT(str_bytes(s)[1], 0x07);
    CHECK_INT(str_bytes(s)[2], 0x07); CHECK_INT(str_bytes(s)[3], 0xF6);
    str_set_u16(s, 0, 0x0052, 0);
    str_set_u16(s, 2, 0x0052, 1);
    CHECK_INT(str_bytes(s)[0], 0x52); CHECK_INT(str_bytes(s)[1], 0x00);
    CHECK_INT(str_bytes(s)[2], 0x00); CHECK_INT(str_bytes(s)[3], 0x52);
    arena_free(a);
}

TEST(str_assign_adopts_source_buffer)
{
    Arena *a = arena_new(0);
    Str *dst = str_new(a);
    Str *src = str_new(a);
    str_append(dst, "OLD");
    str_append(src, "NEW CONTENT");
    str_assign(dst, src);
    CHECK_INT(str_len(dst), 11);
    CHECK_STR(str_cstr(dst), "NEW CONTENT");
    /* dst must keep working as a normal Str after adoption. */
    str_push_u8(dst, 0x21);
    CHECK_STR(str_cstr(dst), "NEW CONTENT!");
    arena_free(a);
}

#ifdef NDRC_ASAN
TEST(str_growth_poisons_stale_buffer)
{
    Arena *a = arena_new(0);
    Str *s = str_new(a);
    const unsigned char *stale = str_bytes(s);
    int i;
    str_append(s, "x");
    /* 100 x 10 bytes forces growth well past the 64-byte initial
       capacity, abandoning the buffer stale points into. */
    for (i = 0; i < 100; i++) str_append(s, "0123456789");
    CHECK(__asan_address_is_poisoned(stale));
    arena_free(a);
}

TEST(str_assign_poisons_old_dst_buffer)
{
    Arena *a = arena_new(0);
    Str *dst = str_new(a);
    Str *src = str_new(a);
    const unsigned char *old = str_bytes(dst);
    str_append(dst, "OLD");
    str_append(src, "NEW");
    str_assign(dst, src);
    CHECK(__asan_address_is_poisoned(old));
    CHECK_STR(str_cstr(dst), "NEW");
    arena_free(a);
}
#endif

int main(void)
{
    RUN(str_starts_empty);
    RUN(str_append_and_push_accumulate);
    RUN(str_grows_past_initial_capacity);
    RUN(str_is_binary_safe);
    RUN(str_appendf_formats);
    RUN(str_appendf_handles_long_output);
    RUN(str_pushes_little_and_big_endian_words);
    RUN(str_push_truncates_to_byte_and_word);
    RUN(str_set_backpatches_in_place);
    RUN(str_clear_resets_length_but_keeps_usable);
    RUN(str_ieq_is_ascii_case_insensitive);
    RUN(str_upper_ascii_leaves_high_bytes_alone);
    RUN(str_set_u8_patches_one_byte);
    RUN(str_u16_dispatch_matches_explicit_forms);
    RUN(str_assign_adopts_source_buffer);
#ifdef NDRC_ASAN
    RUN(str_growth_poisons_stale_buffer);
    RUN(str_assign_poisons_old_dst_buffer);
#endif
    return test_summary("str");
}
