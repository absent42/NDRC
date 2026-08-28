/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_arena.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "arena.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

TEST(arena_alloc_returns_usable_distinct_memory)
{
    Arena *a = arena_new(1024);
    char *p = arena_alloc(a, 10);
    char *q = arena_alloc(a, 10);
    CHECK(p != NULL);
    CHECK(q != NULL);
    CHECK(p != q);
    memset(p, 'x', 10);
    memset(q, 'y', 10);
    CHECK_INT(p[0], 'x');
    CHECK_INT(q[0], 'y');
    arena_free(a);
}

TEST(arena_alloc_is_max_aligned)
{
    Arena *a = arena_new(1024);
    /* A 1-byte allocation must still leave the next pointer aligned. */
    arena_alloc(a, 1);
    void *p = arena_alloc(a, 8);
    CHECK_INT((uintptr_t)p % _Alignof(max_align_t), 0);
    arena_free(a);
}

TEST(arena_calloc_zeroes)
{
    Arena *a = arena_new(1024);
    unsigned char *p = arena_calloc(a, 64);
    int all_zero = 1;
    size_t i;
    for (i = 0; i < 64; i++) {
        if (p[i] != 0) all_zero = 0;
    }
    CHECK(all_zero);
    arena_free(a);
}

TEST(arena_grows_past_one_block)
{
    Arena *a = arena_new(64);
    char *p = arena_alloc(a, 32);
    char *q = arena_alloc(a, 4096);   /* larger than block_size */
    CHECK(p != NULL);
    CHECK(q != NULL);
    memset(q, 'z', 4096);
    CHECK_INT(q[4095], 'z');
    CHECK(arena_bytes_used(a) >= 4128);
    arena_free(a);
}

TEST(arena_strdup_copies)
{
    Arena *a = arena_new(1024);
    const char *src = "SYSMESS";
    char *copy = arena_strdup(a, src);
    CHECK_STR(copy, "SYSMESS");
    CHECK(copy != src);
    arena_free(a);
}

TEST(arena_strndup_truncates_and_terminates)
{
    Arena *a = arena_new(1024);
    char *copy = arena_strndup(a, "MESSAGE 12", 7);
    CHECK_STR(copy, "MESSAGE");
    arena_free(a);
}

TEST(arena_strndup_stops_at_embedded_nul)
{
    Arena *a = arena_new(1024);
    char *copy = arena_strndup(a, "AB\0CD", 5);
    CHECK_STR(copy, "AB");
    arena_free(a);
}

TEST(arena_zero_size_alloc_is_safe)
{
    Arena *a = arena_new(1024);
    void *p = arena_alloc(a, 0);
    CHECK(p != NULL);
    arena_free(a);
}

int main(void)
{
    RUN(arena_alloc_returns_usable_distinct_memory);
    RUN(arena_alloc_is_max_aligned);
    RUN(arena_calloc_zeroes);
    RUN(arena_grows_past_one_block);
    RUN(arena_strdup_copies);
    RUN(arena_strndup_truncates_and_terminates);
    RUN(arena_strndup_stops_at_embedded_nul);
    RUN(arena_zero_size_alloc_is_safe);
    return test_summary("arena");
}
