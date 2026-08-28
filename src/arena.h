/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/arena.h - bump allocator.
   Copyright (C) 2026 Dan Gibson.

   NDRC allocates everything from an arena and frees nothing until exit.
   A compiler runs once, builds a graph, emits bytes and terminates, so
   individual lifetimes never need tracking. This removes use-after-free
   and double-free as a class rather than defending against them. */
#ifndef NDRC_ARENA_H
#define NDRC_ARENA_H

#include <stddef.h>

/* NDRC_ASAN is defined exactly when AddressSanitizer is active. The
   two-branch check is required: gcc defines __SANITIZE_ADDRESS__ but
   clang only exposes __has_feature(address_sanitizer). Everything
   ASan-conditional in the project keys off NDRC_ASAN and nothing
   else, so it cannot half-enable. */
#if defined(__SANITIZE_ADDRESS__)
#define NDRC_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NDRC_ASAN 1
#endif
#endif

#ifdef NDRC_ASAN
#include <sanitizer/asan_interface.h>
#endif

typedef struct Arena Arena;

/* block_size is a hint for the size of each underlying block. A request
   larger than block_size gets its own dedicated block, so no allocation
   can fail for being too big. Passing 0 selects a 64 KiB default. */
Arena *arena_new(size_t block_size);

/* Returns memory aligned for any type. Never returns NULL: allocation
   failure calls abort(), because a compiler that cannot allocate cannot
   produce correct output and must not continue. A zero-size request
   returns a valid unique-enough pointer rather than NULL. */
void *arena_alloc(Arena *a, size_t n);

/* As arena_alloc, with the memory zeroed. */
void *arena_calloc(Arena *a, size_t n);

/* Copies a NUL-terminated string into the arena. */
char *arena_strdup(Arena *a, const char *s);

/* Copies at most n bytes, stopping early at an embedded NUL, and always
   NUL-terminates the result. */
char *arena_strndup(Arena *a, const char *s, size_t n);

/* Total bytes handed out, excluding per-block bookkeeping. Includes
   alignment padding, so it is greater than the sum of requested sizes.
   Used by tests and by the verbose diagnostic output. */
size_t arena_bytes_used(const Arena *a);

/* Releases every block. The arena pointer is invalid afterwards. */
void arena_free(Arena *a);

/* Marks [p, p+n) unaddressable under AddressSanitizer; no-op otherwise.
   Safe only for arena memory that will never be handed out again,
   which is every abandoned growth buffer, because the arena never
   reuses. ASan shadow has 8-byte granularity: a region end that is
   not 8-byte aligned leaves up to 7 tail bytes readable, so detection
   of tail-byte stale reads is best-effort - that is a granularity
   limit, not poisoning failing to work. The region start is covered
   by the arena's max_align_t alignment. */
static inline void arena_poison(Arena *a, void *p, size_t n)
{
    (void)a;
#ifdef NDRC_ASAN
    __asan_poison_memory_region(p, n);
#else
    (void)p; (void)n;
#endif
}

#endif /* NDRC_ARENA_H */
