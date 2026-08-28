/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/vec.c - Copyright (C) 2026 Dan Gibson. */
#include "vec.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VEC_INITIAL_CAP 8u

struct Vec {
    void **items;
    size_t len;
    size_t cap;
    Arena *arena;
};

Vec *vec_new(Arena *a)
{
    Vec *v = arena_alloc(a, sizeof(*v));
    v->arena = a;
    v->items = arena_alloc(a, VEC_INITIAL_CAP * sizeof(void *));
    v->cap = VEC_INITIAL_CAP;
    v->len = 0;
    return v;
}

void vec_push(Vec *v, void *item)
{
    if (v->len == v->cap) {
        size_t cap;
        void **ni;

        /* One check covers both multiplications below: if cap is within
           this bound then cap * 2 * sizeof(void *) cannot wrap. A wrapped
           size would allocate a small array while the vector recorded a
           large capacity, and the next push would write past the end. */
        if (v->cap > SIZE_MAX / (2u * sizeof(void *))) {
            fprintf(stderr,
                    "ndrc: vector cannot grow beyond %zu items\n", v->cap);
            abort();
        }

        cap = v->cap * 2u;
        ni = arena_alloc(v->arena, cap * sizeof(void *));
        memcpy(ni, v->items, v->len * sizeof(void *));
        arena_poison(v->arena, v->items, v->cap * sizeof(void *));
        v->items = ni;
        v->cap = cap;
    }
    v->items[v->len++] = item;
}

void *vec_at(const Vec *v, size_t i)
{
    if (i >= v->len) return NULL;
    return v->items[i];
}

void vec_set(Vec *v, size_t i, void *item)
{
    if (i >= v->len) {
        fprintf(stderr,
                "ndrc: internal error, vec_set index %zu beyond length %zu\n",
                i, v->len);
        abort();
    }
    v->items[i] = item;
}

size_t vec_len(const Vec *v)
{
    return v->len;
}

Arena *vec_arena(const Vec *v)
{
    return v->arena;
}
