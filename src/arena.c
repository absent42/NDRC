/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/arena.c - Copyright (C) 2026 Dan Gibson. */
#include "arena.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define ARENA_DEFAULT_BLOCK 65536u

typedef struct Block {
    struct Block *next;
    size_t cap;
    size_t used;
    /* Payload follows immediately, aligned by the union below. */
} Block;

/* Guarantees the payload start is suitably aligned for any type. */
typedef union BlockHeader {
    Block block;
    max_align_t align;
} BlockHeader;

struct Arena {
    Block *head;
    size_t block_size;
    size_t bytes_used;
};

static size_t align_up(size_t n)
{
    const size_t a = _Alignof(max_align_t);
    return (n + a - 1u) / a * a;
}

static unsigned char *payload(Block *b)
{
    return (unsigned char *)b + sizeof(BlockHeader);
}

static Block *block_new(size_t cap)
{
    BlockHeader *h = malloc(sizeof(BlockHeader) + cap);
    if (h == NULL) {
        fprintf(stderr, "ndrc: out of memory allocating %zu bytes\n", cap);
        abort();
    }
    h->block.next = NULL;
    h->block.cap = cap;
    h->block.used = 0;
    return &h->block;
}

Arena *arena_new(size_t block_size)
{
    Arena *a = malloc(sizeof(*a));
    if (a == NULL) {
        fprintf(stderr, "ndrc: out of memory creating arena\n");
        abort();
    }
    a->block_size = block_size ? block_size : ARENA_DEFAULT_BLOCK;
    a->head = block_new(a->block_size);
    a->bytes_used = 0;
    return a;
}

void *arena_alloc(Arena *a, size_t n)
{
    /* Prevent overflow: a wrapped size would hand back a small buffer
       for a large request, causing memory corruption. Abort instead. */
    if (n > SIZE_MAX - sizeof(BlockHeader) - _Alignof(max_align_t)) {
        fprintf(stderr,
                "ndrc: allocation request of %zu bytes is impossibly large\n", n);
        abort();
    }
    size_t want = align_up(n ? n : 1u);
    Block *b = a->head;
    unsigned char *p;

    if (b->used + want > b->cap) {
        size_t cap = want > a->block_size ? want : a->block_size;
        Block *nb = block_new(cap);
        nb->next = a->head;
        a->head = nb;
        b = nb;
    }

    p = payload(b) + b->used;
    b->used += want;
    a->bytes_used += want;
    return p;
}

void *arena_calloc(Arena *a, size_t n)
{
    void *p = arena_alloc(a, n);
    memset(p, 0, n);
    return p;
}

char *arena_strdup(Arena *a, const char *s)
{
    size_t n = strlen(s);
    char *p = arena_alloc(a, n + 1u);
    memcpy(p, s, n + 1u);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n)
{
    size_t len = 0;
    char *p;
    while (len < n && s[len] != '\0') len++;
    p = arena_alloc(a, len + 1u);
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

size_t arena_bytes_used(const Arena *a)
{
    return a->bytes_used;
}

void arena_free(Arena *a)
{
    Block *b = a->head;
    while (b != NULL) {
        Block *next = b->next;
#ifdef NDRC_ASAN
        /* free of poisoned memory can itself be reported; hand the
           block back clean. */
        __asan_unpoison_memory_region(payload(b), b->cap);
#endif
        /* Block is a union member so its address equals the BlockHeader
           union's address. The offsetof recovers the original malloc pointer
           explicitly rather than relying on offsetof always being zero. */
        free((unsigned char *)b - offsetof(BlockHeader, block));
        b = next;
    }
    free(a);
}
