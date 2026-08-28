/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/map.c - Copyright (C) 2026 Dan Gibson. */
#include "map.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_INITIAL_BUCKETS 64u
#define MAP_INITIAL_ENTRIES 16u

/* Bucket slots hold an index into the entry array plus one, so that 0
   means empty and no sentinel value is needed. */
struct Map {
    Arena *arena;
    MapEntry *entries;
    size_t len;
    size_t entries_cap;
    size_t *buckets;
    size_t nbuckets;
};

/* FNV-1a. Chosen for being short, dependency-free and adequate for
   identifier-shaped keys. */
static size_t hash_key(const char *s)
{
    size_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static void map_rehash(Map *m, size_t nbuckets)
{
    size_t i;
    size_t *old_buckets = m->buckets;
    size_t old_nbuckets = m->nbuckets;
    m->buckets = arena_calloc(m->arena, nbuckets * sizeof(size_t));
    m->nbuckets = nbuckets;
    for (i = 0; i < m->len; i++) {
        size_t b = hash_key(m->entries[i].key) & (nbuckets - 1u);
        while (m->buckets[b] != 0) b = (b + 1u) & (nbuckets - 1u);
        m->buckets[b] = i + 1u;
    }
    arena_poison(m->arena, old_buckets, old_nbuckets * sizeof(size_t));
}

Map *map_new(Arena *a)
{
    Map *m = arena_alloc(a, sizeof(*m));
    m->arena = a;
    m->len = 0;
    m->entries_cap = MAP_INITIAL_ENTRIES;
    m->entries = arena_alloc(a, m->entries_cap * sizeof(MapEntry));
    m->buckets = arena_calloc(a, MAP_INITIAL_BUCKETS * sizeof(size_t));
    m->nbuckets = MAP_INITIAL_BUCKETS;
    return m;
}

/* Returns the bucket slot holding key, or the empty slot where it
   belongs. Linear probing; the table is kept below 70 percent load so a
   free slot always exists. */
static size_t map_slot(const Map *m, const char *key)
{
    size_t b = hash_key(key) & (m->nbuckets - 1u);
    while (m->buckets[b] != 0) {
        const MapEntry *e = &m->entries[m->buckets[b] - 1u];
        if (strcmp(e->key, key) == 0) return b;
        b = (b + 1u) & (m->nbuckets - 1u);
    }
    return b;
}

void map_put(Map *m, const char *key, void *val)
{
    size_t b = map_slot(m, key);

    if (m->buckets[b] != 0) {
        m->entries[m->buckets[b] - 1u].val = val;
        return;
    }

    if (m->len == m->entries_cap) {
        size_t cap;
        MapEntry *ne;

        /* Same shape as vec_push: one check bounds both the doubling and the
           byte-size multiply. */
        if (m->entries_cap > SIZE_MAX / (2u * sizeof(MapEntry))) {
            fprintf(stderr,
                    "ndrc: map cannot grow beyond %zu entries\n",
                    m->entries_cap);
            abort();
        }

        cap = m->entries_cap * 2u;
        ne = arena_alloc(m->arena, cap * sizeof(MapEntry));
        memcpy(ne, m->entries, m->len * sizeof(MapEntry));
        arena_poison(m->arena, m->entries, m->entries_cap * sizeof(MapEntry));
        m->entries = ne;
        m->entries_cap = cap;
    }

    m->entries[m->len].key = arena_strdup(m->arena, key);
    m->entries[m->len].val = val;
    m->len++;

    if ((m->len + 1u) * 10u > m->nbuckets * 7u) {
        /* Bounds both the doubling and map_rehash's nbuckets * sizeof(size_t).
           These two guards also keep the load-factor arithmetic above safe by
           construction: they hold nbuckets below SIZE_MAX/16 and len below
           SIZE_MAX/32, so neither (len + 1) * 10 nor nbuckets * 7 can wrap. */
        if (m->nbuckets > SIZE_MAX / (2u * sizeof(size_t))) {
            fprintf(stderr,
                    "ndrc: map cannot rehash beyond %zu buckets\n",
                    m->nbuckets);
            abort();
        }
        map_rehash(m, m->nbuckets * 2u);
    } else {
        m->buckets[b] = m->len;
    }
}

void *map_get(const Map *m, const char *key)
{
    size_t b = map_slot(m, key);
    if (m->buckets[b] == 0) return NULL;
    return m->entries[m->buckets[b] - 1u].val;
}

int map_has(const Map *m, const char *key)
{
    size_t b = map_slot(m, key);
    return m->buckets[b] != 0;
}

size_t map_len(const Map *m)
{
    return m->len;
}

int map_at(const Map *m, size_t i, MapEntry *out)
{
    if (i >= m->len) return 0;
    *out = m->entries[i];
    return 1;
}
