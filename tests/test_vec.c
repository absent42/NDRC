/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_vec.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "arena.h"
#include "vec.h"

TEST(vec_starts_empty)
{
    Arena *a = arena_new(0);
    Vec *v = vec_new(a);
    CHECK_INT(vec_len(v), 0);
    arena_free(a);
}

TEST(vec_push_preserves_order)
{
    Arena *a = arena_new(0);
    Vec *v = vec_new(a);
    int x = 1, y = 2, z = 3;
    vec_push(v, &x);
    vec_push(v, &y);
    vec_push(v, &z);
    CHECK_INT(vec_len(v), 3);
    CHECK(vec_at(v, 0) == &x);
    CHECK(vec_at(v, 1) == &y);
    CHECK(vec_at(v, 2) == &z);
    arena_free(a);
}

TEST(vec_grows_past_initial_capacity)
{
    Arena *a = arena_new(0);
    Vec *v = vec_new(a);
    static int values[1000];
    int i;
    for (i = 0; i < 1000; i++) {
        values[i] = i;
        vec_push(v, &values[i]);
    }
    CHECK_INT(vec_len(v), 1000);
    CHECK_INT(*(int *)vec_at(v, 0), 0);
    CHECK_INT(*(int *)vec_at(v, 999), 999);
    arena_free(a);
}

TEST(vec_accepts_null_items)
{
    Arena *a = arena_new(0);
    Vec *v = vec_new(a);
    vec_push(v, NULL);
    CHECK_INT(vec_len(v), 1);
    CHECK(vec_at(v, 0) == NULL);
    arena_free(a);
}

TEST(vec_set_replaces_in_place)
{
    Arena *a = arena_new(0);
    Vec *v = vec_new(a);
    int x = 1, y = 2;
    vec_push(v, &x);
    vec_set(v, 0, &y);
    CHECK_INT(vec_len(v), 1);
    CHECK(vec_at(v, 0) == &y);
    arena_free(a);
}

TEST(vec_at_out_of_range_returns_null)
{
    Arena *a = arena_new(0);
    Vec *v = vec_new(a);
    CHECK(vec_at(v, 0) == NULL);
    CHECK(vec_at(v, 99) == NULL);
    arena_free(a);
}

/* A file-local element type exercises the macro exactly as production
   types will. */
typedef struct TItem { int tag; } TItem;
VEC_DECLARE(TItem, TItem *)

TEST(typed_vec_round_trip)
{
    Arena *a = arena_new(0);
    Vec_TItem *v = vec_new_TItem(a);
    TItem x = {1}, y = {2};
    vec_push_TItem(v, &x);
    vec_push_TItem(v, &y);
    CHECK_INT(vec_len_TItem(v), 2);
    CHECK(vec_at_TItem(v, 0) == &x);
    CHECK_INT(vec_at_TItem(v, 1)->tag, 2);
    vec_set_TItem(v, 0, &y);
    CHECK(vec_at_TItem(v, 0) == &y);
    CHECK(vec_arena_TItem(v) == a);
    arena_free(a);
}

TEST(typed_vec_at_out_of_range_returns_null)
{
    Arena *a = arena_new(0);
    Vec_TItem *v = vec_new_TItem(a);
    CHECK(vec_at_TItem(v, 0) == NULL);
    arena_free(a);
}

TEST(typed_vec_grows_past_initial_capacity)
{
    Arena *a = arena_new(0);
    Vec_TItem *v = vec_new_TItem(a);
    static TItem items[1000];
    int i;
    for (i = 0; i < 1000; i++) {
        items[i].tag = i;
        vec_push_TItem(v, &items[i]);
    }
    CHECK_INT(vec_len_TItem(v), 1000);
    CHECK_INT(vec_at_TItem(v, 0)->tag, 0);
    CHECK_INT(vec_at_TItem(v, 999)->tag, 999);
    arena_free(a);
}

TEST(typed_cstr_vec_holds_qualified_strings)
{
    Arena *a = arena_new(0);
    Vec_CStr *v = vec_new_CStr(a);
    vec_push_CStr(v, "alpha");
    vec_push_CStr(v, "beta");
    CHECK_INT(vec_len_CStr(v), 2);
    CHECK_STR(vec_at_CStr(v, 0), "alpha");
    CHECK_STR(vec_at_CStr(v, 1), "beta");
    arena_free(a);
}

TEST(two_typed_vecs_share_one_arena)
{
    Arena *a = arena_new(0);
    Vec_TItem *v1 = vec_new_TItem(a);
    Vec_CStr *v2 = vec_new_CStr(a);
    TItem x = {7};
    vec_push_TItem(v1, &x);
    vec_push_CStr(v2, "seven");
    CHECK_INT(vec_len_TItem(v1), 1);
    CHECK_INT(vec_len_CStr(v2), 1);
    arena_free(a);
}

int main(void)
{
    RUN(vec_starts_empty);
    RUN(vec_push_preserves_order);
    RUN(vec_grows_past_initial_capacity);
    RUN(vec_accepts_null_items);
    RUN(vec_set_replaces_in_place);
    RUN(vec_at_out_of_range_returns_null);
    RUN(typed_vec_round_trip);
    RUN(typed_vec_at_out_of_range_returns_null);
    RUN(typed_vec_grows_past_initial_capacity);
    RUN(typed_cstr_vec_holds_qualified_strings);
    RUN(two_typed_vecs_share_one_arena);
    return test_summary("vec");
}
