/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/vec.h - growable pointer array.
   Copyright (C) 2026 Dan Gibson.

   Untyped substrate. NDRC's collections hold arena-allocated structs
   that are never copied and never freed, so a pointer vector covers
   every case; the VEC_DECLARE layer below wraps it with per-type
   compile-time checking, and production code uses that layer. */
#ifndef NDRC_VEC_H
#define NDRC_VEC_H

#include <stddef.h>
#include "arena.h"

typedef struct Vec Vec;

Vec *vec_new(Arena *a);

/* NULL is a legitimate item and is stored as such. */
void vec_push(Vec *v, void *item);

/* Returns NULL for an out-of-range index rather than aborting: callers
   that care check vec_len first, and a soft return keeps diagnostic and
   dump code simple. */
void *vec_at(const Vec *v, size_t i);

/* Replaces an existing element. An out-of-range index aborts, because
   unlike a read it cannot be meaningfully ignored. */
void vec_set(Vec *v, size_t i, void *item);

size_t vec_len(const Vec *v);

/* The arena all of this vector's memory comes from. */
Arena *vec_arena(const Vec *v);

/* Typed vector layer. VEC_DECLARE(name, elemtype) generates a
   distinct handle type Vec_##name and six wrappers over the untyped
   API, so pushing or reading the wrong element type is a compile
   error. Vec_##name is deliberately never defined: it exists only to
   give each element type a distinct pointer type, so sizeof or a
   dereference of the handle cannot compile either. The casts are
   sound because every object is created by vec_new and only ever
   accessed through Vec *, its one effective type - the handle is
   never dereferenced. elemtype is the full element type, e.g.
   Message * or const char * (two parameters because a single token
   cannot carry a qualifier and a star).

   One declaration per element type, in the element type's home
   header (file-local types declare in their .c). A duplicate
   VEC_DECLARE in one translation unit fails as function
   redefinition on the wrapper bodies. The untyped API below remains
   the substrate and stays legitimate for genuinely heterogeneous
   uses; such a use needs a comment saying why. */
/* A VEC_DECLARE in a .c generates all six wrappers into the main
   source file, where clang's -Wunused-function fires on the ones the
   caller does not use (in a header it does not). They are generated
   API surface, not dead code. */
#if defined(__GNUC__)
#define NDRC_VEC_FN static inline __attribute__((unused))
#else
#define NDRC_VEC_FN static inline
#endif

#define VEC_DECLARE(name, elemtype)                                    \
    typedef struct Vec_##name Vec_##name;  /* never defined */         \
    NDRC_VEC_FN Vec_##name *vec_new_##name(Arena *a)                   \
    { return (Vec_##name *)vec_new(a); }                               \
    NDRC_VEC_FN void vec_push_##name(Vec_##name *s, elemtype it)       \
    { vec_push((Vec *)s, (void *)it); }                                \
    NDRC_VEC_FN elemtype vec_at_##name(const Vec_##name *s,            \
                                       size_t i)                       \
    { return (elemtype)vec_at((const Vec *)s, i); }                    \
    NDRC_VEC_FN void vec_set_##name(Vec_##name *s, size_t i,           \
                                    elemtype it)                       \
    { vec_set((Vec *)s, i, (void *)it); }                              \
    NDRC_VEC_FN size_t vec_len_##name(const Vec_##name *s)             \
    { return vec_len((const Vec *)s); }                                \
    NDRC_VEC_FN Arena *vec_arena_##name(const Vec_##name *s)           \
    { return vec_arena((const Vec *)s); }

/* Vector of C strings - the one generic element type, homed here. */
VEC_DECLARE(CStr, const char *)

#endif /* NDRC_VEC_H */
