/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_map.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "arena.h"
#include "map.h"

#include <stdio.h>

TEST(map_starts_empty)
{
    Arena *a = arena_new(0);
    Map *m = map_new(a);
    CHECK_INT(map_len(m), 0);
    CHECK(map_get(m, "COLS") == NULL);
    CHECK_INT(map_has(m, "COLS"), 0);
    arena_free(a);
}

TEST(map_put_then_get)
{
    Arena *a = arena_new(0);
    Map *m = map_new(a);
    int cols = 80;
    map_put(m, "COLS", &cols);
    CHECK_INT(map_len(m), 1);
    CHECK(map_get(m, "COLS") == &cols);
    CHECK_INT(map_has(m, "COLS"), 1);
    arena_free(a);
}

TEST(map_put_replaces_without_growing)
{
    Arena *a = arena_new(0);
    Map *m = map_new(a);
    int first = 42, second = 80;
    map_put(m, "COLS", &first);
    map_put(m, "COLS", &second);
    CHECK_INT(map_len(m), 1);
    CHECK(map_get(m, "COLS") == &second);
    arena_free(a);
}

TEST(map_replace_keeps_original_insertion_position)
{
    Arena *a = arena_new(0);
    Map *m = map_new(a);
    int one = 1, two = 2, replaced = 9;
    MapEntry e;
    map_put(m, "FIRST", &one);
    map_put(m, "SECOND", &two);
    map_put(m, "FIRST", &replaced);
    CHECK_INT(map_len(m), 2);
    CHECK_INT(map_at(m, 0, &e), 1);
    CHECK_STR(e.key, "FIRST");
    CHECK(e.val == &replaced);
    CHECK_INT(map_at(m, 1, &e), 1);
    CHECK_STR(e.key, "SECOND");
    arena_free(a);
}

TEST(map_iterates_in_insertion_order)
{
    Arena *a = arena_new(0);
    Map *m = map_new(a);
    static int v[5] = {0, 1, 2, 3, 4};
    const char *keys[5] = {"ZULU", "ALPHA", "MIKE", "BRAVO", "YANKEE"};
    size_t i;
    for (i = 0; i < 5; i++) map_put(m, keys[i], &v[i]);
    CHECK_INT(map_len(m), 5);
    for (i = 0; i < 5; i++) {
        MapEntry e;
        CHECK_INT(map_at(m, i, &e), 1);
        CHECK_STR(e.key, keys[i]);
        CHECK(e.val == &v[i]);
    }
    arena_free(a);
}

TEST(map_keys_are_case_sensitive)
{
    Arena *a = arena_new(0);
    Map *m = map_new(a);
    int hi = 1, lo = 2;
    map_put(m, "COLS", &hi);
    map_put(m, "cols", &lo);
    CHECK_INT(map_len(m), 2);
    CHECK(map_get(m, "COLS") == &hi);
    CHECK(map_get(m, "cols") == &lo);
    arena_free(a);
}

TEST(map_copies_keys)
{
    Arena *a = arena_new(0);
    Map *m = map_new(a);
    char key[8];
    int val = 7;
    snprintf(key, sizeof(key), "TEMP");
    map_put(m, key, &val);
    snprintf(key, sizeof(key), "GONE");
    CHECK(map_get(m, "TEMP") == &val);
    arena_free(a);
}

TEST(map_survives_many_entries_and_rehash)
{
    Arena *a = arena_new(0);
    Map *m = map_new(a);
    static int v[2000];
    static char keys[2000][16];
    MapEntry e;
    int i;
    for (i = 0; i < 2000; i++) {
        v[i] = i;
        snprintf(keys[i], sizeof(keys[i]), "SYM%d", i);
        map_put(m, keys[i], &v[i]);
    }
    CHECK_INT(map_len(m), 2000);
    for (i = 0; i < 2000; i++) {
        CHECK(map_get(m, keys[i]) == &v[i]);
    }
    /* Insertion order must survive rehashing. */
    CHECK_INT(map_at(m, 0, &e), 1);
    CHECK_STR(e.key, "SYM0");
    CHECK_INT(map_at(m, 1999, &e), 1);
    CHECK_STR(e.key, "SYM1999");
    arena_free(a);
}

TEST(map_at_out_of_range_returns_zero)
{
    Arena *a = arena_new(0);
    Map *m = map_new(a);
    MapEntry e;
    CHECK_INT(map_at(m, 0, &e), 0);
    arena_free(a);
}

TEST(map_at_copy_survives_growth)
{
    Arena *a = arena_new(0);
    Map *m = map_new(a);
    static int v[2000];
    static char keys[2000][16];
    MapEntry held;
    int i;
    map_put(m, "HELD", &v[0]);
    CHECK_INT(map_at(m, 0, &held), 1);
    /* 2000 inserts force both entry growth and rehash. */
    for (i = 0; i < 2000; i++) {
        snprintf(keys[i], sizeof(keys[i]), "SYM%d", i);
        map_put(m, keys[i], &v[i]);
    }
    CHECK_STR(held.key, "HELD");
    CHECK(held.val == &v[0]);
    arena_free(a);
}

TEST(map_stores_null_value_distinctly_from_absent)
{
    Arena *a = arena_new(0);
    Map *m = map_new(a);
    map_put(m, "PRESENT", NULL);
    CHECK_INT(map_has(m, "PRESENT"), 1);
    CHECK(map_get(m, "PRESENT") == NULL);
    CHECK_INT(map_has(m, "ABSENT"), 0);
    arena_free(a);
}

int main(void)
{
    RUN(map_starts_empty);
    RUN(map_put_then_get);
    RUN(map_put_replaces_without_growing);
    RUN(map_replace_keeps_original_insertion_position);
    RUN(map_iterates_in_insertion_order);
    RUN(map_keys_are_case_sensitive);
    RUN(map_copies_keys);
    RUN(map_survives_many_entries_and_rehash);
    RUN(map_at_out_of_range_returns_zero);
    RUN(map_at_copy_survives_growth);
    RUN(map_stores_null_value_distinctly_from_absent);
    return test_summary("map");
}
